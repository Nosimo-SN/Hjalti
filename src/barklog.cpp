// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#include "barklog.h"

#include <chrono>

namespace Canine::BarkLog
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		std::string Lower(std::string_view a_text)
		{
			std::string out(a_text);
			std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		}

		bool Noisy(std::string_view a_text)
		{
			const auto low = Lower(a_text);
			for (const auto word : { "sound"sv, "bark"sv, "voice"sv, "howl"sv, "growl"sv, "whine"sv, "yip"sv, "snarl"sv, "attack"sv, "power"sv,
					 "hitframe"sv, "swing"sv }) {
				if (low.find(word) != std::string::npos) {
					return true;
				}
			}
			return false;
		}

		std::string IdleName(const RE::TESIdleForm* a_idle)
		{
			if (!a_idle) {
				return "none";
			}
			const char* editorId = a_idle->GetFormEditorID();
			return std::format("{} ({:08X})", editorId && *editorId ? editorId : "?", a_idle->GetFormID());
		}

		const RE::TESIdleForm* LastIdle(RE::Actor* a_dog)
		{
			const auto process = a_dog ? a_dog->GetActorRuntimeData().currentProcess : nullptr;
			const auto middle = process ? process->middleHigh : nullptr;
			return middle ? middle->lastIdlePlayed : nullptr;
		}

		// Animation events arrive on whatever thread runs his graph, so this only reads and logs.
		class Sink final : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
			{
				if (!a_event) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const std::string_view tag = a_event->tag.c_str() ? a_event->tag.c_str() : "";
				const std::string_view payload = a_event->payload.c_str() ? a_event->payload.c_str() : "";
				if (!Noisy(tag) && !Noisy(payload)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				// The same event twice within a second and a half is one bark.
				std::string key = std::string(tag) + "|" + std::string(payload);
				const auto  now = Clock::now();
				{
					std::scoped_lock lock(m_lock);
					if (key == m_lastKey && now - m_lastAt < 1500ms) {
						return RE::BSEventNotifyControl::kContinue;
					}
					m_lastKey = std::move(key);
					m_lastAt = now;
				}
				const auto dog = a_event->holder ? a_event->holder->As<RE::Actor>() : nullptr;
				const auto owner = dog ? dog->AsActorValueOwner() : nullptr;
				logs::info("Anim log: '{}'{} combat={} stamina {:.0f}", tag, payload.empty() ? ""s : std::format(" payload '{}'", payload),
					dog && dog->IsInCombat(), owner ? owner->GetActorValue(RE::ActorValue::kStamina) : -1.0f);
				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			std::mutex        m_lock;
			std::string       m_lastKey;
			Clock::time_point m_lastAt{};
		};

		Sink                   g_sink;
		RE::ActorHandle        g_registered;  // a handle: the actor may be gone by the time we look again
		const RE::TESIdleForm* g_lastIdle = nullptr;
		bool                   g_announced = false;
	}

	void Detach()
	{
		if (const auto old = g_registered.get()) {
			old->RemoveAnimationGraphEventSink(&g_sink);
		}
		g_registered.reset();
		g_lastIdle = nullptr;
	}

	void Update(RE::Actor* a_dog, bool a_enabled)
	{
		const bool want = a_enabled && a_dog && a_dog->Is3DLoaded();
		if (!want) {
			Detach();
			return;
		}
		if (!g_announced) {
			g_announced = true;
			logs::info("Anim log: on (sound, bark and attack animation events)");
		}
		// Re-added every tick: his graph is rebuilt when his 3D reloads, and adding a sink twice is a no-op.
		if (g_registered && g_registered.get().get() != a_dog) {
			Detach();
		}
		a_dog->AddAnimationGraphEventSink(&g_sink);
		g_registered = RE::ActorHandle(a_dog);

		const auto idle = LastIdle(a_dog);
		if (idle && idle != g_lastIdle) {
			logs::info("Bark log: idle {} combat={}", IdleName(idle), a_dog->IsInCombat());
		}
		g_lastIdle = idle;
	}
}
