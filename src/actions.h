// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#pragma once

#include "settings.h"
#include "sleep.h"

// The dog's own SkyrimNet actions. Bridge registers them; eligibility runs on SkyrimNet workers (reads the
// published drive snapshot only), execution on the game thread.
namespace Canine::Actions
{
	struct Spec
	{
		const char*                         name;
		const char*                         description;
		std::function<bool(RE::Actor*)>     eligible;
		std::function<bool(RE::Actor*)>     execute;
		bool                                topLevel = false;  // listed by name in the dialogue prompt, not inside DogBehavior
		// An action that takes parameters: their JSON schema, and an execute that gets SkyrimNet's params (used
		// instead of execute when set).
		const char*                                         schema = "{}";
		std::function<bool(RE::Actor*, const std::string&)> executeWithParams;
	};

	void              BuildFoodList();  // kDataLoaded: every food item, for the gift menu filter
	std::vector<Spec> All();
	bool              IsDog(RE::Actor* a_actor);  // thread-safe
	bool              Warming();  // game thread: he's pressed against the player, warming them
	void              RegisterEvents();  // kDataLoaded: the bleedout event
	bool              Downed();  // he's in bleedout (lying hurt); thread-safe
	// Game thread: downed, or below a quarter of his health. Too hurt to beg, scavenge or go back into a fight.
	bool Hurt(RE::Actor* a_dog);
	bool              SongToJoin(RE::Actor* a_dog);  // a bard's song he hasn't howled along with yet, cooldown passed
	bool              FoodInReach();  // food lying loose near him (scanned every few seconds), for the behavior loop
	bool              FishInReach();  // fish swimming close by he could catch, fishing cooldown passed (scanned too)
	bool              PreyInReach();  // game he could take on, hunt cooldown passed (scanned too)
	bool              CarcassInReach();  // a body near him he'd eat from (scanned too)
	std::string       ForageFact();  // any thread: game or a carcass that has just caught his nose, or ""
	// Any thread: this person is standing right beside him, free to pet him, and the cooldown has passed.
	bool              CanPetHim(RE::Actor* a_actor);
	// Game thread: they kneel where they stand and scratch his ears. They never walk anywhere.
	void              PettedByNpc(RE::FormID a_actor);
	// Game thread: he catches a new scent. A threat gets a growl (the dog's warning animation, with its own sound),
	// prey an eager whine; either way a short narration says which way he's facing.
	void ScentAlert(RE::Actor* a_dog, bool a_threat, std::string_view a_where);

	// Game thread: the behavior loop's version of an action, with no menu ("beg", "nap", "seek_affection", "play",
	// "sniff"). Play and Sniff post their own event (Sniff names who he went to).
	bool Perform(RE::Actor* a_dog, std::string_view a_behavior);

