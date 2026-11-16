// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#pragma once

#include "settings.h"

// What he has learned from the player's praise and scolding. Each of his own actions keeps two slowly fading
// tallies, pleased and cross; a praise or scolding heard soon after an action counts toward that action. The numbers
// never reach a prompt: only plain learned associations do, and only once the lean is clear.
namespace Canine::Training
{
	// Game thread: he just did this ("play", "sniff", "beg", "nap", "seek_affection", "eat").
	void Did(std::string_view a_action);
	// Game thread: a praise (true) or scolding (false) was heard. Counted toward his last action when it was recent.
	void Feedback(bool a_praise, const Settings& a_settings);
	// Game thread, from the tick: fades the tallies with game time and rebuilds the wording.
	void Update(double a_hoursPassed, RE::FormID a_ref, RE::FormID a_base, std::string_view a_player, const Settings& a_settings);
	void ResetSession();

	// The learned associations as prompt text ("" when nothing is clear yet). Thread-safe.
	std::string Text();
	// Decorator: Text() for the tracked dog, "" for anyone else. SkyrimNet worker threads.
	std::string DecoratorLessons(RE::Actor* a_actor);

	// --- co-save (game thread) ---
	inline constexpr std::uint32_t kRecord = 'TRAN';
	void Save(SKSE::SerializationInterface* a_intfc);
	void LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version);
	void Revert();
}
