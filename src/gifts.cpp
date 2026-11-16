// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#include "gifts.h"

#include "actions.h"
#include "bridge.h"
#include "combat.h"
#include "notify.h"
#include "runes.h"
#include "soundcues.h"

#include <array>
#include <chrono>
#include <cmath>
#include <numbers>
#include <mutex>
#include <thread>

namespace Canine::Gifts
{
	namespace
	{
		constexpr auto       kPlugin = "SN_Dog.esp"sv;
		constexpr RE::FormID kHjalti = 0x800;
		constexpr RE::FormID kFrostBitePerk = 0x817;
		constexpr RE::FormID kFrostBiteSpell = 0x816;
		constexpr int        kTiers = 7;

		// What each tier gives, in order.
		enum Tier : int
		{
			kFrostBite1 = 1,
			kPerseverance = 2,
			kFrostStep = 3,
			kFrostBite2 = 4,
			kQuicker = 5,
			kFrostBite3 = 6,
			kFrostHowl = 7,
		};

		// Frost Bite I / II / III: frost damage (health and stamina), and the slow (percent, seconds).
		struct Frost
		{
			float         damage;
			float         slow;
			std::uint32_t slowFor;
		};
		constexpr std::array<Frost, 3> kFrost{ { { 10.0f, 30.0f, 3 }, { 18.0f, 40.0f, 3 }, { 28.0f, 50.0f, 4 } } };

		using Clock = std::chrono::steady_clock;

		int                     g_frostSet = 0;  // game thread: the Frost Bite level written to the spell
		std::string             g_summary;       // game thread: the last gift summary logged (once per load and per change)

		// Game thread: one line with every gift and whether he has it, logged after a load and whenever it changes
		// (a new unlock, or a gift switched on or off in the settings). The gifts themselves only log when they fire.
		void LogSummary(std::string a_line)
		{
			if (a_line != g_summary) {
				logs::info("{}", a_line);
				g_summary = std::move(a_line);
			}
		}

		std::string Summary(int a_souls, int a_per, int a_tier, const Settings& a_s)
		{
			static constexpr std::array<std::string_view, kTiers> kNames{ "Frost Bite I", "Perseverance", "Frost Step", "Frost Bite II",
				"Quicker (shorter cooldowns)", "Frost Bite III", "Frost Howl" };
			std::string have;
			std::string locked;
			for (int t = 1; t <= kTiers; ++t) {
				std::string name{ kNames[static_cast<std::size_t>(t - 1)] };
				if ((t == kFrostStep && !a_s.frostStep) || (t == kFrostHowl && !a_s.frostHowl)) {
					name += " (off in settings)";
				}
				auto& list = t <= a_tier ? have : locked;
				list += list.empty() ? name : ", " + name;
			}
			return std::format("Gifts: {} souls ({} per unlock), step {}/{} | has: {} | locked: {}", a_souls, a_per, a_tier, kTiers,
				have.empty() ? "none" : have, locked.empty() ? "none" : locked);
		}

		// Perseverance (game thread): badly hurt, the runes knit his wounds over a few seconds, then need time to recover.
		constexpr float                  kPerseverAt = 0.2f;  // a fifth of his health or less (or down)
		constexpr auto                   kHealOver = 5s;
		constexpr RE::FormID             kHealGlow = 0x04E220;  // Skyrim.esm HealMysticFXS: a pale blue-white healing glow
		std::optional<Clock::time_point> g_perseverReady;  // spent until then
		std::optional<Clock::time_point> g_healUntil;
		float                            g_healPerSec = 0.0f;
		float                            g_healLeft = 0.0f;  // what Perseverance still owes him
		std::optional<Clock::time_point> g_lastTick;
		std::atomic<RE::FormID> g_dogId{ 0 };
		std::mutex              g_factLock;
		std::string             g_fact;

		template <class T>
		T* Lookup(RE::FormID a_id)
		{
			const auto data = RE::TESDataHandler::GetSingleton();
			return data ? data->LookupForm<T>(a_id, kPlugin) : nullptr;
		}

		int FrostLevel(int a_tier)
		{
			return a_tier >= kFrostBite3 ? 3 : a_tier >= kFrostBite2 ? 2 : a_tier >= kFrostBite1 ? 1 : 0;
		}

