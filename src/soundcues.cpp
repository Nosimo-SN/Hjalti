// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#include "soundcues.h"

#include "progression.h"


namespace Canine::SoundCues
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		struct Set
		{
			const char* name;
			const char* payload;
			RE::FormID  descriptor;  // in SN_Dog.esp
		};
		constexpr std::array<Set, 13> g_sets{ {
			{ "sneeze", "CanineDrivesSneeze", 0x000819 },
			{ "sniff", "CanineDrivesSniff", 0x00081A },
			{ "big sniff", "CanineDrivesSniffBig", 0x00081B },
			{ "bark", "CanineDrivesBark", 0x00081C },
			{ "angry bark", "CanineDrivesBarkAngry", 0x00081D },
			{ "growl", "CanineDrivesGrowl", 0x00081E },
			{ "whine", "CanineDrivesWhine", 0x00081F },
			{ "pouting whine", "CanineDrivesWhinePout", 0x000820 },
			{ "sad whine", "CanineDrivesWhineSad", 0x000821 },
			{ "chewing", "CanineDrivesEat", 0x000822 },
			{ "Frost Step", "CanineDrivesFrostStep", 0x000823 },
			{ "Frost Howl", "CanineDrivesFrostHowl", 0x000824 },
			{ "Perseverance", "CanineDrivesPerseverance", 0x000825 },
		} };
		constexpr auto kPlugin = "SN_Dog.esp"sv;
		constexpr auto kSneezeFallback = std::chrono::milliseconds(800);  // the sneeze marker sits at 0.70 s

		std::optional<Clock::time_point> g_sneezeDue;  // game thread: a sneeze is under way, its sound not played yet
		float                            g_pitchOverride = 0.0f;  // testing (Set Drives 'pitch'); 0 = his level's

		// Chewing: the eat sound is 2.56 s; the next one starts as it ends.
		constexpr auto kChewEvery = std::chrono::milliseconds(2500);
		// The chewing stops early only if he walks off from his meal (2026-11-15: it used to stop when he wasn't lying
		// down 2.5 s in, and a lie-down that didn't take left one crunch).
		constexpr float kChewWalkedOff = 120.0f;
		struct Chewing
		{
			RE::FormID        dog = 0;
			Clock::time_point start{}, next{}, until{};
			RE::NiPoint3      at{};
		};
		std::optional<Chewing> g_chew;

		// His voice deepens as he grows (Progression::VoicePitch): barks and growls fully, whines by a share, sneezes and
		// sniffs never.
		float PitchFor(Cue a_cue)
		{
			switch (a_cue) {
			case Cue::kBark:
			case Cue::kBarkAngry:
			case Cue::kGrowl:
				return g_pitchOverride > 0.0f ? g_pitchOverride : Progression::VoicePitch(SnapshotSettings());
			case Cue::kWhine:
			case Cue::kWhinePout:
			case Cue::kWhineSad:
				{
					const auto s = SnapshotSettings();
					const float full = g_pitchOverride > 0.0f ? g_pitchOverride : Progression::VoicePitch(s);
					return 1.0f + (full - 1.0f) * static_cast<float>(s.whinePitchShare);
				}
			default:
				return 1.0f;
			}
		}
		RE::FormID                       g_sneezeDog = 0;

		std::optional<Cue> CueFor(std::string_view a_tag, std::string_view a_payload)
		{
			for (std::size_t i = 0; i < g_sets.size(); ++i) {
				const std::string_view p = g_sets[i].payload;
				if ((a_tag == "SoundPlay"sv && a_payload == p) || (a_tag.starts_with("SoundPlay."sv) && a_tag.substr(10) == p)) {
					return static_cast<Cue>(i);
				}
			}
			return std::nullopt;
		}

		// Animation events arrive on whatever thread runs his graph: only note the event and hand the sound to the
		// game thread.
		class Sink final : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
			{
				const std::string_view tag = a_event && a_event->tag.c_str() ? a_event->tag.c_str() : "";
				const std::string_view payload = a_event && a_event->payload.c_str() ? a_event->payload.c_str() : "";
				const auto             cue = tag.starts_with("SoundPlay"sv) ? CueFor(tag, payload) : std::nullopt;
				if (!cue) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto dog = a_event->holder ? a_event->holder->As<RE::Actor>() : nullptr;
				if (const auto id = dog ? dog->GetFormID() : 0) {
					SKSE::GetTaskInterface()->AddTask([id, cue = *cue] {
						if (cue == Cue::kSneeze) {
							g_sneezeDue.reset();  // heard: the fallback stands down
							logs::info("Sound: sneeze marker heard");
						}
						Play(RE::TESForm::LookupByID<RE::Actor>(id), cue);
					});
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		Sink            g_sink;
		RE::ActorHandle g_registered;
	}

	void Play(RE::Actor* a_dog, Cue a_cue)
	{
		const auto& set = g_sets[static_cast<std::size_t>(a_cue)];
		const auto  data = RE::TESDataHandler::GetSingleton();
		const auto  descriptor = data ? data->LookupForm<RE::BGSSoundDescriptorForm>(set.descriptor, kPlugin) : nullptr;
		const auto  audio = RE::BSAudioManager::GetSingleton();
		const auto  node = a_dog ? a_dog->Get3D() : nullptr;
		if (!descriptor || !audio || !node) {
			logs::info("Sound: {} not played ({})", set.name,
				!descriptor ? "its sound record isn't in SN_Dog.esp" : !audio ? "no audio manager" : "he isn't loaded");
			return;
		}
		RE::BSSoundHandle handle;
		if (!audio->GetSoundHandle(handle, descriptor) || !handle.IsValid()) {
			logs::warn("Sound: no handle for the {} record ({:06X}:{})", set.name, set.descriptor, kPlugin);
			return;
		}
		handle.SetPosition(a_dog->GetPosition());
		handle.SetObjectToFollow(node);
		const float pitch = PitchFor(a_cue);
		const bool  pitched = pitch == 1.0f || handle.SetFrequency(pitch);
		logs::info("Sound: {} {}{}", set.name, handle.Play() ? "played" : "refused to play",
			pitch == 1.0f ? ""s : std::format(" at pitch {:.2f}{}", pitch, pitched ? "" : " (the pitch wasn't taken)"));
	}

	void OverridePitch(float a_pitch)
	{
		g_pitchOverride = a_pitch;
		logs::info("Sound: voice pitch {}", a_pitch > 0.0f ? std::format("fixed at {:.2f} for testing", a_pitch) : "follows his level again"s);
	}

	void Chew(RE::Actor* a_dog, std::chrono::milliseconds a_for)
	{
		if (!a_dog) {
			return;
		}
		const auto now = Clock::now();
		g_chew = Chewing{ a_dog->GetFormID(), now, now + kChewEvery, now + a_for, a_dog->GetPosition() };
		Play(a_dog, Cue::kEat);
	}

	void ExpectSneeze(RE::Actor* a_dog)
	{
		if (a_dog) {
			g_sneezeDue = Clock::now() + kSneezeFallback;
			g_sneezeDog = a_dog->GetFormID();
		}
	}

	void Detach()
	{
		if (const auto old = g_registered.get()) {
			old->RemoveAnimationGraphEventSink(&g_sink);
		}
		g_registered.reset();
	}

	void Update(RE::Actor* a_dog)
	{
		if (g_chew) {
			const auto now = Clock::now();
			const auto dog = RE::TESForm::LookupByID<RE::Actor>(g_chew->dog);
			if (!dog || dog->IsDead() || now >= g_chew->until || dog->GetPosition().GetDistance(g_chew->at) > kChewWalkedOff) {
				g_chew.reset();
			} else if (now >= g_chew->next) {
				g_chew->next = now + kChewEvery;
				Play(dog, Cue::kEat);
			}
		}
		if (g_sneezeDue && Clock::now() >= *g_sneezeDue) {
			g_sneezeDue.reset();
			logs::info("Sound: the sneeze clip's marker didn't arrive, played on the timer");
			Play(RE::TESForm::LookupByID<RE::Actor>(g_sneezeDog), Cue::kSneeze);
		}
		if (!a_dog || !a_dog->Is3DLoaded()) {
			Detach();
			return;
		}
		if (g_registered && g_registered.get().get() != a_dog) {
			Detach();
		}
		// Re-added every tick: his graph is rebuilt when his 3D reloads, and adding a sink twice is a no-op.
		a_dog->AddAnimationGraphEventSink(&g_sink);
		g_registered = RE::ActorHandle(a_dog);
	}
}
