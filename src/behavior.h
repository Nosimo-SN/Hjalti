// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#pragma once

// The dog's life between conversations, and the guard on his voice.
//  - Behavior loop: in quiet moments (no dialogue, no fighting, no menus, the player close by) the decisions
//    model picks what he does next from his drives, and the plugin does it and posts a quiet event.
//  - Leak repair: a spoken line of his with words in it gets a quiet follow-up event saying the words reached
//    only the player's mind, so nobody nearby remembers a talking dog.
namespace Canine::Behavior
{
	void Start();  // kDataLoaded, after the SkyrimNet bridge is up
	void SetTarget(RE::FormID a_ref);
	void SetInGame(bool a_inGame);
	void ResetSession();
}