		// Game thread: the frost spell's strength for his level of Frost Bite, and the perk on him once he has it.
		void UpdateFrostBite(RE::Actor* a_dog, RE::TESNPC* a_base, int a_level)
		{
			const auto spell = Lookup<RE::SpellItem>(kFrostBiteSpell);
			const auto perk = Lookup<RE::BGSPerk>(kFrostBitePerk);
			if (!spell || !perk || spell->effects.size() < 2) {
				return;
			}
			if (a_level != g_frostSet) {
				// No level (a save without souls, or a lowered debug count): the perk can't come off, so the bite
				// carries nothing.
				const Frost frost = a_level > 0 ? kFrost[static_cast<std::size_t>(a_level - 1)] : Frost{ 0.0f, 0.0f, 0 };
				spell->effects[0]->effectItem.magnitude = frost.damage;
				spell->effects[1]->effectItem.magnitude = frost.slow;
				spell->effects[1]->effectItem.duration = frost.slowFor;
				g_frostSet = a_level;
				logs::info("Gifts: Frost Bite {} (frost {:.0f}, slowed {:.0f}% for {} s)", a_level, frost.damage, frost.slow, frost.slowFor);
			}
			if (a_level > 0 && !a_base->GetPerkIndex(perk) && a_base->AddPerk(perk, 1)) {
				a_dog->ApplyPerksFromBase();
				logs::info("Gifts: Frost Bite perk on him");
			}
		}

		void UpdatePerseverance(RE::Actor* a_dog, const Settings& a_s, int a_tier)
		{
			const auto now = Clock::now();
			const float dt = g_lastTick ? std::chrono::duration<float>(now - *g_lastTick).count() : 0.0f;
			g_lastTick = now;
			const auto values = a_dog->AsActorValueOwner();
			if (!values || a_dog->IsDead()) {
				g_healUntil.reset();
				return;
			}
			if (g_healUntil) {
				// Paid by elapsed time, and whatever is left on the last tick, so a slow tick never loses any of it.
				const float heal = now < *g_healUntil ? (std::min)(g_healPerSec * dt, g_healLeft) : g_healLeft;
				if (heal > 0.0f) {
					values->RestoreActorValue(RE::ActorValue::kHealth, heal);
					g_healLeft -= heal;
				}
				if (now >= *g_healUntil || g_healLeft <= 0.0f) {
					g_healUntil.reset();
					g_healLeft = 0.0f;
					logs::info("Gifts: Perseverance done (health {:.0f})", values->GetActorValue(RE::ActorValue::kHealth));
				}
				return;
			}
			if (a_tier < kPerseverance || (g_perseverReady && now < *g_perseverReady)) {
				return;
			}
			const float max = values->GetPermanentActorValue(RE::ActorValue::kHealth);
			const float health = values->GetActorValue(RE::ActorValue::kHealth);
			const bool  down = a_dog->AsActorState()->IsBleedingOut();
			if (max <= 0.0f || !(down || (a_dog->IsInCombat() && health <= max * kPerseverAt))) {
				return;
			}
			const float total = max * static_cast<float>(a_s.perseverHeal) / 100.0f;
			g_healPerSec = total / std::chrono::duration<float>(kHealOver).count();
			g_healUntil = now + kHealOver;
			g_healLeft = total;
			const double cooldown = a_s.perseverCooldown * (a_tier >= kQuicker ? 0.6 : 1.0);
			g_perseverReady = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(cooldown));
			SoundCues::Play(a_dog, SoundCues::Cue::kPerseverance);
			Runes::Light(Runes::Flare::kGift);
			// A pale healing glow on him for as long as the runes are knitting his wounds.
			if (const auto glow = RE::TESForm::LookupByID<RE::TESEffectShader>(kHealGlow)) {
				a_dog->ApplyEffectShader(glow, std::chrono::duration<float>(kHealOver).count());
			}
			logs::info("Gifts: Perseverance ({}health {:.0f}/{:.0f}): {:.0f} back over 5 s, ready again in {:.0f} s", down ? "down, " : "", health, max,
				total, cooldown);
			// Kept as an event for later talk, not narrated: the runes' light is the moment.
			const char* name = a_dog->GetDisplayFullName();
			Bridge::PostEvent(std::format("Badly hurt, {} found his feet again as the runes in his coat kindled and his wounds began to knit.",
								  name && *name ? name : "the dog"),
				a_dog->GetFormID(), 0, false);
		}

