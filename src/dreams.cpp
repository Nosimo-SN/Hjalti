// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#include "dreams.h"

#include "bridge.h"
#include "drives.h"
#include "notify.h"
#include "training.h"

#include "jsonfield.h"

namespace Canine::Dreams
{
	namespace
	{
		constexpr double kNapGapHours = 6.0;  // a nap brings no dream this soon after the last one
		constexpr int    kEvents = 10;
		constexpr int    kMemories = 8;
		constexpr auto   kPrompt = "canine_dream";

		std::atomic_bool g_asking{ false };
		double           g_lastDream = -1.0;  // game hours; game thread

		std::string Name(RE::TESObjectREFR* a_ref, const char* a_fallback)
		{
			const char* name = a_ref ? a_ref->GetDisplayFullName() : nullptr;
			return name && *name ? name : a_fallback;
		}

		std::string LocationName(RE::Actor* a_dog)
		{
			if (const auto location = a_dog->GetCurrentLocation(); location && location->GetFullName() && *location->GetFullName()) {
				return location->GetFullName();
			}
			if (const auto cell = a_dog->GetParentCell(); cell && cell->GetFullName() && *cell->GetFullName()) {
				return cell->GetFullName();
			}
			return "the wilds";
		}

		// SkyrimNet worker: no RE:: calls until the task. Each dream goes into his event history as a persistent event
		// (SkyrimNet's persistent_generic, via Papyrus RegisterPersistentEvent): recent events reach his dialogue prompt
		// whatever the topic, where a memory only came up when retrieval judged it relevant.
		void Store(RE::FormID a_dog, std::uint32_t a_session, bool a_ok, const std::string& a_reply)
		{
			g_asking = false;
			if (!a_ok) {
				return;
			}
			// The model may wrap the JSON in prose or a code fence: take the outermost braces.
			const auto open = a_reply.find('{');
			const auto close = a_reply.rfind('}');
			const auto json = open != std::string::npos && close != std::string::npos && close > open ?
			                      nlohmann::json::parse(a_reply.substr(open, close - open + 1), nullptr, false) :
			                      nlohmann::json();
			// {"dreams": [...]} as asked; a lone {"dream": ...} (seen when only one was asked for) counts as one.
			auto dreams = json.is_object() && json.contains("dreams") && json["dreams"].is_array() ? json["dreams"] : nlohmann::json::array();
			if (dreams.empty() && json.is_object() && json.contains("dream")) {
				dreams.push_back(json);
			}
			std::vector<std::pair<std::string, std::string>> found;  // dream, emotion
			for (const auto& entry : dreams) {
				if (auto dream = JsonField::Text(entry, "dream"); !dream.empty()) {
					found.emplace_back(std::move(dream), JsonField::Text(entry, "emotion", "content"));
				}
			}
			if (found.empty()) {
				logs::warn("Dream: no dream in the reply '{}'", a_reply);
				return;
			}
			SKSE::GetTaskInterface()->AddTask([a_dog, a_session, found = std::move(found)] {
				const auto dog = RE::TESForm::LookupByID<RE::Actor>(a_dog);
				if (a_session != Bridge::Session() || !dog || !dog->Is3DLoaded()) {
					logs::info("Dream: he's gone or a load began, {} dream(s) dropped", found.size());
					return;
				}
				const auto name = Name(dog, "The dog");
				for (const auto& [dream, emotion] : found) {
					Bridge::PostEvent(std::format("{} dreamt in his sleep ({}): {}", name, emotion, dream), a_dog, 0);
					logs::info("Dream stored as an event ({}): {}", emotion, dream);
				}
				Notify::About(a_dog, found.size() == 1 ? "{} dreamt in his sleep."s : std::format("{{}} dreamt {} dreams in his sleep.", found.size()));
			});
		}
	}

	void Request(RE::Actor* a_dog, std::string_view a_when, int a_count)
	{
		const auto s = SnapshotSettings();
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto calendar = RE::Calendar::GetSingleton();
		if (!s.dreamsEnabled || !a_dog || a_dog->IsDead() || !player || !calendar || g_asking) {
			return;
		}
		const double now = calendar->GetHoursPassed();
		if (a_when == "nap" && g_lastDream >= 0.0 && now - g_lastDream < kNapGapHours) {
			logs::info("Dream (nap): he dreamt {:.1f} game hours ago, skipped", now - g_lastDream);
			return;
		}

		nlohmann::json ctx;
		ctx["count"] = (std::max)(1, a_count);
		ctx["dogFormId"] = a_dog->GetFormID();
		ctx["playerName"] = Name(player, "the player");
		ctx["when"] = a_when;
		const auto location = LocationName(a_dog);
		ctx["location"] = location;
		const auto cell = a_dog->GetParentCell();
		ctx["indoors"] = cell && cell->IsInteriorCell();
		ctx["needs"] = Drives::DecoratorNeeds(a_dog);
		for (std::size_t d = 0; d < kDriveCount; ++d) {
			ctx[std::string(kDriveKeys[d])] = Drives::Peek(a_dog, static_cast<Drive>(d)).value_or(0);
		}
		ctx["recentEvents"] = Bridge::RecentEvents(a_dog->GetFormID(), kEvents);
		ctx["memories"] = Bridge::RecentMemories(a_dog->GetFormID(), kMemories);
		ctx["lessons"] = Training::Text();

		g_asking = true;
		const bool sent = Bridge::AskPrompt(kPrompt, ctx.dump(),
			[dog = a_dog->GetFormID(), session = Bridge::Session()](bool a_ok, std::string a_reply) {
				if (session != Bridge::Session()) {
					logs::info("Dream: reply from before the last load, dropped");
					return;  // not stored into the save now loaded
				}
				try {
					Store(dog, session, a_ok, a_reply);
				} catch (const std::exception& e) {
					g_asking = false;
					logs::warn("Dream: reply could not be read ({})", e.what());
				}
			});
		if (!sent) {
			g_asking = false;
			logs::warn("Dream: the prompt could not be sent");
			return;
		}
		g_lastDream = now;
		logs::info("Dream ({}): asked for {}", a_when, (std::max)(1, a_count));

		if (s.events && a_dog->Is3DLoaded()) {
			Bridge::PostEvent(std::format("{} stirs with a twitch of his paws and a muffled, sleepy woof, as if he had been dreaming.",
								  Name(a_dog, "The dog")),
				a_dog->GetFormID(), 0);
		}
	}

	void ResetSession()
	{
		g_lastDream = -1.0;
		g_asking = false;  // a reply that never came must not block dreams for good
	}
}
