// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#pragma once

// Vanilla idles, played through the actor's AI process (what Papyrus Actor.PlayIdle does). A bare animation
// graph event is ignored while a follower's AI owns his idles; the idle request goes through.
namespace Canine::Idles
{
	// Skyrim.esm
	inline constexpr RE::FormID kDogLay = 0x0010F7FE;        // IdleDogLay
	inline constexpr RE::FormID kDogSit = 0x0010F7FF;        // IdleDogSit
	inline constexpr RE::FormID kDogBarkSit = 0x000BF209;    // DogBarkSittingTalk
	inline constexpr RE::FormID kDogBarkStand = 0x000BF207;  // DogBarkStandingTalk
	inline constexpr RE::FormID kSearchBody = 0x000EFC64;    // IdleSearchBody (the player kneels)
	inline constexpr RE::FormID kStopLoose = 0x0010D9EE;     // IdleStop_Loose

	// Game thread. Falls back to the raw graph event when the idle can't be requested.
	inline bool Play(RE::Actor* a_actor, RE::FormID a_idle, const char* a_fallbackEvent)
	{
		if (!a_actor) {
			return false;
		}
		const auto idle = RE::TESForm::LookupByID<RE::TESIdleForm>(a_idle);
		const auto process = a_actor->GetActorRuntimeData().currentProcess;
		if (idle && process && process->PlayIdle(a_actor, idle, nullptr)) {
			return true;
		}
		return a_actor->NotifyAnimationGraph(a_fallbackEvent);
	}
}
