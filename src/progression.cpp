// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#include "progression.h"

#include "bridge.h"
#include "notify.h"

#include <chrono>

namespace Canine::Progression
{
	namespace
	{
		std::atomic_int g_growthLevel{ 0 };
	}

	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr std::uint32_t kRecord = 'PROG';
		constexpr std::uint32_t kVersion = 3;  // 2: speed added; 3: size
		constexpr auto          kEvery = 2s;
		constexpr auto          kPlugin = "SN_Dog.esp"sv;

		// The stats he grows in, in the order the settings and the co-save keep them.
		constexpr std::array<RE::ActorValue, 5> kStats{ RE::ActorValue::kUnarmedDamage, RE::ActorValue::kDamageResist,
			RE::ActorValue::kResistMagic, RE::ActorValue::kHealth, RE::ActorValue::kSpeedMult };
		constexpr std::array<std::string_view, 5> kStatNames{ "bite", "armor", "magic resist", "health", "speed" };

		// Below level 30 a stat steps up at these levels, to this share of its level-30 value.
		constexpr std::array<std::pair<int, double>, 5> kSteps{ { { 5, 0.2 }, { 10, 0.4 }, { 15, 0.6 }, { 20, 0.8 }, { 30, 1.0 } } };

		// His own perks in SN_Dog.esp: (local FormID, the setting that says at which level it unlocks).
		enum class PerkSlot { kSteady1, kSteady2, kSteady3, kBowling };
		constexpr std::array<RE::FormID, 4> kPerks{ 0x807, 0x808, 0x809, 0x80A };
		constexpr std::array<std::string_view, 4> kPerkNames{ "Steady Stance I", "Steady Stance II", "Steady Stance III", "Bowling Charge" };

		struct State
		{
			RE::FormID            ref = 0;       // the dog these were applied to (a new spawn starts from nothing)
			std::array<float, 5>  applied{};     // what this module has added to each stat
			int                   announced = 0; // the highest milestone level already announced
			float                 scale = 0.0f;  // the size we set on him, 0 = never touched
		};
		State             g_state;
		Clock::time_point g_next{};
		RE::FormID        g_perksOn = 0;  // the base record our perks were put on (the base isn't saved, so it outlives a load)
		std::mutex        g_sizeLock;
		std::string       g_sizeFact;  // his size as a sentence for his prompt

		double Value(int a_level, double a_at30, double a_at50, int a_maxLevel)
		{
			if (a_level < 30) {
				double share = 0.0;
				for (const auto& [level, part] : kSteps) {
					if (a_level >= level) {
						share = part;
					}
				}
				return a_at30 * share;
			}
			const int    top = (std::max)(a_maxLevel, 31);
			const double t = std::clamp(static_cast<double>(a_level - 30) / (top - 30), 0.0, 1.0);
			return a_at30 + (a_at50 - a_at30) * t;
		}

		std::array<double, 5> Targets(int a_level, const Settings& a_s)
		{
			const int top = a_s.progressMaxLevel;
			return { Value(a_level, a_s.biteAt30, a_s.biteAt50, top), Value(a_level, a_s.armorAt30, a_s.armorAt50, top),
				Value(a_level, a_s.magicAt30, a_s.magicAt50, top), Value(a_level, a_s.healthAt30, a_s.healthAt50, top),
				Value(a_level, a_s.speedAt30, a_s.speedAt50, top) };
		}

		int PerkLevel(std::size_t a_slot, const Settings& a_s)
		{
			switch (static_cast<PerkSlot>(a_slot)) {
			case PerkSlot::kSteady1:
				return a_s.steady1Level;
			case PerkSlot::kSteady2:
				return a_s.steady2Level;
			case PerkSlot::kSteady3:
				return a_s.steady3Level;
			default:
				return a_s.bowlingLevel;
			}
		}

		constexpr RE::FormID kHardyHide = 0x80B;  // Hardy Hide (SN_Dog.esp), see UpdateHardiness

		// Takes our perks (the level ones and Hardy Hide) off a base record. The base isn't saved, so without this a
		// perk earned in a later save would still be on him after loading an earlier one.
		bool StripPerks(RE::TESNPC* a_npc)
		{
			const auto data = RE::TESDataHandler::GetSingleton();
			if (!a_npc || !data) {
				return false;
			}
			bool changed = false;
			for (const auto id : { kPerks[0], kPerks[1], kPerks[2], kPerks[3], kHardyHide }) {
				const auto perk = data->LookupForm<RE::BGSPerk>(id, kPlugin);
				if (perk && a_npc->GetPerkIndex(perk) && a_npc->RemovePerk(perk)) {
					changed = true;
				}
			}
			return changed;
		}

