// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#include "watchdog.h"

#include "actions.h"

#include <chrono>
#include <format>

namespace Canine::Watchdog
{
	namespace
	{
		using namespace std::literals;
		using Clock = std::chrono::steady_clock;

		constexpr std::array kSnapshotsAfterLoad{ 0s, 5s, 15s };  // load snapshots, from when he's first loaded
		constexpr float      kJumpUnits = 600.0f;     // more than he can run in one tick
		constexpr float      kStillUnits = 25.0f;     // within this of where he stopped, he hasn't moved
		constexpr float      kFollowGap = 700.0f;     // a follower this far behind ought to be catching up
		constexpr float      kPlayerWalked = 400.0f;  // ...once the player has walked this far since he stopped
		constexpr auto       kStepEvaluate = 10s;     // stuck this long: re-evaluate his package, reset his graph
		constexpr auto       kStepReset3D = 18s;      // then disable and enable him (rebuilds his 3D and graph)
		constexpr auto       kStepMoveTo = 30s;       // then move him to the player
		// Switching his AI off and on (tai) didn't free him from the 2026-11-14 freeze; disable/enable did.

		enum class Step : int
		{
			kNone = 0,
			kEvaluate,
			kReset3D,
			kMoveTo,
		};

		constexpr std::string_view StepName(Step a_step)
		{
			switch (a_step) {
			case Step::kEvaluate:
				return "re-evaluating his package and resetting his graph";
			case Step::kReset3D:
				return "disabling and enabling him";
			case Step::kMoveTo:
				return "moving him to the player";
			default:
				return "nothing";
			}
		}

		RE::FormID                       g_dog = 0;
		std::optional<Clock::time_point> g_loadedAt;
		std::size_t                      g_snapshotsDone = 0;
		std::optional<RE::NiPoint3>      g_lastPos;
		RE::TESObjectCELL*               g_lastCell = nullptr;
		bool                             g_combatLast = false;

		// The stuck watch.
		std::optional<RE::NiPoint3>      g_stillAt;          // where he stopped
		std::optional<RE::NiPoint3>      g_playerAtStill;    // where the player was then
		std::optional<Clock::time_point> g_stillSince;       // since when he's been there while he ought to move
		Step                             g_step = Step::kNone;
		bool                             g_enablePending = false;  // disabled last tick: enabled again this one
		bool                             g_lockedAtLoad = false;   // his graph came out of the load locked (see AnimLocked)
		bool                             g_lockedLast = false;     // the lock as of last tick, to log changes
		bool                             g_jumped = false;         // something moved him this tick (not walking)
		std::string                      g_lastAnomaly;      // the last odd state logged (logged on change)

		std::string_view ProcessName(RE::PROCESS_TYPE a_type)
		{
			switch (a_type) {
			case RE::PROCESS_TYPE::kHigh:
				return "high";
			case RE::PROCESS_TYPE::kMiddleHigh:
				return "middle-high";
			case RE::PROCESS_TYPE::kMiddleLow:
				return "middle-low";
			case RE::PROCESS_TYPE::kLow:
				return "low";
			default:
				return "none";
			}
		}

		bool SameSpace(RE::TESObjectREFR* a_a, RE::TESObjectREFR* a_b)
		{
			const auto cellA = a_a->GetParentCell();
			const auto cellB = a_b->GetParentCell();
			if (!cellA || !cellB) {
				return false;
			}
			if (cellA->IsInteriorCell() || cellB->IsInteriorCell()) {
				return cellA == cellB;
			}
			return a_a->GetWorldspace() == a_b->GetWorldspace();
		}

		RE::PACKAGE_TYPE PackageType(RE::Actor* a_dog)
		{
			const auto package = a_dog->GetCurrentPackage();
			return package ? package->packData.packType.get() : RE::PACKAGE_TYPE::kNone;
		}

		// Packages that hold him in place on purpose (a wait, a sandbox, sleeping, sitting somewhere). Template
		// packages (type 18, most of SeverActions') can't be told apart this way: see GraphSaysMoving.
		bool HoldsHimStill(RE::PACKAGE_TYPE a_type)
		{
			using T = RE::PACKAGE_TYPE;
			switch (a_type) {
			case T::kDoNothing:
			case T::kSandbox:
			case T::kSleep:
			case T::kEat:
			case T::kUseItemAt:
			case T::kGuard:
			case T::kPatrol:
			case T::kDialogue:
			case T::kInGameDialogue:
			case T::kActivate:
				return true;
			default:
				return false;
			}
		}

