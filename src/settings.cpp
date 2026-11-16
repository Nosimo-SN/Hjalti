// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#include "settings.h"

#include "bridge.h"

namespace Canine
{
	namespace
	{
		std::mutex g_lock;
		Settings   g_settings;

		constexpr std::array<std::string_view, kTierCount> kTierKeys{ "noticeable", "strong", "urgent" };

		// Default prompt text: facts about the dog's body, not instructions. {name} = the dog, {player} = the player.
		const std::array<std::array<std::string_view, kTierCount>, kDriveCount> kDefaultText{ {
			{ "{name} is getting hungry; his nose follows any smell of food.",
				"{name} is very hungry. His stomach growls, and his nose keeps turning toward any smell of meat or bread.",
				"{name} is starving. Food is all he can think about; he whines and searches every corner for something to eat." },
			{ "{name} wants some attention and keeps drifting close to {player}.",
				"{name} badly wants affection. He presses against {player} and nudges for a scratch.",
				"{name} is starved for affection; he whines softly and won't leave {player}'s side, desperate to be touched." },
			{ "{name} is restless; he paces and sniffs around.",
				"{name} is bursting with energy. He can't keep still, circling and bouncing, eager to run.",
				"{name} is wild with pent-up energy; he dashes about barking, desperate to run and play." },
			{ "{name} is getting sleepy; he yawns, and his steps are slower.",
				"{name} is very tired. His eyelids droop, and he keeps looking for a soft spot to lie down.",
				"{name} is exhausted. He can barely keep his eyes open and wants nothing but to curl up and sleep." },
			{ "{name} feels {player} is too far away and keeps checking which way {player} went.",
				"{name} misses {player} badly; he whines and paces, sniffing the air for {player}'s scent.",
				"{name} is frantic without {player}. He howls and searches everywhere, unable to settle." },
		} };

		bool ParseBool(std::string a_value, bool a_default)
		{
			std::ranges::transform(a_value, a_value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (a_value == "true" || a_value == "1" || a_value == "yes" || a_value == "on") {
				return true;
			}
			if (a_value == "false" || a_value == "0" || a_value == "no" || a_value == "off") {
				return false;
			}
			return a_default;
		}

		bool Bool(const std::string& a_path, bool a_default)
		{
			return ParseBool(Bridge::ConfigValue(a_path, a_default ? "true" : "false"), a_default);
		}

		// Anything unparsable keeps the default.
		double Number(const std::string& a_path, double a_default, double a_min, double a_max)
		{
			double parsed = a_default;
			try {
				parsed = std::stod(Bridge::ConfigValue(a_path, std::to_string(a_default)));
			} catch (...) {
				parsed = a_default;
			}
			return std::clamp(parsed, a_min, a_max);
		}

		std::string Text(const std::string& a_path, std::string_view a_default)
		{
			auto value = Bridge::ConfigValue(a_path, std::string(a_default));
			return value.empty() ? std::string(a_default) : value;
		}
	}

