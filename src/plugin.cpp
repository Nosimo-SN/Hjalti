// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.

// Canine Drives: bodily needs for one dog companion. Five drives (hunger, affection, restlessness,
// tiredness, separation) rise and fall with game time and what the dog is doing, are kept in the
// co-save, and reach SkyrimNet through canine_* decorators: the values for action eligibility rules and
// canine_needs, the strongest needs worded as prompt text. Food arriving in his inventory is eaten, and
// petting (a hotkey, or optionally activating him) settles his need for affection.
// A worker thread paces the ticks and reloads settings; the drives are stepped in tasks on the game thread.

#include "actions.h"
#include "barklog.h"
#include "soundcues.h"
#include "behavior.h"
#include "bridge.h"
#include "dreams.h"
#include "drives.h"
#include "hotkey.h"
#include "inputs.h"
#include "combat.h"
#include "mood.h"
#include "progression.h"
#include "runes.h"
#include "gifts.h"
#include "senses.h"
#include "settings.h"
#include "sleep.h"
#include "training.h"
#include "watchdog.h"

namespace
{
	constexpr auto          kTickInterval = 1s;
	constexpr int           kReloadEveryTicks = 5;  // settings reload every ~5 s
	constexpr double        kMaxStepHours = 24.0;   // a longer gap counts as this much
	constexpr std::uint32_t kSerializationID = 'CNDR';
	constexpr double        kDreamSleepHours = 3.0;  // a sleep this long beside the player can bring him a dream
	constexpr double        kHoursPerDream = 4.0;
	constexpr int           kMaxDreamsPerSleep = 3;

	using Clock = std::chrono::steady_clock;

	std::atomic_bool g_inGame{ false };
	std::atomic_bool g_tickQueued{ false };

	// --- game thread ---------------------------------------------------------------------------------

	struct Target
	{
		std::string configured;  // the settings string this was resolved from
		RE::FormID  ref = 0;
		RE::FormID  base = 0;
		bool        byBase = false;  // the setting names an NPC record: whichever loaded actor of it (a spawned dog)
	};

	Target                           g_target;
	std::optional<RE::NiPoint3>      g_lastPos;
	std::optional<Clock::time_point> g_lastReal;
	bool                             g_lastNear = false;
	double                           g_lastLoggedHour = -1.0;
	std::optional<std::string>       g_lastDebugSet;  // nullopt until the first settings read
	bool                             g_aiChecked = false;  // his AI checked once after this load

	// Sleep: the player's sleep counts as the dog's when he was close by when it started. Game time
	// jumps while the sleep runs, so every step between the start and stop events counts as asleep, and
	// a step that straddles the stop counts as asleep up to the hour it ended.
	std::optional<double> g_sleepStart;
	std::optional<double> g_sleepEnd;

	// "Plugin.esp|0xLocalID" -> runtime FormID, or 0.
	RE::FormID ResolveFormSpec(const std::string& a_spec)
	{
		const auto bar = a_spec.rfind('|');
		if (bar == std::string::npos) {
			return 0;
		}
		const auto plugin = a_spec.substr(0, bar);
		RE::FormID local = 0;
		try {
			local = static_cast<RE::FormID>(std::stoul(a_spec.substr(bar + 1), nullptr, 16));
		} catch (...) {
			return 0;
		}
		const auto data = RE::TESDataHandler::GetSingleton();
		return data ? data->LookupFormID(local & 0x00FFFFFF, plugin) : 0;
	}

	// A loaded actor whose base is this NPC record (a spawned dog has a new reference every time).
	RE::Actor* FindByBase(RE::FormID a_base)
	{
		const auto lists = RE::ProcessLists::GetSingleton();
		if (!lists) {
			return nullptr;
		}
		for (const auto* list : { &lists->highActorHandles, &lists->middleHighActorHandles, &lists->middleLowActorHandles, &lists->lowActorHandles }) {
			for (const auto& handle : *list) {
				const auto actor = handle.get();
				const auto npc = actor ? actor->GetActorBase() : nullptr;
				if (npc && (npc->GetFormID() == a_base || npc->GetRootFaceNPC() && npc->GetRootFaceNPC()->GetFormID() == a_base) && !actor->IsDead()) {
					return actor.get();
				}
			}
		}
		return nullptr;
	}

