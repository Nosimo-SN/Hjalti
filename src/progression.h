// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#pragma once

#include "settings.h"

// The dog grows with his level: bite damage, armor, magic resistance and health rise in steps up to level 30 and
// smoothly after that, and his own perks (in SN_Dog.esp) unlock at set levels. Stat bonuses are permanent actor-value
// changes the co-save keeps track of, so each is applied once and adjusted when the settings or his level change.
namespace Canine::Progression
{
	// Game thread, every tick.
	void Update(RE::Actor* a_dog, const Settings& a_settings);

	// His size as a sentence for his prompt (empty until he has been seen). Thread-safe.
	std::string SizeFact();

	// The level his growth uses (the higher of his own and the player's), once he has been seen; else 1. Thread-safe.
	int GrowthLevel();
	// His voice's playback pitch for his growth level (barks and growls; 1 = as recorded). Thread-safe.
	float VoicePitch(const Settings& a_settings);

	// Co-save.
	void Save(SKSE::SerializationInterface* a_intfc);
	bool LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type, std::uint32_t a_version);
	void Revert();
}