		// Perks for NPCs live on the base record: each one is on him exactly while his level has reached its setting.
		void UpdatePerks(RE::Actor* a_dog, int a_level, const Settings& a_s)
		{
			const auto npc = a_dog->GetActorBase();
			const auto data = RE::TESDataHandler::GetSingleton();
			if (!npc || !data) {
				return;
			}
			g_perksOn = npc->GetFormID();
			bool changed = false;
			for (std::size_t i = 0; i < kPerks.size(); ++i) {
				const auto perk = data->LookupForm<RE::BGSPerk>(kPerks[i], kPlugin);
				if (!perk) {
					continue;  // SN_Dog.esp isn't loaded: a dog of another plugin gets the stats only
				}
				const bool want = a_level >= PerkLevel(i, a_s);
				const bool has = npc->GetPerkIndex(perk).has_value();
				if (want && !has && npc->AddPerk(perk, 1)) {
					changed = true;
					logs::info("Progression: {} (level {})", kPerkNames[i], a_level);
				} else if (!want && has && npc->RemovePerk(perk)) {
					changed = true;
					logs::info("Progression: {} taken off (level {} is below {})", kPerkNames[i], a_level, PerkLevel(i, a_s));
				}
			}
			if (changed) {
				a_dog->ApplyPerksFromBase();
			}
		}

		struct Milestone
		{
			int         level;
			const char* line;  // {} = his name
		};
		constexpr std::array<Milestone, 7> kMilestones{ {
			{ 5, "{} has grown into his paws lately; he plants himself more firmly when he stands his ground." },
			{ 10, "{} has filled out, his coat thicker and his shoulders broader than before." },
			{ 15, "{} moves with a new steadiness now, hard to knock off his feet." },
			{ 20, "{} hits like a battering ram when he charges these days." },
			{ 30, "{} has become a formidable beast, a seasoned fighter in his prime." },
			{ 40, "{} keeps growing tougher with every fight; old scars ride under his fur." },
			{ 50, "{} is a legend of a dog now, as hard to bring down as any warrior." },
		} };