	void SetTargetRef(RE::FormID a_ref)
	{
		g_target.ref = a_ref;
		Canine::Inputs::SetTarget(a_ref);
		Canine::Behavior::SetTarget(a_ref);
		g_lastPos.reset();
	}

	void ResolveTarget(const Canine::Settings& a_settings)
	{
		if (g_target.configured == a_settings.actor && g_target.byBase) {
			// Keep hold of him while he exists; look for him again when he's gone (or before he's found).
			const auto current = g_target.ref ? RE::TESForm::LookupByID<RE::Actor>(g_target.ref) : nullptr;
			if (!current || current->IsDeleted() || current->IsDead()) {
				if (const auto found = FindByBase(g_target.base); found && found->GetFormID() != g_target.ref) {
					SetTargetRef(found->GetFormID());
					logs::info("Tracking {} (spawned, runtime {:08X})", found->GetDisplayFullName(), found->GetFormID());
				}
			}
			return;
		}
		if (g_target.configured == a_settings.actor) {
			if (g_target.ref && !g_target.base) {
				// The base is only readable once the reference exists in memory.
				if (const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(g_target.ref)) {
					if (const auto base = ref->GetBaseObject()) {
						g_target.base = base->GetFormID();
					}
				}
			}
			return;
		}
		g_target = { a_settings.actor, ResolveFormSpec(a_settings.actor), 0 };
		if (const auto form = g_target.ref ? RE::TESForm::LookupByID(g_target.ref) : nullptr; form && form->Is(RE::FormType::NPC)) {
			// An NPC record, not a placed reference: track the loaded actor of it, found each tick until there is one.
			g_target.base = g_target.ref;
			g_target.byBase = true;
			SetTargetRef(0);
			logs::info("Tracking any {} (the NPC record {:08X}); waiting for him to be loaded", a_settings.actor, g_target.base);
			return;
		}
		Canine::Inputs::SetTarget(g_target.ref);
		Canine::Behavior::SetTarget(g_target.ref);
		g_lastPos.reset();
		if (g_target.ref) {
			logs::info("Tracking {} (runtime {:08X})", a_settings.actor, g_target.ref);
		} else {
			logs::warn("Could not resolve actor '{}' (expected Plugin.esp|0xLocalID of a placed reference)", a_settings.actor);
		}
	}

	std::string DogName(RE::Actor* a_actor)
	{
		const char* name = a_actor ? a_actor->GetDisplayFullName() : nullptr;
		return name && *name ? name : "The dog";
	}

