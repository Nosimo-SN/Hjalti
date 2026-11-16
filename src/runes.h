// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#pragma once

#include "settings.h"

// Dragon souls and the runes on his coat. Each dragon soul the player takes while he is close by counts as one he
// witnessed (kept in the co-save); every "Souls per Unlock" of them works another rune into his coat (four stages, each a
// skin in SN_Dog.esp). The runes stay dark, and light up only at moments: a soul taken, a soul gift used, a power
// attack, a threat on the air, a song, or faintly while he fights. The glow is the body shader's emissive multiple.
namespace Canine::Runes
{
	enum class Flare
	{
		kSoul,         // a dragon soul taken near him: the strongest
		kGift,         // one of his soul gifts used (for the gifts to come)
		kPowerAttack,  // a power attack
		kScent,        // a threat on the air
		kSong,         // howling along with a song
		kBite,         // a frost bite landing
	};

	// Game thread, every tick.
	void Update(RE::Actor* a_dog, const Settings& a_settings);

	// Game thread: the dragon souls he has witnessed.
	int Souls();

	// The step reached after a_souls witnessed souls: the first with the first soul, then one every a_per (capped).
	int Step(int a_souls, int a_per, int a_cap);

	// Any thread: light the runes for a moment.
	void Light(Flare a_kind);

	// Where the runes are and what they are doing, for his prompt (empty without runes). Thread-safe.
	std::string Fact();

	// Game thread, debug.set: "souls=N" sets the souls he has witnessed; "runes" lights them as a soul would.
	bool ApplyDebugSet(std::string_view a_text);

	void ResetSession();

	// Co-save.
	constexpr std::uint32_t kRecord = 'RUNE';
	void                    Save(SKSE::SerializationInterface* a_intfc);
	bool                    LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type, std::uint32_t a_version);
	void                    Revert();
}
