// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#pragma once

// The only door to SkyrimNet. PublicAPI.h defines globals, so it is included in bridge.cpp alone.
namespace Bridge
{
	// Resolves the SkyrimNet API. Call once at kDataLoaded. Returns the API version, or 0 when absent.
	int  Init();

	// A number that changes with every game load and new game. A reply or delayed task that started in an earlier
	// session compares it and drops itself, so nothing from one save acts in another. Thread-safe.
	std::uint32_t Session();
	void          NewSession();
	bool HasConfig();

	std::string ConfigValue(const std::string& a_path, const std::string& a_default);

	// Registers the canine_* decorators. Returns how many were registered.
	int RegisterDecorators();
	// Registers the dog's actions under the DogBehavior category. Returns how many were registered.
	int RegisterActions();

	// Calls a_onLine(type, speakerFormID, targetFormID, text) for every spoken line, telepathy included. The target
	// is 0 when the line has none. Runs on SkyrimNet workers: no RE:: calls inside.
	using LineHandler = std::function<void(std::string_view a_type, RE::FormID a_speaker, RE::FormID a_target, std::string_view a_text)>;
	void WatchDialogue(LineHandler a_onLine);
	// SkyrimNet's background lines that are sung (its bards' songs, one lyric line at a time): the singer's FormID.
	void WatchSinging(std::function<void(RE::FormID a_singer)> a_onLyric);

	// Asks the decisions model one question from prompts/decisions/<a_template>.prompt. a_onAnswer runs on a
	// SkyrimNet worker with the chosen key and its confidence (0-1), or ok = false.
	using DecisionHandler = std::function<void(bool a_ok, std::string a_answer, double a_confidence)>;
	bool AskDecision(const char* a_template, const std::string& a_contextJson, DecisionHandler a_onAnswer);

	// Renders prompts/<a_prompt>.prompt with the context and sends it to the default model. a_onReply runs on a
	// SkyrimNet worker with the reply text, or ok = false.
	using ReplyHandler = std::function<void(bool a_ok, std::string a_reply)>;
	bool AskPrompt(const char* a_prompt, const std::string& a_contextJson, ReplyHandler a_onReply);

	// The actor's recent events as short "Name: what happened" lines, oldest first.
	std::string RecentEvents(RE::FormID a_actor, int a_max);

	// The actor's newest memories as "- text" lines, oldest first. Dreams and plain knowledge are left out.
	std::string RecentMemories(RE::FormID a_actor, int a_max);

	// Stores a memory for the actor (PublicAddMemory). Any thread. Returns false when SkyrimNet refused it.
	bool AddMemory(RE::FormID a_actor, const std::string& a_text, float a_importance, const char* a_type,
		const std::string& a_emotion, const std::string& a_location, const std::string& a_tagsJson);

	bool IsBusy(RE::FormID a_actor);  // SkyrimNet's own busy flag (an action running, a scene)

	// Posts a persistent event (a quiet fact people nearby perceive; it doesn't force a reply), or with a_narrate a
	// direct narration, which SkyrimNet answers: the originator reacts, to the target when there is one.
	// Game thread; the actors must be loaded.
	void PostEvent(std::string a_line, RE::FormID a_originator, RE::FormID a_target, bool a_narrate = false);
}
