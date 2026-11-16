// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#include "combat.h"

#include "actions.h"
#include "bridge.h"
#include "runes.h"

#include <chrono>
#include <map>
#include <mutex>
#include <set>

namespace Canine::Combat
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr auto kCallOffFor = 8s;

		std::string Name(RE::TESObjectREFR* a_ref, std::string_view a_fallback)
		{
			const char* name = a_ref ? a_ref->GetDisplayFullName() : nullptr;
			return name && *name ? name : std::string(a_fallback);
		}

		std::set<RE::FormID>             g_spared;     // enemies he let go (until they turn on us again)
		std::optional<Clock::time_point> g_calledOff;  // the player's "leave it", until then
		bool                             g_calledOffTold = false;

		// The player's "get them!": one target, for a while.
		constexpr auto                   kOrderFor = 60s;
		constexpr float                  kOrderRange = 3000.0f;
		RE::FormID                       g_ordered = 0;
		bool                             g_hunting = false;  // the order is his own hunt, not the player's word
		RE::FormID                       g_bitten = 0;       // the prey his killing bite landed on
		bool                             g_killingBite = true;  // this hunt's prey dies to his first bite
		std::optional<Clock::time_point> g_orderedUntil;
		std::optional<Clock::time_point> g_orderNudge;  // next time he's pointed back at them if he drifted off

		// Who struck him or the player lately (hit events come off the game thread). Someone swinging at us hasn't
		// yielded, whatever their faction standing says: a resurrected or calmed enemy can attack without counting as
		// hostile.
		constexpr auto                                   kStruckFor = 6s;
		std::mutex                                       g_struckLock;
		std::map<RE::FormID, Clock::time_point>          g_struck;

		std::map<RE::FormID, RE::FormID> g_struckWith;  // the weapon or spell of each striker's last hit (for the log)

		void NoteStruck(RE::FormID a_by, RE::FormID a_with)
		{
			std::scoped_lock lock(g_struckLock);
			g_struck[a_by] = Clock::now();
			g_struckWith[a_by] = a_with;
		}

		RE::FormID StruckWith(RE::FormID a_by)
		{
			std::scoped_lock lock(g_struckLock);
			const auto it = g_struckWith.find(a_by);
			return it != g_struckWith.end() ? it->second : 0;
		}

		// A hit that is an attack: a weapon or unarmed blow, or a spell flagged hostile. Friendly magic lands as a hit
		// too (2026-11-14: the Greybeards teaching the player a word of power, and he went for Einarth).
		bool HostileHit(RE::FormID a_source)
		{
			const auto form = a_source ? RE::TESForm::LookupByID(a_source) : nullptr;
			if (!form) {
				return true;  // unarmed, a bash, or a source the game doesn't say
			}
			if (const auto magic = form->As<RE::MagicItem>()) {
				if (magic->IsHostile()) {
					return true;
				}
				for (const auto effect : magic->effects) {
					if (effect && effect->baseEffect && effect->baseEffect->IsHostile()) {
						return true;
					}
				}
				return false;
			}
			return true;  // a weapon, an explosion, a trap
		}

		// Game thread: his last swing, and when he was last pointed at someone striking us.
		std::optional<Clock::time_point> g_lastSwing;
		std::optional<Clock::time_point> g_defendNudge;
		RE::FormID                       g_defendTold = 0;

		// Everyone who struck him or the player within a_within.
		std::vector<RE::FormID> StrikersWithin(Clock::duration a_within)
		{
			std::scoped_lock        lock(g_struckLock);
			std::vector<RE::FormID> out;
			const auto              now = Clock::now();
			for (auto it = g_struck.begin(); it != g_struck.end();) {
				if (now - it->second >= kStruckFor) {
					g_struckWith.erase(it->first);
					it = g_struck.erase(it);
					continue;
				}
				if (now - it->second < a_within) {
					out.push_back(it->first);
				}
				++it;
			}
			return out;
		}

		bool StruckLately(RE::FormID a_by)
		{
			std::scoped_lock lock(g_struckLock);
			const auto it = g_struck.find(a_by);
			return it != g_struck.end() && Clock::now() - it->second < kStruckFor;
		}

		// Still fighting us: hostile to the player or him, going for one of us, or struck one of us just now.
		bool Fighting(RE::Actor* a_target, RE::Actor* a_dog, RE::Actor* a_player)
		{
			if (a_target->IsHostileToActor(a_player) || a_target->IsHostileToActor(a_dog) || StruckLately(a_target->GetFormID())) {
				return true;
			}
			if (!a_target->IsInCombat()) {
				return false;
			}
			const auto aim = a_target->GetActorRuntimeData().currentCombatTarget.get();
			return aim && (aim.get() == a_dog || aim.get() == a_player);
		}

		void Stop(RE::Actor* a_dog)
		{
			a_dog->StopCombat();
			a_dog->EvaluatePackage(true, true);
		}

		// Children are never his to fight, whatever they did or whoever says so.
		bool Child(RE::Actor* a_actor)
		{
			if (!a_actor) {
				return false;
			}
			const auto race = a_actor->GetRace();
			return a_actor->IsChild() || (race && race->data.flags.any(RE::RACE_DATA::Flag::kChild));
		}

		// Game thread, as each of his attacks swings: a swing at someone he's sparing, or while he's called off, is cut short.
		void OnSwing(RE::FormID a_dog)
		{
			const auto dog = RE::TESForm::LookupByID<RE::Actor>(a_dog);
			if (!dog || dog->IsDead()) {
				return;
			}
			const auto target = dog->GetActorRuntimeData().currentCombatTarget.get();
			const bool spared = target && target->GetFormID() != g_ordered && g_spared.contains(target->GetFormID()) &&
			                    (target->AsActorState()->IsBleedingOut() || !Fighting(target.get(), dog, RE::PlayerCharacter::GetSingleton()));
			if (g_calledOff || spared) {
				// Ending the fight doesn't stop a bite already in the air: the dog's recoil interrupts it before the hit lands.
				dog->NotifyAnimationGraph("recoilStart");
				Stop(dog);
				logs::info("Combat: swing at {} cut short", target ? Name(target.get(), "?") : "nobody"s);
				return;
			}
			g_lastSwing = Clock::now();
			// Diagnostic: which attack he's doing. The game charges power attacks itself (his race's attack data).
			const auto process = dog->GetActorRuntimeData().currentProcess;
			const auto high = process ? process->high : nullptr;
			const auto attack = high ? high->attackData.get() : nullptr;
			const auto values = dog->AsActorValueOwner();
			if (attack && attack->data.flags.any(RE::AttackData::AttackFlag::kPowerAttack)) {
				Runes::Light(Runes::Flare::kPowerAttack);
			}
			if (!SnapshotSettings().logCombat) {
				return;
			}
			logs::info("Combat: swing, attack {} flags {:#x}, health {:.0f}/{:.0f}, stamina {:.0f}", attack ? attack->event.c_str() : "(none)",
				attack ? attack->data.flags.underlying() : 0u, values ? values->GetActorValue(RE::ActorValue::kHealth) : -1.0f,
				values ? values->GetPermanentActorValue(RE::ActorValue::kHealth) : -1.0f, values ? values->GetActorValue(RE::ActorValue::kStamina) : -1.0f);
		}

		// Each attack sends one "weaponSwing".
		class SwingSink final : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
			{
				if (a_event && a_event->holder && a_event->tag == "weaponSwing"sv) {
					SKSE::GetTaskInterface()->AddTask([id = a_event->holder->GetFormID()] { OnSwing(id); });
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		std::atomic<RE::FormID> g_dogId{ 0 };

		std::string FormName(RE::FormID a_id)
		{
			const auto form = a_id ? RE::TESForm::LookupByID(a_id) : nullptr;
			if (!form) {
				return a_id ? std::format("{:08X}", a_id) : "none"s;
			}
			const char* name = form->GetName();
			return std::format("{} ({:08X}, {})", name && *name ? name : "?", a_id, RE::FormTypeToString(form->GetFormType()));
		}

		// Diagnostic (debug.log_combat): the hits he takes, gathered per attacker and weapon or spell and logged once a
		// second, so a concentration spell's many ticks make one line.
		struct HitTally
		{
			std::string name;
			std::string with;
			int         hits = 0;
			int         power = 0;
			int         sneak = 0;
			int         bash = 0;
			int         blocked = 0;
			float       healthFirst = 0.0f;
		};
		std::mutex                                                g_hitLock;
		std::map<std::pair<RE::FormID, RE::FormID>, HitTally>     g_hits;
		std::optional<Clock::time_point>                          g_hitsSince;
		std::atomic_bool                                          g_logCombat{ false };

		// Game thread: his bite landed on what he was hunting. A hunted animal doesn't survive it, and with the fight over
		// this fast his companions have no time to join in. (Papyrus Actor.Kill, so the kill is credited to him.)
		void KillingBite(RE::FormID a_dog, RE::FormID a_target)
		{
			// One bite sends several hit events (the bite, and the Frost Bite spell on it): the first one does it.
			if (!g_hunting || !g_killingBite || g_ordered != a_target || g_bitten == a_target) {
				return;
			}
			const auto dog = RE::TESForm::LookupByID<RE::Actor>(a_dog);
			const auto prey = RE::TESForm::LookupByID<RE::Actor>(a_target);
			const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			if (!dog || !prey || prey->IsDead() || !vm) {
				return;
			}
			g_bitten = a_target;
			RE::Actor* killer = dog;
			std::unique_ptr<RE::BSScript::IFunctionArguments>        args{ RE::MakeFunctionArguments(std::move(killer)) };
			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
			vm->DispatchMethodCall(vm->handlePolicy->GetHandleForObject(RE::Actor::FORMTYPE, prey), "Actor", "Kill", args.get(), none);
			logs::info("Combat: his bite kills {}", Name(prey, "?"));
		}

		class HitSink final : public RE::BSTEventSink<RE::TESHitEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				const auto dogId = g_dogId.load();
				if (!a_event || !a_event->target || !dogId) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto targetId = a_event->target->GetFormID();
				if (a_event->cause && a_event->cause->GetFormID() == dogId && targetId != dogId) {
					SKSE::GetTaskInterface()->AddTask([dogId, targetId] { KillingBite(dogId, targetId); });
				}
				if (a_event->cause && (targetId == dogId || targetId == 0x14)) {
					const auto by = a_event->cause->GetFormID();
					if (by != dogId && by != 0x14 && HostileHit(a_event->source)) {
						NoteStruck(by, a_event->source);
					}
				}
				if (!g_logCombat || targetId != dogId) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto dog = a_event->target->As<RE::Actor>();
				const auto values = dog ? dog->AsActorValueOwner() : nullptr;
				const float health = values ? values->GetActorValue(RE::ActorValue::kHealth) : -1.0f;
				const auto  cause = a_event->cause.get();
				using F = RE::TESHitEvent::Flag;
				const auto& flags = a_event->flags;
				std::scoped_lock lock(g_hitLock);
				auto& tally = g_hits[{ cause ? cause->GetFormID() : 0, a_event->source }];
				if (tally.hits == 0) {
					tally.name = Name(cause, "nobody");
					tally.with = FormName(a_event->source);
					tally.healthFirst = health;
				}
				++tally.hits;
				tally.power += flags.any(F::kPowerAttack);
				tally.sneak += flags.any(F::kSneakAttack);
				tally.bash += flags.any(F::kBashAttack);
				tally.blocked += flags.any(F::kHitBlocked);
				if (!g_hitsSince) {
					g_hitsSince = Clock::now();
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// Game thread, every tick: a second after the first hit, one line per attacker and weapon or spell.
		void FlushHits(RE::Actor* a_dog)
		{
			std::map<std::pair<RE::FormID, RE::FormID>, HitTally> hits;
			{
				std::scoped_lock lock(g_hitLock);
				if (!g_hitsSince || Clock::now() - *g_hitsSince < 1s) {
					return;
				}
				hits.swap(g_hits);
				g_hitsSince.reset();
			}
			const float now = a_dog ? a_dog->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth) : -1.0f;
			for (const auto& [key, t] : hits) {
				std::string extra;
				for (const auto& [count, label] : { std::pair{ t.power, "power" }, { t.sneak, "sneak" }, { t.bash, "bash" }, { t.blocked, "blocked" } }) {
					if (count > 0) {
						extra += std::format(", {} x{}", label, count);
					}
				}
				logs::info("Hit log: by {} with {} x{}{}, health {:.0f} -> {:.0f} ({:+.0f})", t.name, t.with, t.hits, extra, t.healthFirst, now, now - t.healthFirst);
			}
		}

		HitSink    g_hitSink;
		SwingSink  g_powerSink;
		RE::ActorHandle g_watched;  // a handle: the actor may be gone by the time we look again

		void Unwatch()
		{
			if (const auto old = g_watched.get()) {
				old->RemoveAnimationGraphEventSink(&g_powerSink);
			}
			g_watched.reset();
		}

		void Watch(RE::Actor* a_dog)
		{
			const bool want = a_dog && a_dog->Is3DLoaded();
			if (g_watched && (g_watched.get().get() != a_dog || !want)) {
				Unwatch();
			}
			if (want) {
				a_dog->AddAnimationGraphEventSink(&g_powerSink);  // re-added every tick: the graph is rebuilt with his 3D
				g_watched = RE::ActorHandle(a_dog);
			}
		}
	}

	namespace
	{
		// Papyrus Actor.StartCombat: he goes for this one now.
		void Attack(RE::Actor* a_dog, RE::Actor* a_target)
		{
			const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			if (!vm || !a_dog || !a_target) {
				return;
			}
			if (Child(a_target)) {
				logs::info("Combat: won't go for {}, a child", Name(a_target, "?"));
				return;
			}
			RE::Actor* target = a_target;
			std::unique_ptr<RE::BSScript::IFunctionArguments>          args{ RE::MakeFunctionArguments(std::move(target)) };
			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
			vm->DispatchMethodCall(vm->handlePolicy->GetHandleForObject(RE::Actor::FORMTYPE, a_dog), "Actor", "StartCombat", args.get(), none);
		}

		// Someone he let go while they were down who is up again and still hostile: back at them.
		void Guard(RE::Actor* a_dog, RE::PlayerCharacter* a_player, const Settings& a_settings)
		{
			for (auto it = g_spared.begin(); it != g_spared.end();) {
				const auto target = RE::TESForm::LookupByID<RE::Actor>(*it);
				if (!target || target->IsDead() || !target->Is3DLoaded() || Child(target)) {
					it = g_spared.erase(it);  // gone (or a child): nothing to watch
					continue;
				}
				if (Fighting(target, a_dog, a_player) && !target->AsActorState()->IsBleedingOut() && !g_calledOff) {
					logs::info("Combat: {} is up again, back at them", Name(target, "?"));
					Bridge::PostEvent(std::format("{}'s hackles shoot up as {} staggers back to their feet.", Name(a_dog, "The dog"), Name(target, "they")),
						a_dog->GetFormID(), target->GetFormID(), a_settings.narrate);
					Attack(a_dog, target);
					it = g_spared.erase(it);
					continue;
				}
				++it;
			}
		}
	}

	namespace
	{
		// Someone striking him or the player while he isn't fighting back (out of combat, aimed elsewhere, or standing
		// there barking without a bite for a few seconds): he goes for them. This covers the engine not counting an
		// attacker as hostile (a resurrected or calmed enemy), which leaves him idle while he's being chewed on.
		void Defend(RE::Actor* a_dog)
		{
			const auto now = Clock::now();
			if (g_defendNudge && now < *g_defendNudge) {
				return;
			}
			const auto aim = a_dog->GetActorRuntimeData().currentCombatTarget.get();
			const bool biting = g_lastSwing && now - *g_lastSwing < 4s;
			RE::Actor* best = nullptr;
			float      bestDistance = 2000.0f;
			for (const auto id : StrikersWithin(3s)) {
				const auto striker = RE::TESForm::LookupByID<RE::Actor>(id);
				if (!striker || striker == a_dog || striker->IsDead() || !striker->Is3DLoaded() || striker->IsPlayerTeammate() ||
					striker->AsActorState()->IsBleedingOut() || Child(striker)) {
					continue;
				}
				const float distance = striker->GetPosition().GetDistance(a_dog->GetPosition());
				if (distance < bestDistance) {
					best = striker;
					bestDistance = distance;
				}
			}
			if (!best) {
				return;
			}
			if (a_dog->IsInCombat() && aim && biting) {
				return;  // he's fighting (this one or another)
			}
			g_defendNudge = now + 3s;
			g_spared.erase(best->GetFormID());
			if (g_defendTold != best->GetFormID()) {
				g_defendTold = best->GetFormID();
				logs::info("Combat: {} is striking at us with {}, he goes for them ({})", Name(best, "?"), FormName(StruckWith(best->GetFormID())),
					!a_dog->IsInCombat() ? "he wasn't fighting" : "no bites lately");
			}
			Attack(a_dog, best);
		}

		// Diagnostic: once a second while he's fighting, what the engine thinks he's doing.
		void LogState(RE::Actor* a_dog)
		{
			static std::optional<Clock::time_point> next;
			static bool                             was = false;
			const bool                              fighting = a_dog->IsInCombat();
			if (fighting != was) {
				was = fighting;
				logs::info("Combat state: {}", fighting ? "fight started" : "fight over");
			}
			const auto now = Clock::now();
			if (!fighting || (next && now < *next)) {
				return;
			}
			next = now + 1s;
			const auto target = a_dog->GetActorRuntimeData().currentCombatTarget.get();
			const auto controller = a_dog->GetActorRuntimeData().combatController;
			const bool fleeing = controller && controller->state && controller->IsFleeing();
			bool       los = false;
			bool       losArg = false;
			if (target) {
				los = a_dog->HasLineOfSight(target.get(), losArg);
			}
			const auto package = a_dog->GetCurrentPackage();
			const char* packageId = package ? package->GetFormEditorID() : nullptr;
			logs::info("Combat state: target {} at {:.0f}, fleeing {}, sees it {}, package {} ({}), health {:.0f}", target ? Name(target.get(), "?") : "none"s,
				target ? a_dog->GetPosition().GetDistance(target->GetPosition()) : -1.0f, fleeing, los,
				packageId && *packageId ? packageId : "?", package ? package->GetObjectTypeName() : "none",
				a_dog->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth));
		}
	}

	namespace
	{
		// Awareness: who is in the fight around him and how they're doing, in words, for his prompt. Built here on the game
		// thread once a second and read by the decorator on SkyrimNet's workers.
		constexpr float kAwareRange = 3000.0f;
		constexpr std::size_t kAwareCap = 6;  // besides the player
		std::mutex                       g_awareLock;
		std::string                      g_aware;
		std::optional<Clock::time_point> g_awareNext;

		void SetAware(std::string a_text)
		{
			std::scoped_lock lock(g_awareLock);
			g_aware = std::move(a_text);
		}

		// "badly hurt, winded, crackling with magic": what could be sensed of them.
		std::string Condition(RE::Actor* a_actor)
		{
			const auto values = a_actor->AsActorValueOwner();
			if (!values) {
				return "";
			}
			auto share = [&](RE::ActorValue a_value) {
				const float max = values->GetPermanentActorValue(a_value);
				return max > 0.0f ? values->GetActorValue(a_value) / max : 1.0f;
			};
			const float health = share(RE::ActorValue::kHealth);
			std::string out = a_actor->AsActorState()->IsBleedingOut() ? "down" : health >= 0.9f ? "unhurt" : health >= 0.6f ? "lightly hurt" : health >= 0.3f ? "hurt" : "badly hurt";
			if (share(RE::ActorValue::kStamina) < 0.25f) {
				out += ", winded";
			}
			const float magic = values->GetPermanentActorValue(RE::ActorValue::kMagicka);
			if (magic >= 100.0f && share(RE::ActorValue::kMagicka) < 0.25f) {
				out += ", their magic running thin";
			} else if (magic >= 150.0f && share(RE::ActorValue::kMagicka) >= 0.5f) {
				out += ", crackling with magic";
			}
			return out;
		}

		std::string Aiming(RE::Actor* a_actor, RE::Actor* a_dog)
		{
			const auto aim = a_actor->GetActorRuntimeData().currentCombatTarget.get();
			if (!aim) {
				return "";
			}
			return ", going for " + (aim.get() == a_dog ? "him"s : Name(aim.get(), "someone"));
		}

		void UpdateAwareness(RE::Actor* a_dog, RE::PlayerCharacter* a_player, const Settings& a_s)
		{
			if (!a_s.combatAwareness || (!a_dog->IsInCombat() && !a_player->IsInCombat())) {
				g_awareNext.reset();
				SetAware("");
				return;
			}
			const auto now = Clock::now();
			if (g_awareNext && now < *g_awareNext) {
				return;
			}
			g_awareNext = now + 1s;
			const auto lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return;
			}
			struct Fighter
			{
				bool        foe;
				float       distance;
				std::string line;
			};
			std::vector<Fighter> fighters;
			const auto           from = a_dog->GetPosition();
			for (const auto& handle : lists->highActorHandles) {
				const auto actor = handle.get();
				if (!actor || actor.get() == a_dog || actor->IsDead() || !actor->Is3DLoaded() || !actor->IsInCombat() || Child(actor.get())) {
					continue;
				}
				const float distance = actor->GetPosition().GetDistance(from);
				if (distance > kAwareRange) {
					continue;
				}
				const bool foe = !actor->IsPlayerTeammate() && Fighting(actor.get(), a_dog, a_player);
				fighters.push_back({ foe, distance,
					std::format("- {}{}: {}{}, {}.", Name(actor.get(), "someone"), foe ? " (foe)" : "", Condition(actor.get()), Aiming(actor.get(), a_dog),
						distance < 500.0f ? "close" : distance < 1500.0f ? "some way off" : "far off") });
			}
			std::ranges::sort(fighters, [](const Fighter& a_one, const Fighter& a_other) {
				return a_one.foe != a_other.foe ? a_one.foe : a_one.distance < a_other.distance;
			});
			if (fighters.empty()) {
				SetAware("");
				return;
			}
			if (fighters.size() > kAwareCap) {
				fighters.resize(kAwareCap);
			}
			std::string text = std::format("The fight around {}, as his nose and ears tell it:\n- {}: {}{}.", Name(a_dog, "the dog"), Name(a_player, "the player"),
				Condition(a_player), Aiming(a_player, a_dog));
			for (const auto& fighter : fighters) {
				text += "\n" + fighter.line;
			}
			SetAware(std::move(text));
		}
	}

	std::string Awareness()
	{
		std::string text;
		{
			std::scoped_lock lock(g_awareLock);
			text = g_aware;
		}
		// What his prompts actually get, at most every ten seconds.
		static std::atomic<Clock::rep> logged{ 0 };
		const auto                     now = Clock::now().time_since_epoch();
		if (!text.empty() && now.count() - logged.load() > std::chrono::duration_cast<Clock::duration>(10s).count()) {
			logged = now.count();
			logs::info("Awareness handed to his prompt: {}", text);
		}
		return text;
	}

	void Update(RE::Actor* a_dog, const Settings& a_settings)
	{
		Watch(a_dog);
		static bool hitRegistered = false;
		if (!hitRegistered) {
			if (const auto holder = RE::ScriptEventSourceHolder::GetSingleton()) {
				holder->AddEventSink<RE::TESHitEvent>(&g_hitSink);
				hitRegistered = true;
			}
		}
		g_dogId = a_dog ? a_dog->GetFormID() : 0;
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!a_dog || !player || !a_dog->Is3DLoaded() || a_dog->IsDead()) {
			SetAware("");
			return;
		}
		g_logCombat = a_settings.logCombat;
		FlushHits(a_dog);
		UpdateAwareness(a_dog, player, a_settings);
		if (a_settings.logCombat) {
			LogState(a_dog);
		}
		if (a_dog->AsActorState()->IsBleedingOut()) {
			return;  // while he's down it's not his call who is spared
		}
		// The engine can still pull him into a fight with a child (following the player's lead, a brawl spilling over):
		// he drops it at once.
		if (a_dog->IsInCombat()) {
			const auto aim = a_dog->GetActorRuntimeData().currentCombatTarget.get();
			if (aim && Child(aim.get())) {
				if (g_ordered == aim->GetFormID()) {
					g_ordered = 0;
					g_orderedUntil.reset();
				}
				g_spared.erase(aim->GetFormID());
				Stop(a_dog);
				// A child going for him is calmed too, so they don't keep swinging at a dog that won't answer.
				const auto theirs = aim->GetActorRuntimeData().currentCombatTarget.get();
				if (aim->IsInCombat() && theirs && theirs.get() == a_dog) {
					aim->StopCombat();
				}
				static RE::FormID told = 0;
				if (told != aim->GetFormID()) {
					told = aim->GetFormID();
					logs::info("Combat: dropped a fight with {}, a child", Name(aim.get(), "?"));
				}
				return;
			}
		}
		if (!Actions::Hurt(a_dog)) {
			Guard(a_dog, player, a_settings);  // badly hurt, he doesn't go back at anyone
			if (!g_calledOff) {
				Defend(a_dog);
			}
		}
		// An order stands until the target is dead or gone, or it runs out; while it does, he keeps at them.
		if (g_ordered) {
			const auto target = RE::TESForm::LookupByID<RE::Actor>(g_ordered);
			const auto now = Clock::now();
			if (!target || target->IsDead() || !target->Is3DLoaded() || (g_orderedUntil && now >= *g_orderedUntil)) {
				logs::info("Combat: order on {} over ({})", Name(target, "?"),
					!target || !target->Is3DLoaded() ? "gone" : target->IsDead() ? "dead" : "ran out");
				g_ordered = 0;
				g_orderedUntil.reset();
			} else {
				const auto aim = a_dog->GetActorRuntimeData().currentCombatTarget.get();
				if ((!a_dog->IsInCombat() || aim.get() != target) && (!g_orderNudge || now >= *g_orderNudge)) {
					g_orderNudge = now + 2s;
					Attack(a_dog, target);
				}
				return;  // no sparing, no backing off from this one
			}
		}
		// The player called him off: no fighting until the time is up.
		if (g_calledOff) {
			if (Clock::now() >= *g_calledOff) {
				g_calledOff.reset();
			} else if (a_dog->IsInCombat()) {
				Stop(a_dog);
			}
		}
		if (!a_settings.spareBeaten || !a_dog->IsInCombat()) {
			return;
		}
		// An enemy who yields (no longer hostile to either of us) or lies down beaten is let go.
		const auto target = a_dog->GetActorRuntimeData().currentCombatTarget.get();
		if (!target || target->IsDead()) {
			return;
		}
		const bool hostile = Fighting(target.get(), a_dog, player);
		if (hostile && !target->AsActorState()->IsBleedingOut()) {
			g_spared.erase(target->GetFormID());  // fighting again: fair game again
			return;
		}
		const bool yielded = !hostile;
		Stop(a_dog);
		if (g_spared.insert(target->GetFormID()).second) {
			logs::info("Combat: letting {} go ({})", Name(target.get(), "?"), yielded ? "yielded" : "down");
			Bridge::PostEvent(std::format("{} backs off from {}, hackles still up, as {} {}.", Name(a_dog, "The dog"), Name(target.get(), "them"),
								  Name(target.get(), "they"), yielded ? "yields" : "goes down"),
				a_dog->GetFormID(), target->GetFormID(), a_settings.narrate);
		}
	}

	void CallOff(RE::Actor* a_dog)
	{
		if (!a_dog) {
			return;
		}
		g_calledOff = Clock::now() + kCallOffFor;
		if (g_ordered) {
			logs::info("Combat: order on {} called off", Name(RE::TESForm::LookupByID<RE::Actor>(g_ordered), "?"));
			g_ordered = 0;
			g_orderedUntil.reset();
		}
		const bool fighting = a_dog->IsInCombat();
		logs::info("Combat: called off{}", fighting ? "" : " (not fighting)");
		if (fighting) {
			Stop(a_dog);
			if (const auto player = RE::PlayerCharacter::GetSingleton()) {
				Bridge::PostEvent(std::format("{} breaks off at {}'s command and backs away, still growling.", Name(a_dog, "The dog"),
									  Name(player, "the player")),
					a_dog->GetFormID(), player->GetFormID(), SnapshotSettings().narrate);
			}
		}
	}

	bool IsChild(RE::Actor* a_actor) { return Child(a_actor); }

	void DropOrder(const char* a_why)
	{
		if (g_ordered) {
			logs::info("Combat: order on {} over ({})", Name(RE::TESForm::LookupByID<RE::Actor>(g_ordered), "?"), a_why);
			g_ordered = 0;
			g_orderedUntil.reset();
		}
	}

	bool Engaged(RE::Actor* a_dog, RE::Actor* a_target)
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!a_dog || !a_target || !player || g_calledOff || Child(a_target) || a_target->IsDead() || a_target->AsActorState()->IsBleedingOut()) {
			return false;
		}
		if (a_target->GetFormID() == g_ordered) {
			return true;
		}
		return !g_spared.contains(a_target->GetFormID()) && Fighting(a_target, a_dog, player);
	}

	void Hunt(RE::Actor* a_dog, RE::Actor* a_prey, std::chrono::seconds a_for, bool a_killingBite)
	{
		if (!a_dog || !a_prey || Child(a_prey)) {
			return;
		}
		g_calledOff.reset();
		g_spared.erase(a_prey->GetFormID());
		g_ordered = a_prey->GetFormID();
		g_hunting = true;
		g_killingBite = a_killingBite;
		g_bitten = 0;
		g_orderedUntil = Clock::now() + a_for;
		g_orderNudge = Clock::now() + 2s;
		Attack(a_dog, a_prey);
	}

	bool CalledOff()
	{
		return g_calledOff.has_value();
	}

	bool BitToDeath(RE::FormID a_prey)
	{
		return a_prey && g_bitten == a_prey;
	}

	RE::FormID Hunted()
	{
		return g_hunting ? g_ordered : 0;
	}

	void EndHunt(RE::Actor* a_dog, const char* a_why)
	{
		if (!g_hunting) {
			return;
		}
		const auto prey = RE::TESForm::LookupByID<RE::Actor>(g_ordered);
		DropOrder(a_why);
		g_hunting = false;
		if (!a_dog || !a_dog->IsInCombat()) {
			return;
		}
		const auto aim = a_dog->GetActorRuntimeData().currentCombatTarget.get();
		if (!aim || aim->IsDead() || aim.get() == prey) {
			Stop(a_dog);
		}
	}

	bool Order(RE::Actor* a_dog, std::string_view a_name)
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto lists = RE::ProcessLists::GetSingleton();
		if (!a_dog || !player || !lists || a_name.empty()) {
			return false;
		}
		auto lower = [](std::string_view a_text) {
			std::string out(a_text);
			std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		};
		// "Fanwyearie [Thalmor Soldier]" is how SkyrimNet shows names; the bracketed title isn't part of a match.
		if (const auto bracket = a_name.find(" ["); bracket != std::string_view::npos && bracket > 0) {
			a_name = a_name.substr(0, bracket);
		}
		const auto want = lower(a_name);
		RE::Actor* best = nullptr;
		int        bestRank = 0;  // 2: the name exactly, 1: one name within the other
		float      bestDistance = kOrderRange;
		for (const auto& handle : lists->highActorHandles) {
			const auto actor = handle.get();
			if (!actor || actor.get() == a_dog || actor.get() == player || actor->IsDead() || actor->IsPlayerTeammate() || !actor->Is3DLoaded()) {
				continue;
			}
			const char* raw = actor->GetDisplayFullName();
			if (!raw || !*raw) {
				continue;
			}
			const auto  name = lower(raw);
			const int   rank = name == want ? 2 : (name.find(want) != std::string::npos || want.find(name) != std::string::npos) ? 1 : 0;
			const float distance = actor->GetPosition().GetDistance(a_dog->GetPosition());
			if (rank == 0 || distance > kOrderRange) {
				continue;
			}
			if (rank > bestRank || (rank == bestRank && distance < bestDistance)) {
				best = actor.get();
				bestRank = rank;
				bestDistance = distance;
			}
		}
		// The best match is a child: refused outright, not passed on to some adult whose name half-matches.
		if (best && Child(best)) {
			logs::info("Combat: told to attack '{}', but {} is a child; he won't", a_name, Name(best, "?"));
			return false;
		}
		if (!best) {
			logs::info("Combat: told to attack '{}', but nobody by that name is close", a_name);
			return false;
		}
		g_calledOff.reset();
		g_spared.erase(best->GetFormID());
		g_ordered = best->GetFormID();
		g_hunting = false;
		g_orderedUntil = Clock::now() + kOrderFor;
		g_orderNudge = Clock::now() + 2s;
		Attack(a_dog, best);
		logs::info("Combat: told to attack '{}': going for {} at {:.0f}", a_name, Name(best, "?"), bestDistance);
		return true;
	}

	void Detach()
	{
		Unwatch();
	}

	void ResetSession()
	{
		SetAware("");
		g_awareNext.reset();
		g_ordered = 0;
		g_hunting = false;
		g_bitten = 0;
		g_orderedUntil.reset();
		g_orderNudge.reset();
		g_lastSwing.reset();
		g_defendNudge.reset();
		g_defendTold = 0;
		g_spared.clear();
		g_calledOff.reset();
		{
			std::scoped_lock lock(g_struckLock);
			g_struck.clear();
		}
		g_calledOffTold = false;
	}
}
