// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#pragma once

namespace Canine
{
	enum class Drive : std::size_t
	{
		kHunger,
		kAffection,
		kRestless,
		kTired,
		kSeparation,
		kTotal
	};
	inline constexpr std::size_t kDriveCount = static_cast<std::size_t>(Drive::kTotal);

	// Prompt tiers above "content" (which renders nothing).
	enum class Tier : std::size_t
	{
		kNoticeable,
		kStrong,
		kUrgent,
		kTotal
	};
	inline constexpr std::size_t kTierCount = static_cast<std::size_t>(Tier::kTotal);

	// Short keys used in settings paths, decorator names and debug commands.
	inline constexpr std::array<std::string_view, kDriveCount> kDriveKeys{ "hunger", "affection", "restless", "tired", "separation" };

	struct Settings
	{
		bool        enabled = true;
		std::string actor = "SN_Dog.esp|0x800";  // the dog, "Plugin.esp|0xLocalID": a placed reference, or an NPC record (any reference of it)

		// Rise per game hour (higher = stronger need).
		double hungerRate = 4.0;
		double affectionRate = 3.0;
		double restlessIdleRate = 10.0;
		double restlessTravelRate = -25.0;  // while on the move
		double tiredRate = 3.0;
		double tiredNightMult = 1.5;
		double tiredTravelMult = 1.5;
		double tiredSleepRate = -15.0;    // while asleep
		double tiredNapRate = -30.0;      // while napping on his own
		double tiredRestlessMult = 0.2;   // idle restlessness rise while tiredness is strong
		double separationApartRate = 20.0;
		double separationNearRate = -30.0;
		float  nearDistance = 1500.0f;
		float  movingSpeed = 60.0f;  // units per real second that count as travelling

		// Combat (while he is fighting).
		double combatHungerMult = 1.5;
		double combatRestlessRate = -60.0;
		double combatTiredMult = 2.0;

		// Eating: hunger removed per food item, more for raw meat, capped per meal.
		double feedPerItem = 25.0;
		double feedRawBonus = 5.0;
		double feedMaxMeal = 70.0;

		// Petting: a hotkey near him (Windows virtual-key code, like SkyrimNet's own hotkeys), or activating him.
		int    petHotkey = -1;     // -1 = off
		int    petModifiers = 2;   // bitmask: 1 = Ctrl, 2 = Shift, 4 = Alt; all must be held, no others
		float  petRange = 250.0f;  // units
		bool   petOnActivate = false;
		bool   petRequireSneak = false;  // activation only
		double petAffection = 25.0;
		double petSeparation = 15.0;
		double petCooldown = 60.0;  // real seconds

		// Short SkyrimNet events for eating and petting, so the people nearby (and he) know.
		bool events = true;
		// Those events (and his own actions) as narration: SkyrimNet has him react instead of quietly noting them.
		bool narrate = true;
		// Which of his bark idles are switched off for him: 0 none, 1 the random ones, 2 also the talking barks (combat
		// barks always stay). At 2 our own happy barks are played as a sound.
		int quietBarks = 1;

		// Greet: when he isn't following and the player comes back after a while, he runs over to them.
	bool greetEnabled = true;
	// Smell: hostile creatures or people near him become a fact in his prompt.
	bool smellEnabled = true;
	// Combat: stamina a power attack costs him (creatures pay none in vanilla), and letting go of a beaten enemy.
	bool   spareBeaten = true;
	// Food: he may take food that belongs to someone (lying on their table, or out of their pack).
	bool takeOwnedFood = true;
	// ...and when he does, it counts as the player's theft (a dog can't be fined, so the owner comes after his person).
	bool chargePlayer = true;
	// NPCs right beside him may kneel where they stand and pet him (no travel, so it never touches their packages).
	bool   npcPet = true;
	double npcPetCooldown = 180.0;  // real seconds between pets from anyone
	// Hunting: he runs down game, and eats from carcasses.
	bool   hunting = true;
	double huntCooldown = 10.0;  // real minutes between hunts
	// Prey whose health right now is at most this many of his bites (Frost Bite included) dies to his first bite;
	// anything tougher is a real fight.
	double killBiteMultiple = 3.0;
	double carcassMeal = 50.0;   // hunger a carcass takes off
	// Whose bodies count as food: 0 animals only, 1 people too when the player wears the Ring of Namira or is a werewolf,
	// 2 people too, always. Never children, whatever this says.
	int    carcassMeat = 0;
	double smellRange = 4000.0;  // units; he reacts out loud within three quarters of it
	double smellQuiet = 300.0;   // seconds after a scent's moment before another new one is news (a close threat always is)
	// Sing: real minutes before he may howl along with a bard again.
	double singCooldown = 30.0;