		// Frost Step (game thread): behind a far foe he can see, or to the player when badly hurt.
		constexpr float                  kStepNear = 600.0f;      // closer than this he just runs
		constexpr float                  kStepFar = 2500.0f;
		constexpr float                  kStepRise = 400.0f;      // a foe this far above or below is out of reach
		constexpr float                  kEscapeAt = 0.3f;        // this share of his health or less
		constexpr float                  kEscapeNear = 500.0f;    // already this close to the player: no point
		constexpr float                  kEscapeFar = 6000.0f;
		constexpr float                  kChest = 60.0f;          // height of the clearance ray
		constexpr float                  kRoom = 40.0f;           // the clearance ray runs this far past the spot
		constexpr float                  kFloorSlack = 100.0f;    // the ground may differ this much from who he lands by
		constexpr auto                   kStepRetry = 3s;         // no landing found, or just stepped
		constexpr RE::FormID             kStepMist = 0x0435A3;    // Skyrim.esm FrostShortFXShader (Frost Howl's burst)
		constexpr float                  kStepMistFor = 2.0f;
		constexpr RE::FormID             kStepSparks = 0x056AC9;  // Skyrim.esm ShieldSpellFXS: frosty sparks round him, short
		constexpr float                  kStepSparksFor = 2.0f;
		// Left where he vanishes, so the eye sees something break apart rather than a skip: the frost crossbow bolt's
		// impact burst (no damage, a small push, an icicle-shatter sound, a brief frosty tint).
		constexpr RE::FormID             kStepBurst = 0x010D91;   // Dawnguard.esm DLC1BoltExplosionFrost02
		constexpr auto                   kStepBurstPlugin = "Dawnguard.esm"sv;
		std::optional<Clock::time_point> g_stepReady;
		std::optional<Clock::time_point> g_escapeReady;
		std::optional<Clock::time_point> g_stepPause;

		// Where a ray from a_from to a_to first meets the world, as a share of its length; nothing if it's clear.
		std::optional<float> Ray(RE::bhkWorld* a_world, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
		{
			const float     scale = RE::bhkWorld::GetWorldScale();
			RE::bhkPickData pick;
			pick.rayInput.from = RE::hkVector4(a_from * scale);
			pick.rayInput.to = RE::hkVector4(a_to * scale);
			pick.ray = RE::hkVector4((a_to - a_from) * scale);
			pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);
			RE::BSReadLockGuard lock(a_world->worldLock);
			a_world->PickObject(pick);
			return pick.rayOutput.HasHit() ? std::optional<float>(pick.rayOutput.hitFraction) : std::nullopt;
		}

		// The spot on the ground at a_spot he could land on beside a_by: a clear line from them to it, solid ground
		// about their height, room to stand, and not under water.
		std::optional<RE::NiPoint3> Landing(RE::Actor* a_by, RE::NiPoint3 a_spot)
		{
			const auto cell = a_by->GetParentCell();
			const auto world = cell ? cell->GetbhkWorld() : nullptr;
			if (!world) {
				return std::nullopt;
			}
			const auto  by = a_by->GetPosition();
			const float ground = by.z;
			const RE::NiPoint3 chest{ by.x, by.y, by.z + kChest };
			RE::NiPoint3       out{ a_spot.x - by.x, a_spot.y - by.y, 0.0f };
			const float        length = out.Length();
			if (length < 1.0f) {
				return std::nullopt;
			}
			out *= (length + kRoom) / length;
			if (Ray(world, chest, { by.x + out.x, by.y + out.y, chest.z })) {
				return std::nullopt;  // a wall or someone in the way
			}
			const RE::NiPoint3 top{ a_spot.x, a_spot.y, ground + kChest + 20.0f };
			const RE::NiPoint3 bottom{ a_spot.x, a_spot.y, ground - 150.0f };
			const auto         hit = Ray(world, top, bottom);
			if (!hit) {
				return std::nullopt;  // nothing to stand on
			}
			RE::NiPoint3 spot{ a_spot.x, a_spot.y, top.z + (bottom.z - top.z) * *hit };
			if (std::abs(spot.z - ground) > kFloorSlack || cell->GetExteriorWaterHeight() > spot.z) {
				return std::nullopt;
			}
			spot.z += 5.0f;
			if (Ray(world, spot, { spot.x, spot.y, spot.z + kChest })) {
				return std::nullopt;  // no headroom
			}
			return spot;
		}

