// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#include "training.h"

namespace Canine::Training
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr std::uint32_t kRecordVersion = 1;
		constexpr auto          kCreditWindow = 60s;  // praise or scolding this soon after an action counts toward it
		constexpr double        kClear = 1.5;          // total weight before a lean shows at all (one reaction is no lesson)
		constexpr double        kStrong = 3.5;         // net weight for "nearly always"
		constexpr double        kLean = 2.0;           // one side this many times the other: a clear lean, else mixed

		struct Lesson
		{
			std::string_view key;
			std::string_view when;  // "when he ..." (no player name; that goes in the sentence)
		};
		constexpr std::array<Lesson, 11> kLessons{ {
			{ "play", "he races about in his mad, happy runs" },
			{ "sniff", "he goes nosing at other people" },
			{ "beg", "he begs for food" },
			{ "nap", "he lies down for a nap" },
			{ "seek_affection", "he comes pressing close for a scratch" },
			{ "eat", "he wolfs down his food" },
			{ "warm", "he presses close to warm someone who is cold" },
			{ "sing", "he howls along with a bard's song" },
			{ "scavenge", "he snaps up food he finds lying about" },
			{ "steal", "he helps himself to other people's food" },
			{ "hunt", "he runs down game and kills it" },
		} };

		struct Tally
		{
			double pleased = 0.0;
			double cross = 0.0;
		};

		// Game thread.
		std::array<Tally, kLessons.size()>  g_tallies{};
		double                              g_lastHours = -1.0;
		std::optional<std::size_t>          g_lastAction;
		Clock::time_point                   g_lastActionAt;

		// Decorator side.
		std::mutex  g_lock;
		std::string g_text;
		RE::FormID  g_ref = 0;
		RE::FormID  g_base = 0;

		std::optional<std::size_t> Index(std::string_view a_key)
		{
			for (std::size_t i = 0; i < kLessons.size(); ++i) {
				if (kLessons[i].key == a_key) {
					return i;
				}
			}
			return std::nullopt;
		}

		std::string Words(std::string_view a_player)
		{
			std::string out;
			for (std::size_t i = 0; i < kLessons.size(); ++i) {
				const auto& [pleased, cross] = g_tallies[i];
				if (pleased + cross < kClear) {
					continue;
				}
				const auto when = kLessons[i].when;
				std::string line;
				if (pleased >= kLean * cross) {
					line = std::format("He has learned that {} is {} pleased when {}.", a_player, pleased - cross >= kStrong ? "nearly always" : "usually", when);
				} else if (cross >= kLean * pleased) {
					line = std::format("He has learned that {} {} cross with him when {}.", a_player, cross - pleased >= kStrong ? "is nearly always" : "tends to be", when);
				} else {
					line = std::format("{} is sometimes pleased and sometimes cross when {}; he isn't sure what to make of it.", a_player, when);
				}
				out += (out.empty() ? "" : " ") + line;
			}
			return out;
		}

		bool Matches(RE::Actor* a_actor)
		{
			if (!a_actor || !g_ref) {
				return false;
			}
			if (a_actor->GetFormID() == g_ref) {
				return true;
			}
			const auto base = a_actor->GetActorBase();
			return base && g_base && base->GetFormID() == g_base;
		}
	}

	void Did(std::string_view a_action)
	{
		if (const auto index = Index(a_action)) {
			g_lastAction = index;
			g_lastActionAt = Clock::now();
		}
	}

	void Feedback(bool a_praise, const Settings& a_settings)
	{
		if (!a_settings.trainingEnabled || !g_lastAction || Clock::now() - g_lastActionAt > kCreditWindow) {
			return;  // nothing he just did to tie it to: the affection change is all it does
		}
		auto& tally = g_tallies[*g_lastAction];
		(a_praise ? tally.pleased : tally.cross) += 1.0;
		logs::info("Training: {} after {} (pleased {:.1f}, cross {:.1f})", a_praise ? "praise" : "scolding", kLessons[*g_lastAction].key,
			tally.pleased, tally.cross);
	}

	void Update(double a_hoursPassed, RE::FormID a_ref, RE::FormID a_base, std::string_view a_player, const Settings& a_settings)
	{
		if (g_lastHours >= 0.0 && a_hoursPassed > g_lastHours && a_settings.trainingHalfLife > 0.0) {
			const double fade = std::pow(0.5, (a_hoursPassed - g_lastHours) / a_settings.trainingHalfLife);
			for (auto& [pleased, cross] : g_tallies) {
				pleased *= fade;
				cross *= fade;
			}
		}
		g_lastHours = a_hoursPassed;

		std::scoped_lock lock(g_lock);
		g_ref = a_ref;
		g_base = a_base;
		g_text = a_settings.trainingEnabled ? Words(a_player) : "";
	}

	void ResetSession()
	{
		g_lastAction.reset();
	}

	std::string Text()
	{
		std::scoped_lock lock(g_lock);
		return g_text;
	}

	std::string DecoratorLessons(RE::Actor* a_actor)
	{
		std::scoped_lock lock(g_lock);
		return Matches(a_actor) ? g_text : "";
	}

	void Save(SKSE::SerializationInterface* a_intfc)
	{
		if (!a_intfc->OpenRecord(kRecord, kRecordVersion)) {
			logs::error("Could not open the training co-save record");
			return;
		}
		const auto count = static_cast<std::uint32_t>(kLessons.size());
		a_intfc->WriteRecordData(count);
		for (const auto& [pleased, cross] : g_tallies) {
			a_intfc->WriteRecordData(pleased);
			a_intfc->WriteRecordData(cross);
		}
		a_intfc->WriteRecordData(g_lastHours);
	}

	void LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version)
	{
		if (a_version != kRecordVersion) {
			logs::warn("Training co-save record version {} not understood; he starts with a clean slate", a_version);
			return;
		}
		std::uint32_t count = 0;
		if (a_intfc->ReadRecordData(count) != sizeof(count)) {
			return;
		}
		std::array<Tally, kLessons.size()> tallies{};
		for (std::uint32_t i = 0; i < count; ++i) {
			Tally tally;
			if (a_intfc->ReadRecordData(tally.pleased) != sizeof(double) || a_intfc->ReadRecordData(tally.cross) != sizeof(double)) {
				logs::warn("Training co-save record is short; he starts with a clean slate");
				return;
			}
			if (i < tallies.size() && std::isfinite(tally.pleased) && std::isfinite(tally.cross)) {
				tallies[i] = { (std::max)(0.0, tally.pleased), (std::max)(0.0, tally.cross) };
			}
		}
		double lastHours = -1.0;
		a_intfc->ReadRecordData(lastHours);
		g_tallies = tallies;
		g_lastHours = std::isfinite(lastHours) ? lastHours : -1.0;
		logs::info("Training loaded");
	}

	void Revert()
	{
		g_tallies = {};
		g_lastHours = -1.0;
		g_lastAction.reset();
		std::scoped_lock lock(g_lock);
		g_text.clear();
	}
}