		// Vanilla package templates (Skyrim.esm) that keep him in one spot or area. A template-based package reports the
		// generic type 18, so it's judged by the template it was built from (2026-11-15: Sever's Hearth's
		// Severs_CampingPackage is built on Sandbox, and his camp idles lock the graph while he sits or sleeps).
		// Follow and travel templates aren't here: on those he should keep up.
		constexpr std::array<RE::FormID, 22> kStillTemplates{
			0x01C254,  // Sandbox
			0x06434E,  // SandboxLockOnArrival
			0x068B86,  // SandboxMultiLocation
			0x0604EE,  // SandboxWorking
			0x06C873,  // SandboxWorkingMultiLocation
			0x06C872,  // SandboxAndKeepEyeOn
			0x05FAD4,  // SandboxAndGuard
			0x019717,  // Sleep
			0x019715,  // Sit
			0x019714,  // Eat
			0x0A9277,  // SitTarget
			0x0DD745,  // SleepAndGuard
			0x0283F0,  // UseIdleMarker
			0x0654E2,  // DoNothing
			0x0503D0,  // HoldPosition
			0x0EE526,  // HoldPositionWithTravel16
			0x02A85F,  // HoldPositionWithTravel512
			0x10FAAF,  // HoldPositionWithTravel1024
			0x082571,  // StayAtCurrentLocation
			0x01C9FF,  // GuardPost
			0x0C4D59,  // SceneActionWaitTemplate
			0x0B7869,  // ForceGreetWaitSitting
		};

		// His current package holds him still: by its own type, or (a template-based package) by its template's.
		bool PackageHoldsHimStill(RE::Actor* a_dog)
		{
			auto package = a_dog->GetCurrentPackage();
			if (!package) {
				return false;
			}
			if (HoldsHimStill(package->packData.packType.get())) {
				return true;
			}
			for (int depth = 0; depth < 4 && package; ++depth) {
				const auto custom = skyrim_cast<RE::TESCustomPackageData*>(package->data);
				package = custom ? custom->templateParent : nullptr;
				if (package && std::ranges::find(kStillTemplates, package->GetFormID()) != kStillTemplates.end()) {
					return true;
				}
			}
			return false;
		}

		// His graph thinks he's walking or running (a frozen walk keeps its speed); a dog told to wait stands at 0.
		bool GraphSaysMoving(RE::Actor* a_dog)
		{
			float speed = 0.0f;
			return a_dog->GetGraphVariableFloat("Speed", speed) && speed > 1.0f;
		}

		// The 2026-11-14 freeze: his graph left animation-driven with head tracking off (what an idle sets while it
		// plays) after the idle itself was gone. Packages can't move an animation-driven actor, and nothing showed.
		bool AnimLocked(RE::Actor* a_dog)
		{
			bool driven = false;
			bool noHeadTrack = false;
			return a_dog->GetGraphVariableBool("bAnimationDriven", driven) && driven &&
			       a_dog->GetGraphVariableBool("bDisableHeadTrack", noHeadTrack) && noHeadTrack;
		}

		// Odd states worth a log line whenever they change: each would leave him frozen.
		std::string Anomalies(RE::Actor* a_dog)
		{
			std::string out;
			const auto add = [&](std::string_view a_text) {
				if (!out.empty()) {
					out += ", ";
				}
				out += a_text;
			};
			if (!a_dog->IsAIEnabled()) {
				add("AI off");
			}
			if (const auto level = a_dog->GetProcessLevel(); level != RE::PROCESS_TYPE::kHigh) {
				add(std::format("process {}", ProcessName(level)));
			}
			const auto& data = a_dog->GetActorRuntimeData();
			if (!data.boolFlags.all(RE::Actor::BOOL_FLAGS::kShouldAnimGraphUpdate)) {
				add("graph not updating");
			}
			if (data.boolFlags.all(RE::Actor::BOOL_FLAGS::kMovementBlocked)) {
				add("movement blocked");
			}
			if (data.boolBits.all(RE::Actor::BOOL_BITS::kParalyzed)) {
				add("paralyzed");
			}
			if (!a_dog->GetCharController()) {
				add("no character controller");
			}
			return out;
		}