		void Announce(RE::Actor* a_dog, int a_level, const Settings& a_s)
		{
			const Milestone* reached = nullptr;
			for (const auto& milestone : kMilestones) {
				if (a_level >= milestone.level && milestone.level > g_state.announced) {
					reached = &milestone;  // the highest new one only, when several are crossed at once
				}
			}
			if (!reached) {
				return;
			}
			const bool first = g_state.announced == 0 && a_level > reached->level;  // a dog already past it on first sight
			g_state.announced = reached->level;
			if (first) {
				return;
			}
			const char* name = a_dog->GetDisplayFullName();
			const auto  line = std::vformat(reached->line, std::make_format_args(name));
			logs::info("Progression: level {} milestone", reached->level);
			Notify::About(a_dog->GetFormID(), "{} has grown stronger.");
			if (const auto player = RE::PlayerCharacter::GetSingleton()) {
				Bridge::PostEvent(line, a_dog->GetFormID(), player->GetFormID(), a_s.narrate);
			}
		}
	}

	namespace
	{
		// His size grows from the setting at level 1 to the one at the top level, evenly.
		float SizeFor(int a_level, const Settings& a_s)
		{
			const int    top = (std::max)(a_s.progressMaxLevel, 2);
			const double t = std::clamp(static_cast<double>(a_level - 1) / (top - 1), 0.0, 1.0);
			return static_cast<float>(a_s.scaleAtStart + (a_s.scaleAtTop - a_s.scaleAtStart) * t);
		}

		std::string SizeWords(RE::Actor* a_dog, float a_scale)
		{
			const char* name = a_dog->GetDisplayFullName();
			const auto  who = name && *name ? name : "He";
			// 1 is an ordinary husky. The steps are finest close to it, where the default range of 0.9 to 1.1 sits.
			if (a_scale < 0.7f) {
				return std::format("{} is a tiny husky, hardly bigger than a fox.", who);
			}
			if (a_scale < 0.85f) {
				return std::format("{} is a small husky, compact and light on his feet.", who);
			}
			if (a_scale < 0.95f) {
				return std::format("{} is a smallish husky, lean and quick.", who);
			}
			if (a_scale < 0.99f) {
				return std::format("{} is a husky a little under the usual size, trim and wiry.", who);
			}
			if (a_scale <= 1.01f) {
				return std::format("{} is a husky of ordinary size, sturdy and well grown.", who);
			}
			if (a_scale < 1.04f) {
				return std::format("{} is a husky a little bigger than most, solid through the shoulders.", who);
			}
			if (a_scale < 1.08f) {
				return std::format("{} is a big husky, broad in the chest and heavy in the paw.", who);
			}
			if (a_scale < 1.15f) {
				return std::format("{} is a very large husky, nearly the size of a wolf, broad and powerful.", who);
			}
			if (a_scale < 1.4f) {
				return std::format("{} is a huge husky, bigger than any wolf, his shoulders level with a person's hip.", who);
			}
			return std::format("{} is an enormous husky, the size of a small bear, and he fills any doorway he stands in.", who);
		}

		void UpdateSize(RE::Actor* a_dog, int a_level, const Settings& a_s)
		{
			const float target = SizeFor(a_level, a_s);
			if (std::abs(a_dog->GetScale() - target) > 0.005f) {
				a_dog->SetScale(target);
				logs::info("Progression: size {:.2f} (level {})", target, a_level);
			}
			g_state.scale = target;
			std::scoped_lock lock(g_sizeLock);
			g_sizeFact = SizeWords(a_dog, target);
		}
	}

	namespace
	{
		// Hardy Hide (SN_Dog.esp): one perk multiplying all damage he takes, whatever the combat overhaul does to armor.
		// Its multiplier is set here from the settings, eased from the level-1 value to the top-level one.
		float                g_hardinessSet = -1.0f;

		void UpdateHardiness(RE::Actor* a_dog, int a_level, const Settings& a_s)
		{
			const auto data = RE::TESDataHandler::GetSingleton();
			const auto perk = data ? data->LookupForm<RE::BGSPerk>(kHardyHide, kPlugin) : nullptr;
			const auto npc = a_dog->GetActorBase();
			if (!perk || !npc) {
				return;
			}
			const int    top = (std::max)(a_s.progressMaxLevel, 2);
			const double t = std::clamp(static_cast<double>(a_level - 1) / (top - 1), 0.0, 1.0);
			const float  multiplier = static_cast<float>((a_s.damageTakenAtStart + (a_s.damageTakenAtTop - a_s.damageTakenAtStart) * t) / 100.0);
			for (const auto entry : perk->perkEntries) {
				if (entry && entry->GetType() == RE::PERK_ENTRY_TYPE::kEntryPoint) {
					if (const auto function = static_cast<RE::BGSEntryPointPerkEntry*>(entry)->functionData) {
						static_cast<RE::BGSEntryPointFunctionDataOneValue*>(function)->data = multiplier;
					}
				}
			}
			if (!npc->GetPerkIndex(perk) && npc->AddPerk(perk, 1)) {
				a_dog->ApplyPerksFromBase();
			}
			if (std::abs(multiplier - g_hardinessSet) > 0.001f) {
				g_hardinessSet = multiplier;
				int entries = 0;
				for (const auto entry : perk->perkEntries) {
					entries += entry && entry->GetType() == RE::PERK_ENTRY_TYPE::kEntryPoint;
				}
				logs::info("Progression: damage taken x{:.2f} (level {}); Hardy Hide on him: {}, on base: {}, entry points {}", multiplier, a_level,
					a_dog->HasPerk(perk), npc->GetPerkIndex(perk).has_value(), entries);
			}
		}
	}

	namespace
	{
		// Takes everything this module put on a dog back off him: stats, size and perks.
		void TakeBack(RE::Actor* a_dog)
		{
			const auto owner = a_dog ? a_dog->AsActorValueOwner() : nullptr;
			if (!owner) {
				return;
			}
			for (std::size_t i = 0; i < kStats.size(); ++i) {
				if (g_state.applied[i] != 0.0f) {
					owner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, kStats[i], -g_state.applied[i]);
					g_state.applied[i] = 0.0f;
				}
			}
			owner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, RE::ActorValue::kCarryWeight, 0.1f);
			owner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, RE::ActorValue::kCarryWeight, -0.1f);
			if (g_state.scale != 0.0f) {
				a_dog->SetScale(1.0f);
				g_state.scale = 0.0f;
			}
			if (StripPerks(a_dog->GetActorBase())) {
				a_dog->ApplyPerksFromBase();
			}
			g_hardinessSet = -1.0f;
			g_state.announced = 0;
			std::scoped_lock lock(g_sizeLock);
			g_sizeFact.clear();
		}

		bool Applied()
		{
			return g_state.scale != 0.0f || std::ranges::any_of(g_state.applied, [](float a_v) { return a_v != 0.0f; });
		}
	}

	int GrowthLevel()
	{
		return (std::max)(1, g_growthLevel.load());
	}

	std::string SizeFact()
	{
		std::scoped_lock lock(g_sizeLock);
		return g_sizeFact;
	}

	void Update(RE::Actor* a_dog, const Settings& a_settings)
	{
		const auto now = Clock::now();
		if (!a_settings.progressEnabled) {
			// Switched off: what he gained comes back off once (it returns when progression is switched on again).
			if (a_dog && g_state.ref == a_dog->GetFormID() && Applied()) {
				TakeBack(a_dog);
				logs::info("Progression: off, the level bonuses taken back off {}", a_dog->GetDisplayFullName());
			}
			return;
		}
		if (!a_dog || now < g_next || !a_dog->Is3DLoaded() || a_dog->IsDead()) {
			return;
		}
		g_next = now + kEvery;
		if (g_state.ref != a_dog->GetFormID()) {
			// A different dog (a new spawn, or the setting changed): take ours back off the old one if he's still
			// around, so he keeps only what he had before; nothing of ours is on the new one yet.
			if (const auto old = g_state.ref ? RE::TESForm::LookupByID<RE::Actor>(g_state.ref) : nullptr; old && !old->IsDeleted()) {
				TakeBack(old);
				logs::info("Progression: took the level bonuses back off {} ({:08X})", old->GetDisplayFullName(), old->GetFormID());
			}
			g_state = State{ a_dog->GetFormID() };
			logs::info("Progression: tracking {} ({:08X}), level {}", a_dog->GetDisplayFullName(), a_dog->GetFormID(), a_dog->GetLevel());
		}
		// His growth keeps up with the player: a spawned or long-known NPC's own level can stop rising (seen at 30 with the
		// player past 45, on a record that scales 1x with the player up to 100).
		const auto player = RE::PlayerCharacter::GetSingleton();
		const int  ownLevel = a_dog->GetLevel();
		const int  playerLevel = player ? player->GetLevel() : 0;
		const int  level = (std::max)(ownLevel, playerLevel);
		g_growthLevel = level;
		static int loggedLevel = -1;
		if (level != loggedLevel) {
			loggedLevel = level;
			logs::info("Progression: growing at level {} (his own level {}, the player's {})", level, ownLevel, playerLevel);
		}
		const auto targets = Targets(level, a_settings);
		const auto owner = a_dog->AsActorValueOwner();
		for (std::size_t i = 0; i < kStats.size(); ++i) {
			const float target = static_cast<float>(targets[i]);
			const float delta = target - g_state.applied[i];
			if (std::abs(delta) < 0.01f || !owner) {
				continue;
			}
			owner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, kStats[i], delta);
			g_state.applied[i] = target;
			if (kStats[i] == RE::ActorValue::kSpeedMult) {
				// A new speed only takes hold when his carry weight changes: nudge it up and back.
				owner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, RE::ActorValue::kCarryWeight, 0.1f);
				owner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, RE::ActorValue::kCarryWeight, -0.1f);
			}
			logs::info("Progression: {} {:+.1f} (now +{:.1f} at level {})", kStatNames[i], delta, target, level);
		}
		UpdatePerks(a_dog, level, a_settings);
		UpdateSize(a_dog, level, a_settings);
		UpdateHardiness(a_dog, level, a_settings);
		Announce(a_dog, level, a_settings);
	}

	void Save(SKSE::SerializationInterface* a_intfc)
	{
		if (!a_intfc->OpenRecord(kRecord, kVersion)) {
			return;
		}
		a_intfc->WriteRecordData(g_state.ref);
		for (const float value : g_state.applied) {
			a_intfc->WriteRecordData(value);
		}
		a_intfc->WriteRecordData(g_state.announced);
		a_intfc->WriteRecordData(g_state.scale);
	}

	bool LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type, std::uint32_t a_version)
	{
		if (a_type != kRecord) {
			return false;
		}
		State state;
		if (a_version < 2 || a_version > kVersion || a_intfc->ReadRecordData(state.ref) != sizeof(state.ref)) {
			return true;
		}
		RE::FormID resolved = 0;
		if (!a_intfc->ResolveFormID(state.ref, resolved)) {
			return true;  // that dog is gone with the plugin that placed him
		}
		state.ref = resolved;
		for (float& value : state.applied) {
			a_intfc->ReadRecordData(value);
		}
		a_intfc->ReadRecordData(state.announced);
		if (a_version >= 3) {
			a_intfc->ReadRecordData(state.scale);
		}
		g_state = state;
		return true;
	}

	void Revert()
	{
		g_state = {};
		// The base record carries over from the game just left: start the loaded one without its perks.
		if (const auto npc = g_perksOn ? RE::TESForm::LookupByID<RE::TESNPC>(g_perksOn) : nullptr) {
			StripPerks(npc);
		}
		g_hardinessSet = -1.0f;
		std::scoped_lock lock(g_sizeLock);
		g_sizeFact.clear();
		g_next = {};
	}

	float VoicePitch(const Settings& a_s)
	{
		const int    top = (std::max)(a_s.progressMaxLevel, 2);
		const double t = std::clamp(static_cast<double>(GrowthLevel() - 1) / (top - 1), 0.0, 1.0);
		return static_cast<float>(a_s.voicePitchStart + (a_s.voicePitchTop - a_s.voicePitchStart) * t);
	}
}