	void ReloadSettings()
	{
		Settings s;
		for (std::size_t d = 0; d < kDriveCount; ++d) {
			for (std::size_t t = 0; t < kTierCount; ++t) {
				s.text[d][t] = kDefaultText[d][t];
			}
		}

		if (Bridge::HasConfig()) {
			s.enabled = Bool("general.enabled", s.enabled);
			s.actor = Text("general.actor", s.actor);

			s.hungerRate = Number("hunger.rate", s.hungerRate, 0.0, 100.0);
			s.affectionRate = Number("affection.rate", s.affectionRate, 0.0, 100.0);
			s.restlessIdleRate = Number("restless.idle_rate", s.restlessIdleRate, 0.0, 100.0);
			s.restlessTravelRate = -Number("restless.travel_drain", -s.restlessTravelRate, 0.0, 200.0);
			s.tiredRate = Number("tired.rate", s.tiredRate, 0.0, 100.0);
			s.tiredNightMult = Number("tired.night_mult", s.tiredNightMult, 1.0, 5.0);
			s.tiredTravelMult = Number("tired.travel_mult", s.tiredTravelMult, 1.0, 5.0);
			s.tiredSleepRate = -Number("tired.sleep_drain", -s.tiredSleepRate, 0.0, 200.0);
			s.tiredNapRate = -Number("tired.nap_drain", -s.tiredNapRate, 0.0, 200.0);
			s.tiredRestlessMult = Number("tired.restless_mult", s.tiredRestlessMult, 0.0, 1.0);
			s.separationApartRate = Number("separation.apart_rate", s.separationApartRate, 0.0, 200.0);
			s.separationNearRate = -Number("separation.near_drain", -s.separationNearRate, 0.0, 200.0);
			s.nearDistance = static_cast<float>(Number("separation.near_distance", s.nearDistance, 200.0, 10000.0));

			s.combatHungerMult = Number("combat.hunger_mult", s.combatHungerMult, 1.0, 5.0);
			s.combatRestlessRate = -Number("combat.restless_drain", -s.combatRestlessRate, 0.0, 300.0);
			s.combatTiredMult = Number("combat.tired_mult", s.combatTiredMult, 1.0, 5.0);

			s.feedPerItem = Number("feeding.per_item", s.feedPerItem, 0.0, 100.0);
			s.feedRawBonus = Number("feeding.raw_bonus", s.feedRawBonus, 0.0, 50.0);
			s.feedMaxMeal = Number("feeding.max_meal", s.feedMaxMeal, 0.0, 100.0);

			s.petHotkey = static_cast<int>(Number("petting.hotkey", s.petHotkey, -1.0, 255.0));
			s.petModifiers = static_cast<int>(Number("petting.modifiers", s.petModifiers, 0.0, 7.0));
			s.petRange = static_cast<float>(Number("petting.range", s.petRange, 100.0, 1000.0));
			s.petOnActivate = Bool("petting.on_activate", s.petOnActivate);
			s.petAffection = Number("petting.affection", s.petAffection, 0.0, 100.0);
			s.petSeparation = Number("petting.separation", s.petSeparation, 0.0, 100.0);
			s.petCooldown = Number("petting.cooldown", s.petCooldown, 0.0, 600.0);
			s.petRequireSneak = Bool("petting.require_sneak", s.petRequireSneak);

			s.events = Bool("events.enabled", s.events);
			s.narrate = Bool("events.narrate", s.narrate);
			s.quietBarks = static_cast<int>(Number("events.quiet_barks", s.quietBarks, 0.0, 2.0));
			s.trainingEnabled = Bool("training.enabled", s.trainingEnabled);
			s.greetEnabled = Bool("events.greet", s.greetEnabled);
			s.progressEnabled = Bool("progression.enabled", s.progressEnabled);
			s.progressMaxLevel = static_cast<int>(Number("progression.max_level", s.progressMaxLevel, 31.0, 200.0));
			s.biteAt30 = Number("progression.bite_30", s.biteAt30, 0.0, 500.0);
			s.biteAt50 = Number("progression.bite_50", s.biteAt50, 0.0, 500.0);
			s.armorAt30 = Number("progression.armor_30", s.armorAt30, 0.0, 2000.0);
			s.armorAt50 = Number("progression.armor_50", s.armorAt50, 0.0, 2000.0);
			s.magicAt30 = Number("progression.magic_30", s.magicAt30, 0.0, 85.0);
			s.magicAt50 = Number("progression.magic_50", s.magicAt50, 0.0, 85.0);
			s.healthAt30 = Number("progression.health_30", s.healthAt30, 0.0, 5000.0);
			s.healthAt50 = Number("progression.health_50", s.healthAt50, 0.0, 5000.0);
			s.speedAt30 = Number("progression.speed_30", s.speedAt30, 0.0, 100.0);
			s.speedAt50 = Number("progression.speed_50", s.speedAt50, 0.0, 100.0);
			s.steady1Level = static_cast<int>(Number("progression.steady1_level", s.steady1Level, 1.0, 200.0));
			s.steady2Level = static_cast<int>(Number("progression.steady2_level", s.steady2Level, 1.0, 200.0));
			s.steady3Level = static_cast<int>(Number("progression.steady3_level", s.steady3Level, 1.0, 200.0));
			s.bowlingLevel = static_cast<int>(Number("progression.bowling_level", s.bowlingLevel, 1.0, 200.0));
			s.scaleAtStart = Number("progression.scale_start", s.scaleAtStart, 0.5, 2.0);
			s.scaleAtTop = Number("progression.scale_top", s.scaleAtTop, 0.5, 2.0);
			s.voicePitchStart = Number("progression.voice_start", s.voicePitchStart, 0.7, 1.5);
			s.voicePitchTop = Number("progression.voice_top", s.voicePitchTop, 0.7, 1.5);
			s.whinePitchShare = Number("progression.voice_whine_share", s.whinePitchShare, 0.0, 1.0);
			s.runesEnabled = Bool("runes.enabled", s.runesEnabled);
			s.soulsPerUnlock = static_cast<int>(Number("runes.souls_per_unlock", s.soulsPerUnlock, 1.0, 20.0));
			s.soulGifts = Bool("runes.gifts", s.soulGifts);
			s.soulWitness = Bool("runes.witness", s.soulWitness);
			s.perseverHeal = Number("runes.persevere_heal", s.perseverHeal, 5.0, 100.0);
			s.perseverCooldown = Number("runes.persevere_cooldown", s.perseverCooldown, 10.0, 1200.0);
			s.frostStep = Bool("runes.frost_step", s.frostStep);
			s.frostStepCooldown = Number("runes.frost_step_cooldown", s.frostStepCooldown, 5.0, 600.0);
			s.frostStepEscapeCooldown = Number("runes.frost_step_escape_cooldown", s.frostStepEscapeCooldown, 10.0, 1200.0);
			s.frostHowl = Bool("runes.frost_howl", s.frostHowl);
			s.frostHowlCooldown = Number("runes.frost_howl_cooldown", s.frostHowlCooldown, 10.0, 1200.0);
			s.frostHowlForce = Number("runes.frost_howl_force", s.frostHowlForce, 0.0, 50.0);
			s.runeMaxGlow = Number("runes.max_glow", s.runeMaxGlow, 0.0, 10.0);
			s.damageTakenAtStart = Number("progression.damage_taken_start", s.damageTakenAtStart, 5.0, 200.0);
			s.damageTakenAtTop = Number("progression.damage_taken_top", s.damageTakenAtTop, 5.0, 200.0);
			s.smellEnabled = Bool("events.smell", s.smellEnabled);
			s.spareBeaten = Bool("combat.spare_beaten", s.spareBeaten);
			s.combatAwareness = Bool("combat.awareness", s.combatAwareness);
			s.takeOwnedFood = Bool("events.take_owned_food", s.takeOwnedFood);
			s.chargePlayer = Bool("events.charge_player", s.chargePlayer);
			s.npcPet = Bool("events.npc_pet", s.npcPet);
			s.npcPetCooldown = Number("events.npc_pet_cooldown", s.npcPetCooldown, 0.0, 3600.0);
			s.hunting = Bool("events.hunting", s.hunting);
			s.huntCooldown = Number("events.hunt_cooldown", s.huntCooldown, 0.0, 600.0);
			s.killBiteMultiple = Number("events.kill_bite_multiple", s.killBiteMultiple, 0.0, 20.0);
			s.carcassMeal = Number("feeding.carcass_meal", s.carcassMeal, 0.0, 100.0);
			s.carcassMeat = static_cast<int>(Number("feeding.carcass_meat", s.carcassMeat, 0.0, 2.0));
			s.smellRange = Number("events.smell_range", s.smellRange, 1000.0, 8000.0);
			s.smellQuiet = Number("events.smell_quiet", s.smellQuiet, 0.0, 3600.0);
			s.singCooldown = Number("events.sing_cooldown", s.singCooldown, 0.0, 600.0);
			s.trainingHalfLife = Number("training.half_life", s.trainingHalfLife, 0.0, 1000.0);
			s.leakRepair = Bool("events.leak_repair", s.leakRepair);
			s.notify = Bool("events.notify", s.notify);

			s.loopEnabled = Bool("loop.enabled", s.loopEnabled);
			s.loopMinSeconds = Number("loop.min_seconds", s.loopMinSeconds, 15.0, 1800.0);
			s.loopMaxSeconds = Number("loop.max_seconds", s.loopMaxSeconds, 15.0, 1800.0);
			if (s.loopMinSeconds > s.loopMaxSeconds) {
				std::swap(s.loopMinSeconds, s.loopMaxSeconds);
			}
			s.loopQuietSeconds = Number("loop.quiet_seconds", s.loopQuietSeconds, 0.0, 300.0);
			s.loopConfidence = Number("loop.confidence", s.loopConfidence, 0.0, 1.0);

			s.perceptionEnabled = Bool("perception.enabled", s.perceptionEnabled);
			s.praiseAffection = Number("perception.praise_affection", s.praiseAffection, 0.0, 100.0);
			s.scoldAffection = Number("perception.scold_affection", s.scoldAffection, 0.0, 100.0);
			s.talkFeed = Bool("perception.talk_feed", s.talkFeed);
			s.perceptionConfidence = Number("perception.confidence", s.perceptionConfidence, 0.0, 1.0);

			s.dreamsEnabled = Bool("dreams.enabled", s.dreamsEnabled);
			s.dreamNapMinutes = Number("dreams.nap_minutes", s.dreamNapMinutes, 0.0, 60.0);

			for (std::size_t t = 0; t < kTierCount; ++t) {
				s.tierThresholds[t] = Number("tiers." + std::string(kTierKeys[t]), s.tierThresholds[t], 1.0, 100.0);
			}
			// Keep the tiers in order even if someone sets them crosswise.
			std::ranges::sort(s.tierThresholds);
			s.promptDrives = static_cast<int>(Number("prompt.max_drives", s.promptDrives, 1.0, 5.0));

			for (std::size_t d = 0; d < kDriveCount; ++d) {
				for (std::size_t t = 0; t < kTierCount; ++t) {
					const auto path = "text." + std::string(kDriveKeys[d]) + "_" + std::string(kTierKeys[t]);
					s.text[d][t] = Text(path, kDefaultText[d][t]);
				}
			}

			s.logChanges = Bool("debug.log_changes", s.logChanges);
			s.logBarks = Bool("debug.log_barks", s.logBarks);
			s.logCombat = Bool("debug.log_combat", s.logCombat);
			s.debugSet = Bridge::ConfigValue("debug.set", "");
		}

		std::scoped_lock lock(g_lock);
		g_settings = std::move(s);
	}

	Settings SnapshotSettings()
	{
		std::scoped_lock lock(g_lock);
		return g_settings;
	}
}
