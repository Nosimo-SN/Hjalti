// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#pragma once

namespace Canine::BarkLog
{
	// Diagnostic: logs each idle he starts and each sound/bark animation event his graph sends, so a bark heard in
	// game can be traced to where it came from. Game thread, once per tick.
	void Update(RE::Actor* a_dog, bool a_enabled);
	// Game thread, before a save loads: let go of his animation graph while the old world still exists.
	void Detach();
}
