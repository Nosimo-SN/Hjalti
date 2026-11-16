// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#pragma once

#include "settings.h"

namespace Canine::Mood
{
	// His mood for the animation conditions (OAR), kept on an actor value only he carries:
	//   ones digit = strongest drive at or above "noticeable" (0 content, 1 hunger, 2 affection, 3 restless,
	//   4 tired, 5 separation); +10 while Quiet Barks is on (his bark clips are swapped for quiet ones);
	//   20 + mood for a few seconds while we have him bark on purpose (the plain bark clips play, whatever else is installed);
	//   30 + sleep style while he sleeps (30 relax, 31 guarded, 32 cold; see Sleep): from the moment a nap begins until
	//   he is back on his feet (that style's clips and still head/tail overlays play; head tracking is off). The style is
	//   picked once at the start and the code outlasts the nap until he stands, so the get-up clip matches the pose he
	//   is in, and nothing else (mood, barks, weather) can swap the clips mid-sleep.
	//   45 while he tilts his head, curious (standing or sitting): the talk-bark idles play, their clips swapped (silent).
	//   46 while he sniffs the ground (standing): the standing talk bark plays, its clip swapped for the sniff.
	//   47 while he sniffs the air (standing): the same, swapped for the air sniff.
	//   48 while he sneezes (standing): the same, swapped for the sneeze.
	//   49 while he barks angrily (standing or sitting): the talk barks swapped for angry-bark copies.
	//   50 while he rolls over (standing to standing): the warning idle again, its clip swapped for the rollover; head
	//   tracking off for it, so his head doesn't turn toward someone while he's upside down.
	//   40 while he play bows: the vanilla warning idle plays, and OAR swaps its clip (AggroWarning2) for the bow.
	//   41 + sleep style while he lies in an awake pose (41 on his side, 42 chin on paws, 43 curled): the sleep style's
	//   body clips with his eyes open, head tracking left on. Held until he's on his feet, as with sleep.
	inline constexpr auto kActorValue = RE::ActorValue::kVariable08;
	inline constexpr int  kSleepingCode = 30;
	inline constexpr int  kPlayBowCode = 40;
	inline constexpr int  kPoseCode = 41;
	inline constexpr int  kWiggleCode = 44;  // the happy wiggle: the warning idle again, its clip swapped for the wiggle
	inline constexpr int  kCuriousCode = 45;  // the curious head tilt: the talk-bark idles, their clips swapped for the tilt
	inline constexpr int  kGroundSniffCode = 46;  // nose to the ground, working a scent: the standing talk bark, swapped
	inline constexpr int  kAirSniffCode = 47;     // nose up, sampling the air in three rising sniffs: the same carrier
	inline constexpr int  kSneezeCode = 48;       // a sneeze: the same carrier
	inline constexpr int  kAngryBarkCode = 49;    // an angry bark: the talk-bark clips, swapped for angry-bark copies
	inline constexpr int  kRolloverCode = 50;     // a full rollover: the warning idle, its clip swapped for the roll

	// Game thread, once per tick.
	void Update(RE::Actor* a_dog, const Settings& a_settings);

	// Game thread, when a nap begins: the sleeping code is written now, before the lie-down idle plays, because OAR
	// picks the lie-down clip the moment it starts.
	void BeginSleep(RE::Actor* a_dog);

	// Game thread. Lifts the quiet flag for a few seconds, so a bark we play on purpose isn't swapped for a quiet move.
	// Ignored while he sleeps (the sleeping code holds).
	void AllowBark(RE::Actor* a_dog, std::chrono::milliseconds a_for);

	// Game thread. Holds the play bow code for a_for (the clip's length), written now, before the warning idle plays,
	// since OAR picks the clip as it starts. Ignored (false) while he sleeps.
	bool PlayBow(RE::Actor* a_dog, std::chrono::milliseconds a_for);
	// The same for the happy wiggle (code 44).
	bool Wiggle(RE::Actor* a_dog, std::chrono::milliseconds a_for);
	// The same for the rollover (code 50); head tracking is off while it's held.
	bool Rollover(RE::Actor* a_dog, std::chrono::milliseconds a_for);
	// The same for the curious head tilt (code 45).
	bool CuriousTilt(RE::Actor* a_dog, std::chrono::milliseconds a_for);
	// The same for the ground sniff (code 46).
	bool GroundSniff(RE::Actor* a_dog, std::chrono::milliseconds a_for);
	// The same for the air sniff (code 47).
	bool AirSniff(RE::Actor* a_dog, std::chrono::milliseconds a_for);
	// The same for the sneeze (code 48).
	bool Sneeze(RE::Actor* a_dog, std::chrono::milliseconds a_for);
	// The same for the angry bark (code 49).
	bool AngryBark(RE::Actor* a_dog, std::chrono::milliseconds a_for);

	// Game thread, when an awake pose begins: writes its code now, before the lie-down idle plays. Held while the pose
	// is on and until he stands. Ignored (false) while he sleeps.
	bool BeginPose(RE::Actor* a_dog, int a_code);

	// On every load: forget what was last written (so it is written again) and any bark window.
	void ResetSession();
}
