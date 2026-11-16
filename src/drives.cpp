// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#include "drives.h"

namespace Canine::Drives
{
	namespace
	{
		constexpr std::uint32_t kRecordDrives = 'DRIV';
		constexpr std::uint32_t kRecordVersion = 1;
		constexpr double        kHysteresis = 5.0;
		constexpr std::array<double, kDriveCount> kStartValues{ 20.0, 20.0, 20.0, 20.0, 0.0 };

		// Game-thread state.
		std::array<double, kDriveCount> g_values = kStartValues;
		std::array<int, kDriveCount>    g_tiers{ -1, -1, -1, -1, -1 };  // -1 = content
		double                          g_lastHours = -1.0;

		// What the decorators read (SkyrimNet worker threads).
		struct Snapshot
		{
			RE::FormID                   ref = 0;
			RE::FormID                   base = 0;
			std::array<int, kDriveCount> values{};
			std::string                  needs;
			std::string                  state = "awake";
			bool                         published = false;  // the C API answers only once a snapshot is in
			CDAPI_SnapshotV1             api{};
		};
		std::mutex g_snapshotLock;
		Snapshot   g_snapshot;

		std::string_view TierName(int a_tier)
		{
			constexpr std::array<std::string_view, kTierCount> kNames{ "noticeable", "strong", "urgent" };
			return a_tier < 0 ? "content"sv : kNames[static_cast<std::size_t>(a_tier)];
		}

		// Rises at once; falls only after dropping kHysteresis below the tier's threshold, so it doesn't flicker.
		int NextTier(double a_value, int a_current, const Settings& a_settings)
		{
			int tier = -1;
			for (std::size_t t = 0; t < kTierCount; ++t) {
				if (a_value >= a_settings.tierThresholds[t]) {
					tier = static_cast<int>(t);
				}
			}
			if (tier >= a_current) {
				return tier;
			}
			for (int t = a_current; t > tier; --t) {
				if (a_value >= a_settings.tierThresholds[static_cast<std::size_t>(t)] - kHysteresis) {
					return t;
				}
			}
			return tier;
		}

		void ReplaceAll(std::string& a_text, std::string_view a_token, std::string_view a_with)
		{
			for (auto pos = a_text.find(a_token); pos != std::string::npos; pos = a_text.find(a_token, pos + a_with.size())) {
				a_text.replace(pos, a_token.size(), a_with);
			}
		}

		bool Matches(RE::Actor* a_actor, const Snapshot& a_snap)
		{
			if (!a_actor || !a_snap.ref) {
				return false;
			}
			if (a_actor->GetFormID() == a_snap.ref) {
				return true;
			}
			const auto base = a_actor->GetActorBase();
			return base && a_snap.base && base->GetFormID() == a_snap.base;
		}
	}

	void Reset()
	{
		g_values = kStartValues;
		g_tiers.fill(-1);
		g_lastHours = -1.0;
		// The save's values aren't in yet: nobody reads the old game's until the next publish.
		std::scoped_lock lock(g_snapshotLock);
		g_snapshot = Snapshot{};
	}

	void UpdateTiers(const Settings& a_settings)
	{
		for (std::size_t d = 0; d < kDriveCount; ++d) {
			const int next = NextTier(g_values[d], g_tiers[d], a_settings);
			if (next != g_tiers[d]) {
				logs::info("{}: {} -> {} ({:.1f})", kDriveKeys[d], TierName(g_tiers[d]), TierName(next), g_values[d]);
				g_tiers[d] = next;
			}
		}
	}

	void Step(const Context& a_ctx, const Settings& a_settings)
	{
		if (a_ctx.hours <= 0.0) {
			return;
		}
		const double awake = (std::max)(0.0, a_ctx.hours - a_ctx.sleepHours);
		auto         add = [](double& a_value, double a_delta) { a_value = std::clamp(a_value + a_delta, 0.0, 100.0); };

		add(g_values[static_cast<std::size_t>(Drive::kHunger)],
			a_settings.hungerRate * a_ctx.hours * (a_ctx.combat ? a_settings.combatHungerMult : 1.0));
		// Asleep he isn't lonely or restless; those only move while awake.
		add(g_values[static_cast<std::size_t>(Drive::kAffection)], a_settings.affectionRate * awake);
		double restlessRate = a_ctx.combat ? a_settings.combatRestlessRate :
		                      a_ctx.moving ? a_settings.restlessTravelRate :
		                                     a_settings.restlessIdleRate;
		// A worn-out dog doesn't build up energy: while tiredness is strong, idle restlessness barely rises.
		if (restlessRate > 0.0 && g_tiers[static_cast<std::size_t>(Drive::kTired)] >= static_cast<int>(Tier::kStrong)) {
			restlessRate *= a_settings.tiredRestlessMult;
		}
		add(g_values[static_cast<std::size_t>(Drive::kRestless)], restlessRate * awake);

		double tiredRate = a_settings.tiredRate;
		if (a_ctx.night) {
			tiredRate *= a_settings.tiredNightMult;
		}
		if (a_ctx.combat) {
			tiredRate *= a_settings.combatTiredMult;
		} else if (a_ctx.moving) {
			tiredRate *= a_settings.tiredTravelMult;
		}
		add(g_values[static_cast<std::size_t>(Drive::kTired)],
			tiredRate * awake + (a_ctx.napping ? a_settings.tiredNapRate : a_settings.tiredSleepRate) * a_ctx.sleepHours);

		add(g_values[static_cast<std::size_t>(Drive::kSeparation)],
			(a_ctx.isNear ? a_settings.separationNearRate : a_settings.separationApartRate) * a_ctx.hours);

		UpdateTiers(a_settings);
	}