		void Snapshot(RE::Actor* a_dog, RE::PlayerCharacter* a_player, std::string_view a_label)
		{
			logs::info("Watch [{}]: {}", a_label, Describe(a_dog, a_player));
		}

		void ResetStill()
		{
			g_stillAt.reset();
			g_playerAtStill.reset();
			g_stillSince.reset();
		}

		void UpdateStuck(RE::Actor* a_dog, RE::PlayerCharacter* a_player, const RE::NiPoint3& a_pos, Clock::time_point a_now)
		{
			// Teleported (a follower catch-up, not us): he didn't walk, so it says nothing about the freeze. Watch
			// afresh from the new spot, keeping how long he's been stuck and the step reached.
			if (g_jumped) {
				g_stillAt = a_pos;
				g_playerAtStill = a_player->GetPosition();
			}
			// Moved: if a step was taken, say which one got him going.
			if (g_stillAt && a_pos.GetDistance(*g_stillAt) > kStillUnits) {
				if (g_step != Step::kNone) {
					logs::info("Watch: he's moving again after {} ({:.0f} s stuck)", StepName(g_step),
						g_stillSince ? std::chrono::duration<double>(a_now - *g_stillSince).count() : 0.0);
					Snapshot(a_dog, a_player, "moving again");
				}
				g_step = Step::kNone;
				ResetStill();
			}
			if (!g_stillAt) {
				g_stillAt = a_pos;
				g_playerAtStill = a_player->GetPosition();
			}

			// Should he be moving? Play/Sniff/Fish running him, or the player walked off and left him behind.
			const float distance = a_pos.GetDistance(a_player->GetPosition());
			const bool  intentional = Actions::NapOn() || Actions::PoseOn() || Actions::Downed() || a_dog->IsInCombat() ||
				a_dog->IsDead() || a_player->IsInCombat();
			// Left behind counts only when he's frozen, not waiting: his graph is locked animation-driven (an idle
			// can't keep him that long that far from the player), or the player has walked off while his graph says
			// he's moving or his state is odd.
			const bool locked = AnimLocked(a_dog);
			const bool walkedOff = g_playerAtStill && a_player->GetPosition().GetDistance(*g_playerAtStill) > kPlayerWalked;
			// A package meant to hold him (a wait, a sandbox such as a camp, sleeping, sitting) keeps its locked idles
			// legitimately, even across a load: only the left-behind and lock-at-load rules are off, a run still counts.
			const bool still = PackageHoldsHimStill(a_dog);
			const bool shouldMove = !intentional &&
				((Actions::Roaming() && !Actions::RoamHoldsStill()) ||
					(locked && g_lockedAtLoad && !still) ||  // no idle legitimately survives a load, unless his package holds him
					(distance > kFollowGap && !still &&
						(locked || (walkedOff && (GraphSaysMoving(a_dog) || !Anomalies(a_dog).empty())))));
			if (!shouldMove) {
				g_stillSince.reset();
				return;
			}
			if (!g_stillSince) {
				g_stillSince = a_now;
				return;
			}
			const auto stuck = a_now - *g_stillSince;
			if (g_step < Step::kEvaluate && stuck >= kStepEvaluate) {
				g_step = Step::kEvaluate;
				logs::info("Watch: he hasn't moved for {:.0f} s though he should ({}{}); {}",
					std::chrono::duration<double>(stuck).count(),
					Actions::Roaming() ? "a roam is running him" : locked ? "left behind" : "the player has walked off",
					locked ? ", graph locked animation-driven with head tracking off" : "", StepName(g_step));
				Snapshot(a_dog, a_player, "stuck");
				Actions::DumpGraph(a_dog, "stuck");
				a_dog->NotifyAnimationGraph("IdleForceDefaultState");
				a_dog->EvaluatePackage(true, true);
			} else if (g_step < Step::kReset3D && stuck >= kStepReset3D) {
				g_step = Step::kReset3D;
				logs::info("Watch: still stuck; {}", StepName(g_step));
				a_dog->Disable();
				g_enablePending = true;  // enabled again next tick
			} else if (g_step < Step::kMoveTo && stuck >= kStepMoveTo && distance > kFollowGap) {
				g_step = Step::kMoveTo;
				logs::info("Watch: still stuck; {}", StepName(g_step));
				a_dog->MoveTo(a_player);
				ResetStill();  // the move itself isn't him walking: watch afresh from the new spot
				g_stillSince = a_now - kStepMoveTo;  // ...but keep the step, so a further freeze reports itself
				Snapshot(a_dog, a_player, "moved to the player");
			}
		}
	}