	std::string PlayerName(RE::PlayerCharacter* a_player)
	{
		const char* name = a_player ? a_player->GetDisplayFullName() : nullptr;
		return name && *name ? name : "the player";
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

	void Tick()
	{
		g_tickQueued = false;
		if (!g_inGame) {
			return;
		}
		const auto calendar = RE::Calendar::GetSingleton();
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!calendar || !player) {
			return;
		}

		const auto s = Canine::SnapshotSettings();
		ResolveTarget(s);

		// debug.set applies once each time its text changes; whatever is there at startup is only remembered.
		if (!g_lastDebugSet) {
			g_lastDebugSet = s.debugSet;
		} else if (*g_lastDebugSet != s.debugSet) {
			g_lastDebugSet = s.debugSet;
			// "graph <label>" dumps his animation graph variables to the log (e.g. "graph lying", "graph standing").
			const auto dog = g_target.ref ? RE::TESForm::LookupByID<RE::Actor>(g_target.ref) : nullptr;
			if (s.debugSet == "bark") {  // test barks: his bark as it plays now, or the bark sound alone
				logs::info("debug.set 'bark'");
				Canine::Actions::Bark(dog, false);
			} else if (s.debugSet == "bark sound") {
				logs::info("debug.set 'bark sound'");
				Canine::Actions::BarkSound(dog);
			} else if (s.debugSet == "sleep" || s.debugSet.starts_with("sleep ")) {
				// "sleep" naps now, the style picked as usual; "sleep relax|guarded|cold" forces that style for this nap.
				const auto rest = s.debugSet.size() > 6 ? std::string_view(s.debugSet).substr(6) : ""sv;
				const auto style = Canine::Sleep::Parse(rest);
				if (!rest.empty() && !style) {
					logs::warn("debug.set '{}': the style is relax, guarded or cold", s.debugSet);
				} else if (!dog || !dog->Is3DLoaded()) {
					logs::warn("debug.set '{}': he isn't here", s.debugSet);
				} else if (Canine::Actions::NapOn()) {
					logs::warn("debug.set '{}': he's already napping (wake him first)", s.debugSet);
				} else {
					Canine::Sleep::Force(style);
					const bool ok = Canine::Actions::Perform(dog, "nap");
					Canine::Sleep::Force(std::nullopt);  // a nap that didn't start mustn't leave the style forced
					logs::info("debug.set '{}': {}", s.debugSet, ok ? "napping now" : "the nap didn't start");
				}
			} else if (s.debugSet.starts_with("pose ")) {
				// "pose relax|guarded|cold": the awake lying pose (on his side, chin on paws, curled), as the dialogue actions do.
				const auto style = Canine::Sleep::Parse(std::string_view(s.debugSet).substr(5));
				if (!style) {
					logs::warn("debug.set '{}': the pose is relax, guarded or cold", s.debugSet);
				} else {
					logs::info("debug.set '{}': {}", s.debugSet, Canine::Actions::Pose(dog, *style) ? "posing" : "he can't now");
				}
			} else if (s.debugSet == "rollover") {
				logs::info("debug.set 'rollover': {}", Canine::Actions::Rollover(dog, "debug.set") ? "rolling over" : "he can't now");
			} else if (s.debugSet == "wiggle") {
				logs::info("debug.set 'wiggle': {}", Canine::Actions::Wiggle(dog, "debug.set") ? "wiggling" : "he can't now");
			} else if (s.debugSet == "sneeze") {
				logs::info("debug.set 'sneeze': {}", Canine::Actions::Sneeze(dog, "debug.set") ? "sneezing" : "he can't now");
			} else if (s.debugSet == "happybark" || s.debugSet == "angrybark" || s.debugSet == "growl" || s.debugSet == "pout" ||
			           s.debugSet == "sadwhine") {
				using V = Canine::Actions::Vocal;
				const auto kind = s.debugSet == "happybark" ? V::kHappyBark : s.debugSet == "angrybark" ? V::kAngryBark :
				                  s.debugSet == "growl"     ? V::kGrowl :
				                  s.debugSet == "pout"      ? V::kPout : V::kSad;
				logs::info("debug.set '{}': {}", s.debugSet, Canine::Actions::Voice(dog, kind, "debug.set") ? "done" : "he can't now");
			} else if (s.debugSet == "bark sound" || s.debugSet == "angry bark sound" || s.debugSet == "growl sound" ||
			           s.debugSet == "whine sound") {
				using C = Canine::SoundCues::Cue;
				Canine::SoundCues::Play(dog, s.debugSet == "bark sound" ? C::kBark : s.debugSet == "angry bark sound" ? C::kBarkAngry :
				                             s.debugSet == "growl sound" ? C::kGrowl : C::kWhine);
			} else if (s.debugSet == "eat sound") {
				Canine::SoundCues::Play(dog, Canine::SoundCues::Cue::kEat);
			} else if (s.debugSet == "frost step sound" || s.debugSet == "frost howl sound" || s.debugSet == "perseverance sound") {
				using C = Canine::SoundCues::Cue;
				Canine::SoundCues::Play(dog, s.debugSet == "frost step sound" ? C::kFrostStep : s.debugSet == "frost howl sound" ? C::kFrostHowl : C::kPerseverance);
			} else if (s.debugSet.starts_with("pitch")) {
				// "pitch 1.2": every voice sound at that pitch until "pitch off" (then his level's again)
				const auto arg = std::string(s.debugSet.size() > 6 ? std::string_view(s.debugSet).substr(6) : "off"sv);
				float      value = 0.0f;
				if (arg != "off") {
					try {
						value = std::clamp(std::stof(arg), 0.5f, 2.0f);
					} catch (...) {
						logs::warn("debug.set '{}': the pitch is a number like 1.2, or off", s.debugSet);
					}
				}
				Canine::SoundCues::OverridePitch(value);
			} else if (s.debugSet == "sneeze sound") {
				Canine::SoundCues::Play(dog, Canine::SoundCues::Cue::kSneeze);
			} else if (s.debugSet == "sniff sound") {
				Canine::SoundCues::Play(dog, Canine::SoundCues::Cue::kSniff);
			} else if (s.debugSet == "big sniff sound") {
				Canine::SoundCues::Play(dog, Canine::SoundCues::Cue::kSniffBig);
			} else if (s.debugSet == "sniffair") {
				logs::info("debug.set 'sniffair': {}", Canine::Actions::AirSniff(dog, "debug.set") ? "sniffing" : "he can't now");
			} else if (s.debugSet == "sniffground") {
				logs::info("debug.set 'sniffground': {}", Canine::Actions::GroundSniff(dog, "debug.set") ? "sniffing" : "he can't now");
			} else if (s.debugSet == "tilt") {
				logs::info("debug.set 'tilt': {}", Canine::Actions::CuriousTilt(dog, "debug.set") ? "tilting" : "he can't now");
			} else if (s.debugSet == "playbow") {
				logs::info("debug.set 'playbow': {}", Canine::Actions::PlayBow(dog, "debug.set") ? "bowing" : "he can't now");
			} else if (s.debugSet.starts_with("graph")) {
				Canine::Actions::DumpGraph(g_target.ref ? RE::TESForm::LookupByID<RE::Actor>(g_target.ref) : nullptr,
					s.debugSet.size() > 6 ? std::string_view(s.debugSet).substr(6) : "now"sv);
			} else if (Canine::Runes::ApplyDebugSet(s.debugSet)) {
			} else if (!s.debugSet.empty() && Canine::Drives::ApplyDebugSet(s.debugSet)) {
				logs::info("debug.set '{}': {}", s.debugSet, Canine::Drives::Describe());
			}
		}

		const double now = calendar->GetHoursPassed();
		const auto   realNow = Clock::now();
		const double last = Canine::Drives::LastHours();
		Canine::Drives::SetLastHours(now);
		const auto lastReal = g_lastReal;
		g_lastReal = realNow;

		const auto actor = g_target.ref ? RE::TESForm::LookupByID<RE::Actor>(g_target.ref) : nullptr;
		const bool loaded = actor && actor->Is3DLoaded();
		if (actor && !g_aiChecked) {
			g_aiChecked = true;
			Canine::Actions::ResumeAfterLoad(actor);
		}

		// The situation is read every tick, so the log shows it even while game time stands still.
		Canine::Drives::Context ctx;
		std::optional<float>    distance;
		if (loaded && SameSpace(actor, player)) {
			distance = actor->GetPosition().GetDistance(player->GetPosition());
			ctx.isNear = *distance <= s.nearDistance;
		}
		if (loaded && g_lastPos && lastReal) {
			const double seconds = std::chrono::duration<double>(realNow - *lastReal).count();
			if (seconds > 0.0) {
				ctx.moving = actor->GetPosition().GetDistance(*g_lastPos) / seconds >= s.movingSpeed;
			}
		}
		ctx.combat = loaded && actor->IsInCombat();
		const float hour = calendar->GetHour();
		ctx.night = hour >= 21.0f || hour < 6.0f;

		if (last >= 0.0 && now > last && s.enabled && !(actor && actor->IsDead())) {
			ctx.hours = (std::min)(now - last, kMaxStepHours);
			if (g_sleepStart || Canine::Actions::Napping()) {  // a nap counts as sleep too, at its own rate
				ctx.sleepHours = ctx.hours;
				ctx.napping = !g_sleepStart;
			} else if (g_sleepEnd) {
				ctx.sleepHours = std::clamp(*g_sleepEnd - last, 0.0, ctx.hours);
			}
			Canine::Drives::Step(ctx, s);
		}
		if (g_sleepEnd && now >= *g_sleepEnd) {
			g_sleepEnd.reset();
		}
		g_lastNear = ctx.isNear;
		if (loaded) {
			g_lastPos = actor->GetPosition();
		} else {
			g_lastPos.reset();
		}

		if (s.logChanges && (g_lastLoggedHour < 0.0 || now - g_lastLoggedHour >= 1.0)) {
			g_lastLoggedHour = now;
			logs::info("[{:.2f} h] {} near={} moving={} combat={} night={} | {}", now,
				!actor ? "not found"s : !loaded ? "not loaded"s : distance ? std::format("{:.0f} units", *distance) : "elsewhere"s,
				ctx.isNear, ctx.moving, ctx.combat, ctx.night, Canine::Drives::Describe());
		}

		const auto dogName = DogName(actor);
		const auto playerName = PlayerName(player);
		if (actor && loaded) {
			Canine::Sleep::Update(actor);  // ahead of any nap this tick: the SeverActions cold reading
		}
		Canine::Actions::Process(actor, ctx.moving, s);
		Canine::Watchdog::Update(actor, player);
		Canine::Inputs::Process(actor, dogName, playerName, s);
		// For the C API: napping first, then a fight, then eating, then whatever a roam has him doing.
		const auto inputState = Canine::Inputs::State();
		std::uint32_t activity = CDAPI_ACTIVITY_UNKNOWN;
		if (actor && loaded && !actor->IsDead()) {
			const auto roam = Canine::Actions::RoamActivity();
			activity = Canine::Actions::Napping() ? CDAPI_ACTIVITY_NAPPING :
			           ctx.combat                 ? CDAPI_ACTIVITY_FIGHTING :
			           inputState == "eating"sv   ? CDAPI_ACTIVITY_EATING :
			           roam != 0                  ? roam :
			                                        CDAPI_ACTIVITY_AWAKE;
		}
		Canine::Drives::Publish(g_target.ref, g_target.base, dogName, playerName,
			Canine::Actions::Napping() ? "napping"sv : inputState, activity, s);
		Canine::Training::Update(now, g_target.ref, g_target.base, playerName, s);
		if (actor) {
			Canine::Actions::QuietIdleBarks(actor, s.quietBarks);
		}
		Canine::BarkLog::Update(actor, s.logBarks);
		Canine::SoundCues::Update(actor);
		Canine::Mood::Update(actor, s);
		Canine::Senses::Update(actor, s);
		Canine::Progression::Update(actor, s);
		Canine::Combat::Update(actor, s);
		Canine::Runes::Update(actor, s);
		Canine::Gifts::Update(actor, s);
	}