	void Adjust(Drive a_drive, double a_delta, const Settings& a_settings)
	{
		auto& value = g_values[static_cast<std::size_t>(a_drive)];
		value = std::clamp(value + a_delta, 0.0, 100.0);
		UpdateTiers(a_settings);
	}

	double Value(Drive a_drive)
	{
		return g_values[static_cast<std::size_t>(a_drive)];
	}

	bool ApplyDebugSet(std::string_view a_command)
	{
		bool applied = false;
		// Accept "hunger = 80" and "hunger: 80" as well as "hunger=80": drop the spaces around the sign.
		std::string command;
		for (const char c : a_command) {
			const char ch = (c == ':') ? '=' : (c == ',' || c == ';') ? ' ' : c;
			if (ch == '=') {
				while (!command.empty() && std::isspace(static_cast<unsigned char>(command.back()))) {
					command.pop_back();
				}
			} else if (std::isspace(static_cast<unsigned char>(ch)) && !command.empty() && command.back() == '=') {
				continue;
			}
			command += ch;
		}
		std::istringstream tokens(command);
		for (std::string token; tokens >> token;) {
			const auto eq = token.find('=');
			if (eq == std::string::npos) {
				logs::warn("debug.set: '{}' is not key=value", token);
				continue;
			}
			auto key = token.substr(0, eq);
			std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			double value = 0.0;
			try {
				value = std::clamp(std::stod(token.substr(eq + 1)), 0.0, 100.0);
			} catch (...) {
				logs::warn("debug.set: '{}' has no number", token);
				continue;
			}
			bool known = false;
			for (std::size_t d = 0; d < kDriveCount; ++d) {
				if (key == "all" || key == kDriveKeys[d]) {
					g_values[d] = value;
					known = true;
				}
			}
			if (known) {
				applied = true;
			} else {
				logs::warn("debug.set: unknown drive '{}'", key);
			}
		}
		return applied;
	}

	void Publish(RE::FormID a_ref, RE::FormID a_base, std::string_view a_dog, std::string_view a_player, std::string_view a_state,
		std::uint32_t a_activity, const Settings& a_settings)
	{
		UpdateTiers(a_settings);  // a load leaves them unset until the next step; a publish shouldn't wait for one
		Snapshot snap;
		snap.ref = a_ref;
		snap.base = a_base;
		snap.state = a_state;
		for (std::size_t d = 0; d < kDriveCount; ++d) {
			snap.values[d] = static_cast<int>(std::lround(g_values[d]));
		}

		// The strongest needs above "content", strongest first.
		// While he's strongly tired, restlessness doesn't show: "exhausted" and "bursting with energy"
		// side by side leave the model nothing to act on.
		const bool worn = g_tiers[static_cast<std::size_t>(Drive::kTired)] >= static_cast<int>(Tier::kStrong);
		std::vector<std::size_t> active;
		for (std::size_t d = 0; d < kDriveCount; ++d) {
			if (g_tiers[d] >= 0 && !(worn && d == static_cast<std::size_t>(Drive::kRestless))) {
				active.push_back(d);
			}
		}
		std::ranges::sort(active, [](std::size_t a, std::size_t b) { return g_values[a] > g_values[b]; });
		if (active.size() > static_cast<std::size_t>(a_settings.promptDrives)) {
			active.resize(static_cast<std::size_t>(a_settings.promptDrives));
		}
		for (const auto d : active) {
			auto text = a_settings.text[d][static_cast<std::size_t>(g_tiers[d])];
			ReplaceAll(text, "{name}", a_dog);
			ReplaceAll(text, "{player}", a_player);
			if (!snap.needs.empty()) {
				snap.needs += ' ';
			}
			snap.needs += text;
		}

		// The C API's copy: the same values, unrounded, with the tiers as Canine Drives keeps them.
		const auto tier = [](Drive a_drive) {
			return static_cast<std::uint32_t>(g_tiers[static_cast<std::size_t>(a_drive)] + 1);  // -1 content -> CDAPI_TIER_CONTENT
		};
		auto& api = snap.api;
		api.structSize = sizeof(CDAPI_SnapshotV1);
		api.apiVersion = CDAPI_VERSION;
		api.dogRefID = a_ref;
		api.enabled = a_settings.enabled ? 1u : 0u;
		api.hunger = static_cast<float>(g_values[static_cast<std::size_t>(Drive::kHunger)]);
		api.tiredness = static_cast<float>(g_values[static_cast<std::size_t>(Drive::kTired)]);
		api.thresholdNoticeable = static_cast<float>(a_settings.tierThresholds[static_cast<std::size_t>(Tier::kNoticeable)]);
		api.thresholdStrong = static_cast<float>(a_settings.tierThresholds[static_cast<std::size_t>(Tier::kStrong)]);
		api.thresholdUrgent = static_cast<float>(a_settings.tierThresholds[static_cast<std::size_t>(Tier::kUrgent)]);
		api.hysteresis = static_cast<float>(kHysteresis);
		api.hungerTier = tier(Drive::kHunger);
		api.tiredTier = tier(Drive::kTired);
		api.flags = (api.hungerTier >= CDAPI_TIER_NOTICEABLE ? CDAPI_FLAG_HUNGER_WARNING : 0u) |
		            (api.tiredTier >= CDAPI_TIER_NOTICEABLE ? CDAPI_FLAG_TIRED_WARNING : 0u);
		api.activity = a_activity;
		snap.published = a_ref != 0;

		std::scoped_lock lock(g_snapshotLock);
		g_snapshot = std::move(snap);
	}

