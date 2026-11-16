// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#pragma once

// Diagnostics for a dog that freezes or jumps (2026-11-14: after a load he stood frozen mid-walk, no head
// tracking, ahead of the player on a path, with his AI on and a travel package that never moved him).
//   - Load snapshot: his position, AI, process level, graph update, package and states, logged when he's
//     first loaded after a load and again a few seconds later.
//   - Jump log: a move of hundreds of units in one tick that wasn't ours.
//   - Stuck watch: he should be moving (Play/Sniff/Fish is running him, or he's far behind with his graph
//     locked animation-driven, or the player walked off while he looks frozen) but hasn't moved. Steps up:
//     re-evaluate his package and reset his graph, then disable and enable him (what freed him on 2026-11-14:
//     the graph had kept bAnimationDriven and bDisableHeadTrack from an idle that was gone), then move him to
//     the player. Every step is logged with a snapshot, and so is the step that got him going again.
namespace Canine::Watchdog
{
	// Game thread, every tick (after Actions::Process).
	void Update(RE::Actor* a_dog, RE::PlayerCharacter* a_player);

	// Game thread: a one-line snapshot of his state, for the log.
	std::string Describe(RE::Actor* a_dog, RE::PlayerCharacter* a_player);

	void ResetSession();
}