	// Game thread, from the hotkey poller.
	void OnPetHotkey()
	{
		if (!g_inGame || !g_target.ref) {
			return;
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto actor = RE::TESForm::LookupByID<RE::Actor>(g_target.ref);
		if (!player || !actor) {
			return;
		}
		Canine::Inputs::PetByHotkey(actor, DogName(actor), PlayerName(player), Canine::SnapshotSettings());
	}

	void ReloadSettings()
	{
		Canine::ReloadSettings();
		const auto s = Canine::SnapshotSettings();
		Hotkey::Configure(s.petHotkey, s.petModifiers);
	}

	// Pace from our own thread: one queued tick at a time, never a self-requeueing task.
	void WorkerLoop()
	{
		int ticks = 0;
		for (;;) {
			std::this_thread::sleep_for(kTickInterval);
			if (++ticks >= kReloadEveryTicks) {
				ticks = 0;
				ReloadSettings();
			}
			if (!g_tickQueued.exchange(true)) {
				SKSE::GetTaskInterface()->AddTask([] { Tick(); });
			}
		}
	}

	// --- sleep ---------------------------------------------------------------------------------------

	class SleepSink :
		public RE::BSTEventSink<RE::TESSleepStartEvent>,
		public RE::BSTEventSink<RE::TESSleepStopEvent>
	{
	public:
		static SleepSink* GetSingleton()
		{
			static SleepSink singleton;
			return &singleton;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::TESSleepStartEvent* a_event, RE::BSTEventSource<RE::TESSleepStartEvent>*) override
		{
			const auto calendar = RE::Calendar::GetSingleton();
			if (a_event && calendar && g_lastNear) {
				g_sleepStart = calendar->GetHoursPassed();
				g_sleepEnd.reset();
			}
			return RE::BSEventNotifyControl::kContinue;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::TESSleepStopEvent* a_event, RE::BSTEventSource<RE::TESSleepStopEvent>*) override
		{
			const auto calendar = RE::Calendar::GetSingleton();
			if (a_event && calendar && g_sleepStart) {
				const double end = calendar->GetHoursPassed();
				g_sleepEnd = end;
				logs::info("Slept {:.1f} h beside the player", end - *g_sleepStart);
				if (end - *g_sleepStart >= kDreamSleepHours && g_target.ref) {
					// A long sleep holds several dreams: one per few hours slept.
					const int count = std::clamp(static_cast<int>((end - *g_sleepStart) / kHoursPerDream), 1, kMaxDreamsPerSleep);
					Canine::Dreams::Request(RE::TESForm::LookupByID<RE::Actor>(g_target.ref), "night", count);
				}
			}
			g_sleepStart.reset();
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// --- lifecycle -----------------------------------------------------------------------------------

	void ResetSession()
	{
		Bridge::NewSession();
		g_lastPos.reset();
		g_lastReal.reset();
		g_lastNear = false;
		g_lastLoggedHour = -1.0;
		g_aiChecked = false;
		g_sleepStart.reset();
		g_sleepEnd.reset();
		Canine::Inputs::ResetSession();
		Canine::Actions::ResetSession();
		Canine::Behavior::ResetSession();
		Canine::Dreams::ResetSession();
		Canine::Training::ResetSession();
		Canine::Senses::ResetSession();
		Canine::Mood::ResetSession();
		Canine::Sleep::ResetSession();
		Canine::Combat::ResetSession();
		Canine::Runes::ResetSession();
		Canine::Gifts::ResetSession();
		Canine::Watchdog::ResetSession();
	}

	void InitializeLog()
	{
		auto path = logs::log_directory();
		if (!path) {
			SKSE::stl::report_and_fail("Failed to find standard logging directory"sv);
		}
		*path /= "CanineDrives.log"sv;
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S:%e] [%l] %v"s);
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			if (const int version = Bridge::Init()) {
				logs::info("SkyrimNet API v{}; {} decorators registered; settings {}", version, Bridge::RegisterDecorators(),
					Bridge::HasConfig() ? "yes" : "NO (defaults)");
				Canine::Actions::BuildFoodList();
				logs::info("{} actions registered", Bridge::RegisterActions());
				Canine::Behavior::Start();
				Canine::Senses::Start();
			} else {
				logs::warn("SkyrimNet API not found: drives still run, but nothing reaches SkyrimNet");
			}
			ReloadSettings();
			if (const auto events = RE::ScriptEventSourceHolder::GetSingleton()) {
				events->AddEventSink<RE::TESSleepStartEvent>(SleepSink::GetSingleton());
				events->AddEventSink<RE::TESSleepStopEvent>(SleepSink::GetSingleton());
			}
			Canine::Inputs::Register();
			Canine::Actions::RegisterEvents();
			Canine::Gifts::RegisterEvents();
			Hotkey::Start([] { SKSE::GetTaskInterface()->AddTask([] { OnPetHotkey(); }); });
			std::thread(WorkerLoop).detach();
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			g_inGame = false;
			Canine::Behavior::SetInGame(false);
			// Replies still out belong to the game being left: from here on they are dropped, even mid-load.
			Bridge::NewSession();
			Canine::BarkLog::Detach();
			Canine::SoundCues::Detach();
			Canine::Combat::Detach();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			ResetSession();
			// The payload is the success bool cast into the pointer value.
			g_inGame = a_msg->data != nullptr;
			Canine::Behavior::SetInGame(g_inGame);
			break;
		case SKSE::MessagingInterface::kNewGame:
			ResetSession();
			g_inGame = true;
			Canine::Behavior::SetInGame(true);
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	InitializeLog();
	SKSE::Init(a_skse, false);
	logs::info("Canine Drives {} loaded", SKSE::PluginDeclaration::GetSingleton()->GetVersion().string("."));

	const auto serialization = SKSE::GetSerializationInterface();
	serialization->SetUniqueID(kSerializationID);
	serialization->SetSaveCallback([](SKSE::SerializationInterface* a_intfc) {
		Canine::Drives::Save(a_intfc);
		Canine::Training::Save(a_intfc);
		Canine::Progression::Save(a_intfc);
		Canine::Runes::Save(a_intfc);
	});
	serialization->SetLoadCallback([](SKSE::SerializationInterface* a_intfc) {
		std::uint32_t type = 0, version = 0, length = 0;
		while (a_intfc->GetNextRecordInfo(type, version, length)) {
			if (Canine::Drives::LoadRecord(a_intfc, type, version) || Canine::Progression::LoadRecord(a_intfc, type, version) ||
				Canine::Runes::LoadRecord(a_intfc, type, version)) {
				continue;
			}
			if (type == Canine::Training::kRecord) {
				Canine::Training::LoadRecord(a_intfc, version);
			}
		}
	});
	serialization->SetRevertCallback([](SKSE::SerializationInterface* a_intfc) {
		Canine::Drives::Revert(a_intfc);
		Canine::Training::Revert();
		Canine::Progression::Revert();
		Canine::Runes::Revert();
	});

	if (!SKSE::GetMessagingInterface()->RegisterListener(OnMessage)) {
		logs::critical("Could not register the SKSE message listener");
		return false;
	}
	return true;
}