		// Standing on the ground, free to move or be landed beside.
		bool Grounded(RE::Actor* a_actor)
		{
			const auto state = a_actor->AsActorState();
			return a_actor->Is3DLoaded() && !a_actor->IsDead() && !state->IsFlying() && !state->IsSwimming() && !a_actor->IsInMidair() &&
			       !a_actor->IsOnMount() && !a_actor->IsInKillMove();
		}

		bool SamePlace(RE::Actor* a_one, RE::Actor* a_other)
		{
			const auto one = a_one->GetParentCell();
			if (!one || !a_other->GetParentCell()) {
				return false;
			}
			if (one == a_other->GetParentCell()) {
				return true;
			}
			const auto world = a_one->GetWorldspace();
			return world && world == a_other->GetWorldspace();
		}

		// The first of a_by's back, left and right (or just the sides) he can land on, a_away out from them.
		std::optional<RE::NiPoint3> FindLanding(RE::Actor* a_by, float a_away, bool a_behindFirst)
		{
			const float  heading = a_by->GetAngleZ();
			const auto   by = a_by->GetPosition();
			const float  pi = std::numbers::pi_v<float>;
			const std::array<float, 3> around = a_behindFirst ? std::array{ pi, pi / 2.0f, -pi / 2.0f } : std::array{ pi / 2.0f, -pi / 2.0f, pi };
			for (const float turn : around) {
				const float angle = heading + turn;
				if (const auto spot = Landing(a_by, { by.x + std::sin(angle) * a_away, by.y + std::cos(angle) * a_away, by.z })) {
					return spot;
				}
			}
			return std::nullopt;
		}

		std::string Name(RE::TESObjectREFR* a_ref);

		// He crosses to the spot, facing a_face. Nothing crosses the space between: he's gone and he's there.
		void StepTo(RE::Actor* a_dog, const RE::NiPoint3& a_spot, RE::Actor* a_face)
		{
			SoundCues::Play(a_dog, SoundCues::Cue::kFrostStep);  // as he goes; the sound travels with him
			// A burst of frost where he stood.
			const auto data = RE::TESDataHandler::GetSingleton();
			if (const auto burst = data ? data->LookupForm<RE::BGSExplosion>(kStepBurst, kStepBurstPlugin) : nullptr) {
				a_dog->PlaceObjectAtMe(burst, false);
			}
			// Frosty sparks start on him as he goes and carry through to where he comes out (a shader travels with him).
			if (const auto sparks = RE::TESForm::LookupByID<RE::TESEffectShader>(kStepSparks)) {
				a_dog->ApplyEffectShader(sparks, kStepSparksFor);
			}
			a_dog->SetPosition(a_spot, true);
			const auto face = a_face->GetPosition();
			a_dog->SetHeading(std::atan2(face.x - a_spot.x, face.y - a_spot.y));
			Runes::Light(Runes::Flare::kGift);
		}

		void UpdateFrostStep(RE::Actor* a_dog, const Settings& a_s, int a_tier)
		{
			if (!a_s.frostStep || a_tier < kFrostStep || !a_dog->IsInCombat()) {
				return;
			}
			const auto now = Clock::now();
			if (g_stepPause && now < *g_stepPause) {
				return;
			}
			const auto state = a_dog->AsActorState();
			if (!Grounded(a_dog) || state->IsBleedingOut() || a_dog->IsInRagdollState() || state->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal ||
				state->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal) {
				return;
			}
			const double quicker = a_tier >= kQuicker ? 0.6 : 1.0;
			auto after = [&](double a_seconds) {
				return now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(a_seconds * quicker));
			};
			const auto values = a_dog->AsActorValueOwner();
			const float max = values->GetPermanentActorValue(RE::ActorValue::kHealth);
			const float health = values->GetActorValue(RE::ActorValue::kHealth);

