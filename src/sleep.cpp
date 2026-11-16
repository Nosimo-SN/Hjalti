// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#include "sleep.h"

#include "actions.h"

#include <atomic>
#include <chrono>

namespace Canine::Sleep
{
	namespace
	{
		using namespace std::literals;
		using Clock = std::chrono::steady_clock;

		constexpr int  kFrostfallColdAt = 2;   // Frostfall exposure level (the player's): 2+ is cold
		constexpr int  kSeverColdAt = 50;      // SeverActions follower cold (his own, 0-100): 50 Cold, 75 Freezing
		constexpr auto kSeverPoll = 10s;       // his SeverActions cold changes every half game hour at most

		// Location types where he sleeps relaxed, and ones that keep him on guard. The nearest location in
		// the chain (a house, then its town, then the hold) that names either decides; none means the wilds.
		constexpr std::array<std::string_view, 13> kSafe{ "LocTypeCity", "LocTypeTown", "LocTypeSettlement",
			"LocTypeInn", "LocTypePlayerHouse", "LocTypeHouse", "LocTypeDwelling", "LocTypeHabitation",
			"LocTypeHabitationHasInn", "LocTypeStore", "LocTypeTemple", "LocTypeCastle", "LocTypeFarm" };
		constexpr std::array<std::string_view, 20> kDanger{ "LocTypeDungeon", "LocTypeAnimalDen", "LocTypeBanditCamp",
			"LocTypeDraugrCrypt", "LocTypeFalmerHive", "LocTypeDwarvenAutomatons", "LocTypeVampireLair",
			"LocTypeWarlockLair", "LocTypeHagravenNest", "LocTypeForswornCamp", "LocTypeGiantCamp", "LocTypeDragonLair",
			"LocTypeDragonPriestLair", "LocTypeMilitaryCamp", "LocTypeMilitaryFort", "LocTypeSprigganGrove",
			"LocTypeWerewolfLair", "LocTypeJail", "LocTypeCemetery", "LocTypeShip" };

		std::optional<Style> g_forced;

		// SeverActions: SeverActionsNative.Survival_GetColdLevel(him), asked every kSeverPoll, answer kept here.
		std::atomic_int  g_severCold{ -1 };  // -1: not known (SeverActions absent, or no answer yet)
		std::atomic_bool g_severAsked{ false };
		Clock::time_point g_nextSeverPoll{};

		class ColdLevelResult final : public RE::BSScript::IStackCallbackFunctor
		{
		public:
			void operator()(RE::BSScript::Variable a_result) override
			{
				g_severCold = a_result.IsInt() ? a_result.GetSInt() : -1;
				g_severAsked = false;
			}
			void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}
		};

		bool SeverActionsLoaded()
		{
			const auto data = RE::TESDataHandler::GetSingleton();
			return data && data->LookupModByName("SeverActions.esp");
		}

		bool Snowing()
		{
			const auto sky = RE::Sky::GetSingleton();
			const auto weather = sky ? sky->currentWeather : nullptr;
			return weather && weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kSnow);
		}

		std::optional<std::string_view> FirstMatch(const RE::BGSLocation* a_location, std::span<const std::string_view> a_keywords)
		{
			for (const auto keyword : a_keywords) {
				if (a_location->HasKeywordString(keyword)) {
					return keyword;
				}
			}
			return std::nullopt;
		}
	}

	std::string_view Name(Style a_style)
	{
		switch (a_style) {
		case Style::kRelax:
			return "relax"sv;
		case Style::kGuarded:
			return "guarded"sv;
		case Style::kCold:
			return "cold"sv;
		}
		return "?"sv;
	}

	std::optional<Style> Parse(std::string_view a_name)
	{
		if (a_name == "relax" || a_name == "relaxed" || a_name == "safe") {
			return Style::kRelax;
		}
		if (a_name == "guarded" || a_name == "guard") {
			return Style::kGuarded;
		}
		if (a_name == "cold" || a_name == "curled") {
			return Style::kCold;
		}
		return std::nullopt;
	}

	void Force(std::optional<Style> a_style)
	{
		g_forced = a_style;
	}

	Style Choose(RE::Actor* a_dog, std::string& a_why)
	{
		if (g_forced) {
			const auto style = *g_forced;
			g_forced.reset();
			a_why = "forced by debug.set";
			return style;
		}

		// Cold first: whichever cold mod is running says so.
		const int frost = Actions::FrostfallExposure();
		if (frost >= kFrostfallColdAt) {
			a_why = std::format("Frostfall exposure {}", frost);
			return Style::kCold;
		}
		const int sever = g_severCold.load();
		if (sever >= kSeverColdAt) {
			a_why = std::format("SeverActions cold {}", sever);
			return Style::kCold;
		}
		const auto cell = a_dog ? a_dog->GetParentCell() : nullptr;
		const bool outdoors = cell && !cell->IsInteriorCell();
		if (frost < 0 && sever < 0 && outdoors && Snowing()) {
			a_why = "snowing outdoors (no Frostfall or SeverActions cold)";
			return Style::kCold;
		}

		// Then the place.
		int depth = 0;
		for (auto location = a_dog ? a_dog->GetCurrentLocation() : nullptr; location && depth < 8; location = location->parentLoc, ++depth) {
			const char* name = location->GetFullName();
			if (const auto danger = FirstMatch(location, kDanger)) {
				a_why = std::format("{} ({})", name && *name ? name : "a location", *danger);
				return Style::kGuarded;
			}
			if (const auto safe = FirstMatch(location, kSafe)) {
				a_why = std::format("{} ({})", name && *name ? name : "a location", *safe);
				return Style::kRelax;
			}
		}
		a_why = outdoors ? "the wilds (no town or home here)" : "an unmarked interior";
		return Style::kGuarded;
	}

	void Update(RE::Actor* a_dog)
	{
		const auto now = Clock::now();
		if (!a_dog || now < g_nextSeverPoll) {  // no wait on an unanswered call: a lost answer mustn't stop the polling
			return;
		}
		g_nextSeverPoll = now + kSeverPoll;
		if (!SeverActionsLoaded()) {
			g_severCold = -1;
			return;
		}
		const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		if (!vm) {
			return;
		}
		RE::Actor* dog = a_dog;
		std::unique_ptr<RE::BSScript::IFunctionArguments> args{ RE::MakeFunctionArguments(std::move(dog)) };
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback{ new ColdLevelResult() };
		g_severAsked = true;
		if (!vm->DispatchStaticCall("SeverActionsNative", "Survival_GetColdLevel", args.get(), callback)) {
			g_severAsked = false;
			g_severCold = -1;
		}
	}

	void ResetSession()
	{
		g_forced.reset();
		g_severCold = -1;
		g_severAsked = false;
		g_nextSeverPoll = {};
	}
}
