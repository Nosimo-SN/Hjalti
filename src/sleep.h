// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#pragma once

// How he sleeps, picked once when a nap begins (so the pose can't change mid-sleep) and shown to the
// animation conditions as sleeping code 30 + style (see Mood):
//   relax   - safe places (towns, homes, inns): flat on his side, legs stretched out
//   guarded - dungeons and the wilds: the plain lie-down, head resting on his front paws
//   cold    - cold (Frostfall exposure, SeverActions cold, or snow outdoors without either): curled up tight
namespace Canine::Sleep
{
	enum class Style : int
	{
		kRelax = 0,
		kGuarded = 1,
		kCold = 2,
	};

	std::string_view          Name(Style a_style);
	std::optional<Style>      Parse(std::string_view a_name);  // "relax", "guarded", "cold"

	// Game thread: the style for a nap starting now. a_why says what decided it, for the log.
	Style Choose(RE::Actor* a_dog, std::string& a_why);

	// Debug: the next nap uses this style, whatever the place or weather (consumed by Choose).
	void Force(std::optional<Style> a_style);

	// Game thread, every tick: keeps the SeverActions cold reading fresh (it can only be asked asynchronously).
	void Update(RE::Actor* a_dog);

	void ResetSession();
}
