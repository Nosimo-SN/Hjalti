// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#pragma once

#include "settings.h"

#include "CanineDrivesAPI.h"

namespace Canine::Drives
{
	// What the tick saw this step. Built on the game thread.
	struct Context
	{
		double hours = 0.0;       // game hours since the last step
		double sleepHours = 0.0;  // part of those hours the dog slept
		bool   napping = false;   // that sleep was his own nap (the nap rate applies), not the player's sleep
		bool   isNear = false;    // close to the player
		bool   moving = false;    // travelling
		bool   combat = false;
		bool   night = false;
	};

	// --- game thread ---
	void   Reset();
	void   Step(const Context& a_ctx, const Settings& a_settings);
	void   Adjust(Drive a_drive, double a_delta, const Settings& a_settings);  // one-off change (eating, petting)
	double Value(Drive a_drive);
	bool   ApplyDebugSet(std::string_view a_command);  // "hunger=80, tired=90" or "all=50"
	// Rebuilds what the decorators and the C API read. a_dog / a_player are display names for the prompt text;
	// a_activity is a CDAPI_ACTIVITY_* value.
	void Publish(RE::FormID a_ref, RE::FormID a_base, std::string_view a_dog, std::string_view a_player, std::string_view a_state,
		std::uint32_t a_activity, const Settings& a_settings);
	std::string Describe();  // one-line dump for the log

	double LastHours();
	void   SetLastHours(double a_hours);

	// --- co-save (SKSE calls these on the game thread) ---
	void Save(SKSE::SerializationInterface* a_intfc);
	// One record from the plugin's load loop; false when it isn't the drives record.
	bool LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type, std::uint32_t a_version);
	void Revert(SKSE::SerializationInterface* a_intfc);

	// --- decorators (SkyrimNet worker threads; read the published snapshot only) ---
	std::string        DecoratorValue(RE::Actor* a_actor, Drive a_drive);
	std::optional<int> Peek(RE::Actor* a_actor, Drive a_drive);  // nullopt for other actors
	std::string DecoratorNeeds(RE::Actor* a_actor);
	std::string DecoratorState(RE::Actor* a_actor);

	// --- C API (any thread; copies the published snapshot only, touches no game objects) ---
	// False until a snapshot is published (and again after a save load / new game reset), or for another reference.
	bool ApiSnapshot(std::uint32_t a_ref, CDAPI_SnapshotV1& a_out);
}