	// Game thread, from the tick: keeps Play and Sniff moving and ends them; ends a nap when he gets up, fights, or is rested.
	void Process(RE::Actor* a_dog, bool a_moving, const Settings& a_settings);
	// Game thread: ends a nap (he's spoken to or petted) and turns his AI back on. a_speech: his own line,
	// ignored just after the nap began, since choosing Nap in a conversation comes with a line.
	void Wake(const char* a_why, bool a_speech = false);
	// Game thread: he drops into a play bow (bark, wagging, two whines; 4.3 s), the vanilla warning idle with its clip
	// swapped by OAR for mood code 40. Also his own random idle while restless is high. False when he can't now.
	bool PlayBow(RE::Actor* a_dog, const char* a_why);
	// Game thread: the happy wiggle (5.9 s; tail, then the whole back end, growing stronger), the warning idle with its clip swapped by OAR
	// for code 44. On greeting, on being petted, or as a dialogue action. False when he can't now (lying, sitting, napping,
	// or did it in the last 15 s).
	bool Wiggle(RE::Actor* a_dog, const char* a_why);
	// A full playful rollover (code 50): lies down, rolls onto his left side, over his back onto his right side, gathers his
	// feet and gets up (6.6 s). As a dialogue action, a random restless idle now and then, or Set Drives 'rollover'. From a
	// sit he stands first. False when he can't now (lying, napping, down, fighting, swimming, not loaded, or just did).
	bool Rollover(RE::Actor* a_dog, const char* a_why);
	// Game thread: the curious head tilt (4.3 s; to his left, then his right, then back; silent), standing or sitting, the
	// talk-bark idle with its clip swapped by OAR for code 45. When something he's watching or someone talking has his
	// close attention. False when he can't now (lying, napping, down, fighting, or did it in the last 20 s).
	bool CuriousTilt(RE::Actor* a_dog, const char* a_why);
	// Game thread: nose to the ground, working a scent left and right (4.8 s, sniffing), the standing talk-bark idle with
	// its clip swapped by OAR for code 46; from a sit he gets up first. With a_then (a food run: Scavenge, EatCarcass) the
	// run starts as his head comes up; if he can't sniff now, the run starts at once. Without it: the SniffGround action,
	// not within 30 s of the last one.
	bool GroundSniff(RE::Actor* a_dog, const char* a_why, bool (*a_then)(RE::Actor*) = nullptr);
	// Game thread: the air sniff (4.75 s; three sniffs, each with the nose higher), the same carrier under code 47. With
	// a_then (Hunt) the run starts as his head comes back down; without it: the SniffAir action, not within 30 s.
	bool AirSniff(RE::Actor* a_dog, const char* a_why, bool (*a_then)(RE::Actor*) = nullptr);
	// Game thread: a sneeze (2.1 s; a small wind-up, the snap, a tiny head shake), the same carrier under code 48; the
	// sound plays on the clip's SoundPlay.CanineDrivesSneeze event (SoundCues). From a sit he gets up first. Not within 60 s.
	bool Sneeze(RE::Actor* a_dog, const char* a_why);
	// His voice on its own, as dialogue actions (his own sounds, SN_Dog.esp): a happy bark (the deliberate bark), an angry
	// bark (the talk bark under code 49), a warning growl (the warning stance standing; the growl alone when he sits or
	// lies), a pouting whine or a sad/hurt whine (the sound alone). Game thread. False when he can't now.
	enum class Vocal
	{
		kHappyBark,
		kAngryBark,
		kGrowl,
		kPout,
		kSad
	};
	bool Voice(RE::Actor* a_dog, Vocal a_kind, const char* a_why);
	void ResumeAfterLoad(RE::Actor* a_dog);  // game thread: turns his AI back on if a mid-nap save left it off
	bool Napping();                 // a nap is on AND his graph says he's lying (isIdleLay)
	bool NapOn();                   // a nap is on, lying yet or not (game thread)
	bool PoseOn();                  // a lying pose (awake body language) is on (thread-safe)
	// Game thread: he lies down in that style's pose, awake (no nap, AI and head tracking on), holds it 45-90 s, then
	// gets up. False when he can't now (napping, already lying, down, not loaded).
	bool Pose(RE::Actor* a_dog, Sleep::Style a_style);
	int  FrostfallExposure();       // game thread: the player's Frostfall exposure level, -1 when Frostfall isn't running
	bool Roaming();                 // a roam (Play, Sniff, Hunt, Carcass, ...) is running him (thread-safe)
	std::uint32_t RoamActivity();   // game thread: the running roam as a CDAPI_ACTIVITY_* value, 0 with none
	bool RoamHoldsStill();          // ...and has him still on purpose right now: at a carcass, sniffing or eating (thread-safe)
	// Game thread: posts this event line (as narration when that's on) once the nap he just began has settled, i.e.
	// his AI is paused; dropped if the nap ends first.
	void TellWhenSettled(std::string a_line);
	// Any thread: one of his own lines (how many voiced words: text and *actions*, no thoughts or tags), so AskForFood
	// can let it play before opening the gift menu.
	void HeardDogLine(int a_voicedWords);
	// Game thread, every tick: which of his bark idles are switched off (0 none, 1 the random ones, 2 all but combat);
	// only acts when that changes.
	void QuietIdleBarks(RE::Actor* a_dog, int a_level);
	// Game thread: one of our deliberate happy barks. With every bark idle off it's the bark sound, else the bark idle.
	void Bark(RE::Actor* a_dog, bool a_sitting);
	void BarkSound(RE::Actor* a_dog);  // the husky bark as a sound only (what Quiet Barks 2 plays)
	void WhineSound(RE::Actor* a_dog);  // the husky whine as a sound only
	void HowlSound(RE::Actor* a_dog);   // the wolf howl he sings with
	bool IsLying(RE::Actor* a_dog);  // game thread
	// Debug: logs every animation graph variable of the actor that isn't zero, to find his lying-down state.
	void DumpGraph(RE::Actor* a_actor, std::string_view a_label);
	void ResetSession();
}
