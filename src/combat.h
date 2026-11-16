// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#pragma once

#include "settings.h"

// His fighting: he lets go of an enemy who
// yields or goes down, or when the player calls him off.
namespace Canine::Combat
{
	// Game thread, every tick.
	void Update(RE::Actor* a_dog, const Settings& a_settings);

	// Game thread: the player told him to leave it; he stops attacking for a few seconds.
	void CallOff(RE::Actor* a_dog);

	// Game thread: the player set him on someone by name (the AttackTarget action). He goes for the best match within
	// range, never the player or their followers, and keeps at them, past yielding or going down and past an earlier
	// "leave it", until they're dead, the order runs out or the player calls him off. False if nobody matched.
	bool Order(RE::Actor* a_dog, std::string_view a_name);

	// Game thread: he pounces on this prey at the end of his own hunt. It stands like an order for a_for. With
	// a_killingBite (prey within his kill-bite multiple) his first bite kills it, so the fight is over before his companions can join
	// in; a predator is a real fight.
	void Hunt(RE::Actor* a_dog, RE::Actor* a_prey, std::chrono::seconds a_for, bool a_killingBite);
	// Game thread: his killing bite landed on this prey. The game credits a follower's kill to the player, so the corpse's
	// killer can't say whether it was his.
	bool BitToDeath(RE::FormID a_prey);
	// Game thread: the player has told him to leave it, just now.
	bool CalledOff();
	// Game thread: the prey he's after, or 0 once the hunt is over or an order or a "leave it" replaced it.
	RE::FormID Hunted();
	// Game thread: the hunt ends and he lets it go.
	void EndHunt(RE::Actor* a_dog, const char* a_why);

	// Any thread: who is in the fight around him and how they're doing, as prompt text; empty out of combat.
	std::string Awareness();

	// Game thread: any standing order ends (he escaped, badly hurt); he isn't called off and can still fight.
	void DropOrder(const char* a_why);

	// A child (the actor's own test or a child race). He never fights them or takes food from them.
	bool IsChild(RE::Actor* a_actor);

	// Game thread: this one is a fair target for him right now (not a child, not spared or down, he isn't called off, and
	// they're fighting us or he was ordered at them).
	bool Engaged(RE::Actor* a_dog, RE::Actor* a_target);

	void ResetSession();
	// Game thread, before a save loads: let go of his animation graph while the old world still exists.
	void Detach();
}
