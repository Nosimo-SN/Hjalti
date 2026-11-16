// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#pragma once

#include "settings.h"

// Soul gifts: what the dragon souls he witnessed give him, on the same count and schedule as the runes (the first with
// the first soul, then one every "Souls per Rune"). In order: Frost Bite I, Perseverance, Frost Step, Frost Bite II,
// quicker cooldowns, Frost Bite III. Frost Bite is a perk in SN_Dog.esp that applies a frost spell on every bite (its
// strength is set here by tier); Perseverance and Frost Step run here.
namespace Canine::Gifts
{
	// Game thread, every tick.
	void Update(RE::Actor* a_dog, const Settings& a_settings);

	// What his gifts are, for his prompt (empty without any). Thread-safe.
	std::string Fact();

	// Game thread: the frost damage his bite carries now (0 without Frost Bite).
	float FrostBiteDamage();

	// Game thread: Frost Step is ready and this one is where it could take him, were he fighting them.
	bool StepWithin(RE::Actor* a_dog, RE::Actor* a_target);

	void ResetSession();

	// kDataLoaded: watch his hits, so a frost bite lights the runes.
	void RegisterEvents();
}
