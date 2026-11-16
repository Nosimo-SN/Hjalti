// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#pragma once

#include "settings.h"

// What the dog picks up from around him: hostile scents nearby, a bard performing, and what he's doing right now,
// as plain facts for his prompt.
namespace Canine::Senses
{
	// Once SkyrimNet's API is up: listens for SkyrimNet bards' sung lines.
	void Start();

	// On every load: no scent, music or warming carried over, no song remembered.
	void ResetSession();

	// Game thread, every tick.
	void Update(RE::Actor* a_dog, const Settings& a_settings);

	// A bard performing near him (vanilla bard scene or a SkyrimNet song). Thread-safe.
	struct Song
	{
		RE::FormID    bard = 0;
		std::uint32_t id = 0;  // changes with every new song
	};
	std::optional<Song> SongNearby();

	// Decorator: the facts of this moment for his prompt (scent, music, warming), empty for other actors.
	std::string DecoratorNow(RE::Actor* a_actor);
}
