// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#pragma once

#include "settings.h"

// Things that happen to the dog: food arriving in his inventory, being petted. Event sinks only note
// them; the tick applies them on the game thread when the moment is right.
namespace Canine::Inputs
{
	void Register();  // event sinks; call at kDataLoaded
	void SetTarget(RE::FormID a_ref);
	void ResetSession();

	// Game thread, from the tick: eats pending food, applies pets. a_player is the player's display name.
	void Process(RE::Actor* a_dog, std::string_view a_dogName, std::string_view a_player, const Settings& a_settings);

	// Game thread: the petting hotkey was pressed.
	void PetByHotkey(RE::Actor* a_dog, std::string_view a_dogName, std::string_view a_player, const Settings& a_settings);

	// Game thread: the player offered him food in words. Moves the best matching food from the player's pack to his
	// inventory (he then eats it like any meal). False when there was nothing to give or it was on its way already.
	bool FeedFromPlayer(RE::Actor* a_dog, std::string_view a_line);

	// Any thread: the player just offered food in words, so talk-to-feed will hand it over shortly.
	void PromiseFood();
	// Game thread: a meal he took himself (put in his inventory by us); the matching item-added event, if any, is skipped.
	void AddMeal(RE::FormID a_item, std::int32_t a_count);
	// Game thread: he's eating something that never came through his inventory (a carcass).
	void NoteEating();
	// Any thread: food was promised or arrived a moment ago, so the gift menu isn't needed.
	bool FoodComing();

	// What he's doing right now, for canine_state: "eating" for a little while after a meal, else "awake".
	std::string_view State();
}
