// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#pragma once

// Dog dreams: after a night beside the player or a long nap, the default model writes a short dream from his side
// (prompts/canine_dream.prompt) and it is stored as one of his SkyrimNet memories.
namespace Canine::Dreams
{
	// Game thread. a_when is "night" (a_count dreams, one request) or "nap" (one dream, skipped if he dreamt in the
	// last few game hours). Does nothing when dreams are off.
	void Request(RE::Actor* a_dog, std::string_view a_when, int a_count = 1);
	void ResetSession();
}
