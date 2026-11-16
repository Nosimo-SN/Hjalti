// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#include "hotkey.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace Hotkey
{
	namespace
	{
		constexpr auto kPollInterval = 30ms;

		std::atomic_int g_key{ -1 };
		std::atomic_int g_modifiers{ 0 };

		bool Down(int a_virtualKey)
		{
			return (GetAsyncKeyState(a_virtualKey) & 0x8000) != 0;
		}

		// Only while Skyrim itself has the keyboard: typing in another window never pets the dog.
		bool GameHasFocus()
		{
			DWORD processId = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &processId);
			return processId == GetCurrentProcessId();
		}

		// Exactly the requested modifiers: Shift+P must not fire on Ctrl+Shift+P.
		bool ModifiersMatch(int a_mask)
		{
			return Down(VK_CONTROL) == ((a_mask & 1) != 0) &&
			       Down(VK_SHIFT) == ((a_mask & 2) != 0) &&
			       Down(VK_MENU) == ((a_mask & 4) != 0);
		}
	}

	void Start(std::function<void()> a_onPress)
	{
		std::thread([onPress = std::move(a_onPress)] {
			bool wasDown = false;
			for (;;) {
				std::this_thread::sleep_for(kPollInterval);
				const int key = g_key.load();
				if (key <= 0) {
					wasDown = false;
					continue;
				}
				const bool down = Down(key);
				if (down && !wasDown && GameHasFocus() && ModifiersMatch(g_modifiers.load())) {
					onPress();
				}
				wasDown = down;
			}
		}).detach();
	}

	void Configure(int a_virtualKey, int a_modifierMask)
	{
		g_key = a_virtualKey;
		g_modifiers = a_modifierMask;
	}
}