	// Progression: what he gains with his level, stepped up to level 30 and smooth from there to the top level.
	bool   progressEnabled = true;
	int    progressMaxLevel = 50;
	double biteAt30 = 12.0, biteAt50 = 20.0;        // unarmed damage added
	double armorAt30 = 160.0, armorAt50 = 250.0;    // armor rating added
	double magicAt30 = 30.0, magicAt50 = 50.0;      // magic resistance, percent
	double healthAt30 = 150.0, healthAt50 = 300.0;  // health added
	double speedAt30 = 10.0, speedAt50 = 20.0;      // movement speed, percent
	int    steady1Level = 5, steady2Level = 15, steady3Level = 30, bowlingLevel = 20;
	double scaleAtStart = 0.9, scaleAtTop = 1.1;
	// His voice deepens as he grows: playback pitch of his barks and growls at level 1 and at the top level (1 = as
	// recorded), on the same curve as his size. Whines move by this share of it (0 = never change).
	double voicePitchStart = 1.12, voicePitchTop = 0.94, whinePitchShare = 0.5;
	bool   runesEnabled = true;    // runes on his coat from witnessed dragon souls (SN_Dog's Hjalti only)
	int    soulsPerUnlock = 2;     // witnessed souls for each new rune stage and soul gift
	bool   soulGifts = true;       // the soul gifts (Frost Bite, ...)
	bool   soulWitness = true;     // the nearest bystander answers the soul narration (else SkyrimNet picks)
	double perseverHeal = 40.0;    // Perseverance: percent of his health knitted back
	double perseverCooldown = 120.0;  // Perseverance: seconds before it can come again
	bool   frostStep = true;       // Frost Step: he crosses a gap to a far foe, or to the player when badly hurt
	double frostStepCooldown = 30.0;        // Frost Step at a foe: seconds before it can come again
	double frostStepEscapeCooldown = 90.0;  // Frost Step to the player: seconds before it can come again
	bool   frostHowl = true;          // Frost Howl: swarmed or badly hurt, he howls and the foes around him are knocked down
	double frostHowlCooldown = 90.0;  // seconds before it can come again
	double frostHowlForce = 2.0;      // PushActorAway force: near zero drops them where they stand
	bool   combatAwareness = true; // his prompt lists who's in the fight around him and how they're doing
	double runeMaxGlow = 3.0;      // the emissive multiple at the strongest glow (a dragon soul)
	double damageTakenAtStart = 80.0, damageTakenAtTop = 50.0;  // percent of incoming damage he takes, level 1 / top level  // his size at level 1 and at the top level (1 = normal)

	// Training: praise and scolding soon after one of his actions become learned associations in his prompts.
		bool   trainingEnabled = true;
		double trainingHalfLife = 72.0;  // game hours for a lesson to fade to half
		// After a spoken line of his that has words in it, a quiet event saying it reached only the player's mind.
		bool leakRepair = true;

		// Behavior loop: in quiet moments the decisions model picks what he does next.
		bool   loopEnabled = true;
		double loopMinSeconds = 45.0;
		double loopMaxSeconds = 90.0;
		double loopQuietSeconds = 20.0;  // no dialogue from anyone for this long
		double loopConfidence = 0.5;

		// Hearing: the decisions model reads the player's lines to him as praise, scolding or an offer of food.
		bool   perceptionEnabled = true;
		double praiseAffection = 10.0;
		double scoldAffection = 5.0;  // a scolding raises his need for affection a little
		bool   talkFeed = true;
		double perceptionConfidence = 0.6;

		// Top-left notifications when he settles for a nap, wakes from one, or keeps a dream.
		bool notify = true;

		// Dreams: after sleeping beside the player (or a long nap) a dream goes into his recent events.
		bool   dreamsEnabled = true;
		double dreamNapMinutes = 5.0;    // real minutes lying down before a nap can bring a dream; 0 = naps never do

		std::array<double, kTierCount> tierThresholds{ 40.0, 65.0, 85.0 };
		int                            promptDrives = 2;

		// Prompt text per drive and tier; {name} = the dog's name.
		std::array<std::array<std::string, kTierCount>, kDriveCount> text{};

		bool        logChanges = false;
		bool        logBarks = true;  // diagnostic: where his barks come from
		bool        logCombat = false;  // diagnostic: hits he takes, his swings, his combat state
		std::string debugSet;  // "hunger=80, tired=90", applied once each time it changes
	};

	void     ReloadSettings();
	Settings SnapshotSettings();
}
