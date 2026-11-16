// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#pragma once

// Hjalti's own sounds. The OAR clips carry "SoundPlay.CanineDrives<cue>" annotations: an annotation only becomes an
// animation event if the behavior graph knows its name, and SoundPlay is one (the rest arrives as the payload). This
// listens on his graph and plays the cue's sound descriptor from SN_Dog.esp on him: a 3D sound (the vanilla dog sniff's
// output model, SOMMono00900) that comes from him and fades with distance, under the Effects volume, picking one of its
// three takes (Sound\FX\CanineDrives). Played as files with no record (until 2026-11-14) they were flat 2D, full volume.
// The descriptors' editor IDs end in SD so the engine's own SoundPlay doesn't also find and play them.
//   CanineDrivesSneeze    CanineDrivesSneezeSD    000819  dog_sneeze1-3, 3 dB attenuation
//   CanineDrivesSniff     CanineDrivesSniffSD     00081A  sniff_short1-3 (0.3 s), 6.61 dB (as the vanilla sniff)
//   CanineDrivesSniffBig  CanineDrivesSniffBigSD  00081B  sniff_big1-3 (0.4-0.6 s), 5 dB
//   CanineDrivesBark      CanineDrivesBarkSD      00081C  bark_happy1-3, 0 dB, the vanilla bark's reverb model
//   CanineDrivesBarkAngry CanineDrivesBarkAngrySD 00081D  bark_angry1-3, 0 dB
//   CanineDrivesGrowl     CanineDrivesGrowlSD     00081E  growl1-3 (1.45 s), 1 dB, the vanilla growl's model
//   CanineDrivesWhine     CanineDrivesWhineSD     00081F  whine_pout + whine_sad (2.5 s), 2 dB
//   (actions only)        CanineDrivesWhinePoutSD 000820  whine_pout;  CanineDrivesWhineSadSD 000821  whine_sad
// The vocal markers come from DogAnimKit voice_markers.py (vanilla per-breed events rewritten, for him only).
// For the sneeze, if the event hasn't come 0.8 s after the clip started, it plays on that timer and says so in the log.
namespace Canine::SoundCues
{
	enum class Cue
	{
		kSneeze,
		kSniff,
		kSniffBig,
		kBark,       // happy bark
		kBarkAngry,
		kGrowl,
		kWhine,      // either whine (the clips' whines)
		kWhinePout,
		kWhineSad,
		kEat,       // chewing (CanineDrivesEatSD 000822)
		kFrostStep, // the soul gifts, at the moment each starts: Frost Step 000823, Frost Howl 000824, Perseverance 000825
		kFrostHowl,
		kPerseverance
	};

	// Game thread, once per tick: keeps the listener on his graph (rebuilt when his 3D reloads), runs the sneeze timer.
	void Update(RE::Actor* a_dog);
	// Game thread, before a save loads: let go of his graph while the old world still exists.
	void Detach();
	// Game thread: play a take of the cue on him now.
	void Play(RE::Actor* a_dog, Cue a_cue);
	// Game thread: a sneeze clip is starting now; its sound is due at the marker (fallback timer armed).
	void ExpectSneeze(RE::Actor* a_dog);
	// Testing: play every voice sound at this pitch instead of his level's (0 = back to his level's).
	void OverridePitch(float a_pitch);
	// Game thread: chewing while he eats: the eat sound now and again as each one ends, for a_for. It stops early once he
	// isn't lying down any more (checked from 2.5 s in, after the lie-down), or when another meal starts.
	void Chew(RE::Actor* a_dog, std::chrono::milliseconds a_for);
}