			// Badly hurt: to the player's side.
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (player && max > 0.0f && health <= max * kEscapeAt && (!g_escapeReady || now >= *g_escapeReady) && Grounded(player) && SamePlace(a_dog, player)) {
				const float distance = a_dog->GetPosition().GetDistance(player->GetPosition());
				if (distance >= kEscapeNear && distance <= kEscapeFar) {
					g_stepPause = now + kStepRetry;
					if (const auto spot = FindLanding(player, 130.0f, false)) {
						StepTo(a_dog, *spot, player);
						Combat::DropOrder("he escaped");
						g_escapeReady = after(a_s.frostStepEscapeCooldown);
						logs::info("Gifts: Frost Step to {} from {:.0f} away (health {:.0f}/{:.0f}), ready again in {:.0f} s", Name(player), distance, health, max,
							a_s.frostStepEscapeCooldown * quicker);
						Actions::WhineSound(a_dog);  // heard over a fight, where the note is easy to miss
						Notify::Show(std::format("{} is badly hurt and has stepped back to you", Name(a_dog)));
						Bridge::PostEvent(std::format("Badly hurt, {} vanished in a breath of frost and stood at {}'s side.", Name(a_dog), Name(player)),
							a_dog->GetFormID(), player->GetFormID(), false);
					} else if (a_s.logCombat) {
						logs::info("Gifts: Frost Step to {}: nowhere to land", Name(player));
					}
					return;
				}
			}