	namespace
	{
		bool GraphBool(RE::Actor* a_dog, const char* a_name)
		{
			bool value = false;
			return a_dog->GetGraphVariableBool(a_name, value) && value;
		}
	}

	std::string Describe(RE::Actor* a_dog, RE::PlayerCharacter* a_player)
	{
		if (!a_dog) {
			return "no dog";
		}
		const auto pos = a_dog->GetPosition();
		const auto cell = a_dog->GetParentCell();
		std::string where = cell ? (cell->IsInteriorCell() ? std::format("interior {:08X}", cell->GetFormID()) :
		                                                     std::format("exterior {:08X}", cell->GetFormID())) :
		                           "no cell"s;
		std::string distance = a_player && SameSpace(a_dog, a_player) ? std::format("{:.0f}", pos.GetDistance(a_player->GetPosition())) :
		                                                                 "elsewhere"s;
		const auto  package = a_dog->GetCurrentPackage();
		std::string packageText = "none"s;
		if (package) {
			const char* edid = package->GetFormEditorID();
			packageText = std::format("{:08X}{}{} type {}{}", package->GetFormID(), edid && *edid ? " " : "", edid ? edid : "",
				static_cast<int>(package->packData.packType.get()), PackageHoldsHimStill(a_dog) ? " (holds him in place)" : "");
		}
		// Follower status, for camps and other mods that take him off following (Skyrim.esm CurrentFollowerFaction).
		const auto followers = RE::TESForm::LookupByID<RE::TESFaction>(0x05C84E);
		packageText += std::format(" | teammate {}, follower faction {}", a_dog->IsPlayerTeammate() ? "yes" : "no",
			followers && a_dog->GetFactionRank(followers, false) >= 0 ? "yes" : "no");
		const auto  state = a_dog->AsActorState();
		const auto& data = a_dog->GetActorRuntimeData();
		float       speed = 0.0f;
		a_dog->GetGraphVariableFloat("Speed", speed);
		const auto anomalies = Anomalies(a_dog);
		return std::format(
			"at ({:.0f}, {:.0f}, {:.0f}) {} {} units from the player, heading {:.0f} | AI {}, process {}, graph update {}, 3D {}, "
			"controller {} | package {} | sit {}, knock {}, life {}, swimming {}, speed {:.0f}, flags {:08X}/{:08X} | "
			"animation-driven {}, head track off {} | nap {}, pose {}, roam {}{}",
			pos.x, pos.y, pos.z, where, distance, a_dog->GetAngleZ() * 57.2958f, a_dog->IsAIEnabled() ? "on" : "OFF",
			ProcessName(a_dog->GetProcessLevel()), data.boolFlags.all(RE::Actor::BOOL_FLAGS::kShouldAnimGraphUpdate) ? "yes" : "NO",
			a_dog->Is3DLoaded() ? "yes" : "no", a_dog->GetCharController() ? "yes" : "NO", packageText,
			state ? static_cast<int>(state->GetSitSleepState()) : -1, state ? static_cast<int>(state->GetKnockState()) : -1,
			state ? static_cast<int>(state->GetLifeState()) : -1, data.boolBits.all(RE::Actor::BOOL_BITS::kSwimming), speed,
			data.boolFlags.underlying(), data.boolBits.underlying(), GraphBool(a_dog, "bAnimationDriven"),
			GraphBool(a_dog, "bDisableHeadTrack"), Actions::NapOn(), Actions::PoseOn(), Actions::Roaming(),
			anomalies.empty() ? ""s : " | ODD: " + anomalies);
	}

