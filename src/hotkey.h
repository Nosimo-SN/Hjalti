// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#pragma once

// The petting hotkey. Polled with GetAsyncKeyState on its own thread, so it takes the same Windows
// virtual-key codes as SkyrimNet's dashboard hotkeys; a press is handed to the game thread as a task.
namespace Hotkey
{
	void Start(std::function<void()> a_onPress);           // once, at kDataLoaded
	void Configure(int a_virtualKey, int a_modifierMask);  // -1 = off; mask: 1 Ctrl, 2 Shift, 4 Alt
}