			// A foe he can see, too far off to reach quickly: behind them.
			if (g_stepReady && now < *g_stepReady) {
				return;
			}
			const auto aim = a_dog->GetActorRuntimeData().currentCombatTarget.get();
			const auto target = aim.get();
			if (!target || Actions::Hurt(a_dog) || !Combat::Engaged(a_dog, target) || !Grounded(target) || !SamePlace(a_dog, target)) {
				return;
			}
			const auto  from = a_dog->GetPosition();
			const auto  at = target->GetPosition();
			const float distance = from.GetDistance(at);
			bool        unused = false;
			if (distance < kStepNear || distance > kStepFar || std::abs(from.z - at.z) > kStepRise || !a_dog->HasLineOfSight(target, unused)) {
				return;
			}
			g_stepPause = now + kStepRetry;
			// Clear of their body, however big they are.
			const auto  bound = target->GetBoundMax();
			const float away = std::clamp((std::max)(bound.x, bound.y) * target->GetScale() + 80.0f, 110.0f, 400.0f);
			if (const auto spot = FindLanding(target, away, true)) {
				StepTo(a_dog, *spot, target);
				g_stepReady = after(a_s.frostStepCooldown);
				logs::info("Gifts: Frost Step behind {} from {:.0f} away, ready again in {:.0f} s", Name(target), distance, a_s.frostStepCooldown * quicker);
				Bridge::PostEvent(std::format("{} vanished in a breath of frost and came out behind {}.", Name(a_dog), Name(target)), a_dog->GetFormID(),
					target->GetFormID(), false);
			} else if (a_s.logCombat) {
				logs::info("Gifts: Frost Step behind {}: nowhere to land", Name(target));
			}
		}

		// Frost Howl (game thread): swarmed, or badly hurt with a foe close, he howls and every foe around him is knocked
		// down where they stand. Papyrus ObjectReference.PushActorAway, called on him once per foe we chose; near-zero
		// force drops them in place.
		constexpr float                  kHowlRange = 300.0f;
		constexpr std::size_t            kHowlCrowd = 2;
		constexpr auto                   kHowlCheckAfter = std::chrono::milliseconds(1000);
		std::optional<Clock::time_point> g_howlReady;

		void PushAway(RE::Actor* a_dog, RE::Actor* a_target, float a_force)
		{
			const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			if (!vm) {
				return;
			}
			const auto handle = vm->handlePolicy->GetHandleForObject(RE::Actor::FORMTYPE, a_dog);
			for (const auto* type : { "Actor", "ObjectReference" }) {
				RE::Actor* target = a_target;
				float      force = a_force;
				std::unique_ptr<RE::BSScript::IFunctionArguments>        args{ RE::MakeFunctionArguments(std::move(target), std::move(force)) };
				RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
				if (vm->DispatchMethodCall(handle, type, "PushActorAway", args.get(), none)) {
					return;
				}
			}
			logs::warn("Gifts: Frost Howl could not call PushActorAway on {}", Name(a_target));
		}

		void UpdateFrostHowl(RE::Actor* a_dog, const Settings& a_s, int a_tier)
		{
			if (!a_s.frostHowl || a_tier < kFrostHowl || !a_dog->IsInCombat()) {
				return;
			}
			const auto now = Clock::now();
			if (g_howlReady && now < *g_howlReady) {
				return;
			}
			const auto state = a_dog->AsActorState();
			if (!Grounded(a_dog) || state->IsBleedingOut() || a_dog->IsInRagdollState() || state->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal) {
				return;
			}
			const auto lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return;
			}
			std::vector<RE::Actor*> foes;
			for (const auto& handle : lists->highActorHandles) {
				const auto ptr = handle.get();
				const auto actor = ptr.get();
				if (!actor || actor == a_dog || actor->IsPlayerRef() || actor->IsPlayerTeammate() || !Grounded(actor) || actor->IsInRagdollState() ||
					actor->AsActorState()->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal || actor->GetPosition().GetDistance(a_dog->GetPosition()) > kHowlRange ||
					!Combat::Engaged(a_dog, actor)) {
					continue;
				}
				foes.push_back(actor);
			}
			const bool hurt = Actions::Hurt(a_dog);
			if (foes.size() < kHowlCrowd && !(hurt && !foes.empty())) {
				return;
			}
			const double cooldown = a_s.frostHowlCooldown * (a_tier >= kQuicker ? 0.6 : 1.0);
			g_howlReady = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(cooldown));
			SoundCues::Play(a_dog, SoundCues::Cue::kFrostHowl);  // his own Frost Howl (the wolf howl stays for singing)
			Runes::Light(Runes::Flare::kGift);
			if (const auto frost = RE::TESForm::LookupByID<RE::TESEffectShader>(kStepMist)) {
				a_dog->ApplyEffectShader(frost, kStepMistFor);
			}
			std::string             names;
			std::vector<RE::FormID> ids;
			for (const auto foe : foes) {
				PushAway(a_dog, foe, static_cast<float>(a_s.frostHowlForce));
				names += (names.empty() ? "" : ", ") + Name(foe);
				ids.push_back(foe->GetFormID());
			}
			// A moment later: did they actually go down? (The push goes through Papyrus, so the call itself can't say.)
			std::thread([ids, session = Bridge::Session()] {
				std::this_thread::sleep_for(kHowlCheckAfter);
				SKSE::GetTaskInterface()->AddTask([ids, session] {
					if (session != Bridge::Session()) {
						return;
					}
					std::string result;
					int         down = 0;
					for (const auto id : ids) {
						const auto foe = RE::TESForm::LookupByID<RE::Actor>(id);
						if (!foe) {
							continue;
						}
						const auto knock = foe->AsActorState()->GetKnockState();
						const bool fell = foe->IsDead() || foe->IsInRagdollState() || knock != RE::KNOCK_STATE_ENUM::kNormal;
						down += fell;
						result += std::format("{}{}: {}", result.empty() ? "" : ", ", Name(foe),
							foe->IsDead() ? "dead"s : fell ? std::format("down (knock state {})", static_cast<int>(knock)) : "still standing"s);
					}
					logs::info("Gifts: Frost Howl check, {} of {} down: {}", down, ids.size(), result);
				});
			}).detach();
			logs::info("Gifts: Frost Howl ({}{} foes close): {} knocked down, ready again in {:.0f} s", hurt ? "badly hurt, " : "", foes.size(), names, cooldown);
			Bridge::PostEvent(std::format("{} let out a howl, frost burst from him, and the foes around him were knocked off their feet.", Name(a_dog)),
				a_dog->GetFormID(), 0, false);
		}

		void SetFact(std::string a_fact)
		{
			std::scoped_lock lock(g_factLock);
			g_fact = std::move(a_fact);
		}

		std::string Name(RE::TESObjectREFR* a_ref)
		{
			const char* name = a_ref ? a_ref->GetDisplayFullName() : nullptr;
			return name && *name ? name : "The dog";
		}

		// A frost bite landing lights the runes a little.
		class HitSink final : public RE::BSTEventSink<RE::TESHitEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				const auto dogId = g_dogId.load();
				if (a_event && dogId && a_event->cause && a_event->cause->GetFormID() == dogId) {
					if (const auto spell = Lookup<RE::SpellItem>(kFrostBiteSpell); spell && a_event->source == spell->GetFormID()) {
						Runes::Light(Runes::Flare::kBite);
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		HitSink g_hitSink;
	}

	void Update(RE::Actor* a_dog, const Settings& a_settings)
	{
		const auto base = Lookup<RE::TESNPC>(kHjalti);
		if (!a_settings.runesEnabled || !a_settings.soulGifts || !a_dog || !base || a_dog->GetActorBase() != base) {
			if (a_dog && base && a_dog->GetActorBase() == base) {
				// Switched off: the bite carries nothing (its perk stays, empty) and a heal under way stops.
				UpdateFrostBite(a_dog, base, 0);
				g_healUntil.reset();
				g_healLeft = 0.0f;
			}
			if (a_dog && base && a_dog->GetActorBase() == base) {
				LogSummary(!a_settings.runesEnabled ? "Gifts: off (runes switched off in settings)" : "Gifts: off (soul gifts switched off in settings)");
			}
			g_dogId = 0;
			SetFact("");
			return;
		}
		g_dogId = a_dog->GetFormID();
		const int souls = Runes::Souls();
		const int tier = Runes::Step(souls, a_settings.soulsPerUnlock, kTiers);
		LogSummary(Summary(souls, a_settings.soulsPerUnlock, tier, a_settings));
		const int frost = FrostLevel(tier);
		UpdateFrostBite(a_dog, base, frost);
		UpdatePerseverance(a_dog, a_settings, tier);
		UpdateFrostStep(a_dog, a_settings, tier);
		UpdateFrostHowl(a_dog, a_settings, tier);
		if (frost == 0) {
			SetFact("");
			return;
		}
		static constexpr std::array<std::string_view, 3> kChill{ "a cold bite that chills and slows what he bites",
			"a bitter-cold bite that frosts and slows what he bites", "a bite of deep frost that numbs, drains and slows what he bites" };
		std::string fact = std::format("From the dragons' souls he has witnessed, {} has {}", Name(a_dog), kChill[static_cast<std::size_t>(frost - 1)]);
		if (tier >= kPerseverance) {
			const bool ready = !g_perseverReady || Clock::now() >= *g_perseverReady;
			fact += g_healUntil ? ", and right now the runes are knitting his wounds" :
			        ready       ? ", and when he is badly hurt the runes can knit his wounds" :
			                      ", and the runes that knit his wounds are spent for now";
		}
		if (tier >= kFrostStep && a_settings.frostStep) {
			fact += ", and in a fight he can vanish in a breath of frost and come out behind a far foe, or at his companion's side when badly hurt";
		}
		if (tier >= kFrostHowl && a_settings.frostHowl) {
			fact += ", and when foes crowd him he can howl them off their feet";
		}
		SetFact(fact + ".");
	}

	float FrostBiteDamage()
	{
		return g_frostSet > 0 ? kFrost[static_cast<std::size_t>(g_frostSet - 1)].damage : 0.0f;
	}

	bool StepWithin(RE::Actor* a_dog, RE::Actor* a_target)
	{
		const auto s = SnapshotSettings();
		const auto now = Clock::now();
		if (!a_dog || !a_target || a_dog->GetFormID() != g_dogId.load() || !s.frostStep || Runes::Step(Runes::Souls(), s.soulsPerUnlock, kTiers) < kFrostStep ||
			(g_stepReady && now < *g_stepReady) || (g_stepPause && now < *g_stepPause) || !Grounded(a_dog) || !Grounded(a_target) || !SamePlace(a_dog, a_target)) {
			return false;
		}
		const auto  from = a_dog->GetPosition();
		const auto  at = a_target->GetPosition();
		const float distance = from.GetDistance(at);
		bool        unused = false;
		return distance >= kStepNear && distance <= kStepFar && std::abs(from.z - at.z) <= kStepRise && a_dog->HasLineOfSight(a_target, unused);
	}

	std::string Fact()
	{
		std::scoped_lock lock(g_factLock);
		return g_fact;
	}

	void ResetSession()
	{
		g_summary.clear();  // log the gifts again after the load
		g_perseverReady.reset();
		g_healUntil.reset();
		g_healLeft = 0.0f;
		g_lastTick.reset();
		g_stepReady.reset();
		g_escapeReady.reset();
		g_stepPause.reset();
		g_howlReady.reset();
	}

	void RegisterEvents()
	{
		if (const auto holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			holder->AddEventSink<RE::TESHitEvent>(&g_hitSink);
		}
	}
}