	void Update(RE::Actor* a_dog, RE::PlayerCharacter* a_player)
	{
		if (!a_dog || !a_player) {
			return;
		}
		if (a_dog->GetFormID() != g_dog) {  // a new dog (or a respawned one): start over
			ResetSession();
			g_dog = a_dog->GetFormID();
		}
		if (g_enablePending) {
			g_enablePending = false;
			if (a_dog->IsDisabled()) {
				a_dog->Enable(false);
			}
			logs::info("Watch: enabled him again");
			g_lastPos.reset();  // his 3D comes back where he stood: not a jump
			return;
		}
		if (!a_dog->Is3DLoaded()) {
			g_lastPos.reset();
			ResetStill();
			return;
		}
		if (const auto ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused()) {
			return;
		}
		const auto now = Clock::now();
		const auto pos = a_dog->GetPosition();
		const auto cell = a_dog->GetParentCell();

		// Load snapshots.
		if (!g_loadedAt) {
			g_loadedAt = now;
		}
		if (g_snapshotsDone == 0) {
			g_lockedAtLoad = AnimLocked(a_dog);
		}
		if (g_snapshotsDone < kSnapshotsAfterLoad.size() && now - *g_loadedAt >= kSnapshotsAfterLoad[g_snapshotsDone]) {
			const auto after = std::chrono::duration_cast<std::chrono::seconds>(kSnapshotsAfterLoad[g_snapshotsDone]).count();
			Snapshot(a_dog, a_player, after == 0 ? "loaded"s : std::format("{} s after load", after));
			++g_snapshotsDone;
		}

		// Jumps: a move in one tick he couldn't have run. Frost Step (in combat) is ours.
		g_jumped = false;
		if (g_lastPos && cell == g_lastCell && pos.GetDistance(*g_lastPos) > kJumpUnits && !a_dog->IsInCombat() && !g_combatLast &&
			g_step != Step::kMoveTo) {
			logs::info("Watch: he jumped {:.0f} units in one tick, from ({:.0f}, {:.0f}, {:.0f})", pos.GetDistance(*g_lastPos),
				g_lastPos->x, g_lastPos->y, g_lastPos->z);
			Snapshot(a_dog, a_player, "after the jump");
			g_jumped = true;
		} else if (g_lastPos && cell != g_lastCell && cell && g_lastCell && (cell->IsInteriorCell() || g_lastCell->IsInteriorCell())) {
			logs::info("Watch: he changed cell {:08X} -> {:08X}", g_lastCell->GetFormID(), cell->GetFormID());
		}
		g_lastPos = pos;
		g_lastCell = cell;
		g_combatLast = a_dog->IsInCombat();

		// The graph lock: logged when it comes on and when it clears (idles set it briefly; a long one is the freeze).
		if (const bool locked = AnimLocked(a_dog); locked != g_lockedLast) {
			logs::info("Watch: graph lock {}{}", locked ? "on (animation-driven, head tracking off)" : "off",
				!locked && g_lockedAtLoad ? " - the lock he loaded with has cleared" : "");
			g_lockedLast = locked;
			if (!locked) {
				g_lockedAtLoad = false;
			}
		}

		// Odd states: logged whenever they change (and when they clear).
		if (auto odd = Anomalies(a_dog); odd != g_lastAnomaly) {
			if (odd.empty()) {
				logs::info("Watch: his state is normal again (was: {})", g_lastAnomaly);
			} else if (!(odd == "AI off" && Actions::NapOn())) {  // a settled nap pauses his AI on purpose
				logs::info("Watch: odd state: {}", odd);
				Snapshot(a_dog, a_player, "odd state");
			}
			g_lastAnomaly = std::move(odd);
		}

		if (SameSpace(a_dog, a_player)) {
			UpdateStuck(a_dog, a_player, pos, now);
		} else {
			ResetStill();
		}
	}

	void ResetSession()
	{
		g_dog = 0;
		g_loadedAt.reset();
		g_snapshotsDone = 0;
		g_lastPos.reset();
		g_lastCell = nullptr;
		g_combatLast = false;
		ResetStill();
		g_step = Step::kNone;
		g_enablePending = false;
		g_lockedAtLoad = false;
		g_lockedLast = false;
		g_jumped = false;
		g_lastAnomaly.clear();
	}
}