	bool ApiSnapshot(std::uint32_t a_ref, CDAPI_SnapshotV1& a_out)
	{
		std::scoped_lock lock(g_snapshotLock);
		if (!g_snapshot.published || a_ref == 0 || a_ref != g_snapshot.ref) {
			return false;
		}
		a_out = g_snapshot.api;
		return true;
	}

	std::string Describe()
	{
		std::string out;
		for (std::size_t d = 0; d < kDriveCount; ++d) {
			out += std::format("{}{} {:.1f} ({})", d ? ", " : "", kDriveKeys[d], g_values[d], TierName(g_tiers[d]));
		}
		return out;
	}

	double LastHours() { return g_lastHours; }
	void   SetLastHours(double a_hours) { g_lastHours = a_hours; }

	void Save(SKSE::SerializationInterface* a_intfc)
	{
		if (!a_intfc->OpenRecord(kRecordDrives, kRecordVersion)) {
			logs::error("Could not open the co-save record");
			return;
		}
		for (const auto value : g_values) {
			a_intfc->WriteRecordData(value);
		}
		a_intfc->WriteRecordData(g_lastHours);
	}

	bool LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type, std::uint32_t a_version)
	{
		if (a_type != kRecordDrives) {
			return false;
		}
		if (a_version != kRecordVersion) {
			logs::warn("Co-save record version {} not understood; drives start fresh", a_version);
			return true;
		}
		std::array<double, kDriveCount> values{};
		double                          lastHours = -1.0;
		bool                            ok = true;
		for (auto& value : values) {
			ok = ok && a_intfc->ReadRecordData(value) == sizeof(value);
		}
		ok = ok && a_intfc->ReadRecordData(lastHours) == sizeof(lastHours);
		if (!ok) {
			logs::warn("Co-save record is short; drives start fresh");
			return true;
		}
		for (std::size_t d = 0; d < kDriveCount; ++d) {
			g_values[d] = std::isfinite(values[d]) ? std::clamp(values[d], 0.0, 100.0) : kStartValues[d];
		}
		g_lastHours = std::isfinite(lastHours) ? lastHours : -1.0;
		g_tiers.fill(-1);  // the next step recomputes them
		logs::info("Loaded: {}", Describe());
		return true;
	}

	void Revert(SKSE::SerializationInterface*)
	{
		Reset();
	}

	std::string DecoratorValue(RE::Actor* a_actor, Drive a_drive)
	{
		std::scoped_lock lock(g_snapshotLock);
		if (!Matches(a_actor, g_snapshot)) {
			return "";
		}
		return std::to_string(g_snapshot.values[static_cast<std::size_t>(a_drive)]);
	}

	std::optional<int> Peek(RE::Actor* a_actor, Drive a_drive)
	{
		std::scoped_lock lock(g_snapshotLock);
		if (!Matches(a_actor, g_snapshot)) {
			return std::nullopt;
		}
		return g_snapshot.values[static_cast<std::size_t>(a_drive)];
	}

	std::string DecoratorNeeds(RE::Actor* a_actor)
	{
		std::scoped_lock lock(g_snapshotLock);
		return Matches(a_actor, g_snapshot) ? g_snapshot.needs : "";
	}

	std::string DecoratorState(RE::Actor* a_actor)
	{
		std::scoped_lock lock(g_snapshotLock);
		return Matches(a_actor, g_snapshot) ? g_snapshot.state : "";
	}
}
