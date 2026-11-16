// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#include "inputs.h"
#include "soundcues.h"

#include "actions.h"
#include "bridge.h"
#include "drives.h"
#include "idles.h"
#include "notify.h"
#include "training.h"

namespace Canine::Inputs
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr RE::FormID kVendorItemFoodRaw = 0x000A0E56;  // Skyrim.esm
		constexpr auto       kMealDelay = 1500ms;              // let the hand-over finish first
		constexpr auto       kEatingTime = 10s;                // canine_state reads "eating" this long
		constexpr auto       kHandedOverGap = 20s;             // talk-to-feed skips food that just arrived another way

		struct Meal
		{
			RE::FormID        item = 0;
			std::int32_t      count = 0;
			Clock::time_point arrived;
		};

		std::atomic<RE::FormID> g_target{ 0 };
		constexpr auto       kPromiseTime = 15s;               // a spoken offer keeps the gift menu shut this long

		std::atomic<Clock::rep> g_lastMeal{ 0 };  // when food last arrived in his inventory; 0 = never
		std::atomic<Clock::rep> g_promised{ 0 };  // when the player last offered food in words; 0 = never

		bool Within(const std::atomic<Clock::rep>& a_when, Clock::duration a_span)
		{
			const auto when = a_when.load();
			return when != 0 && Clock::now() - Clock::time_point(Clock::duration(when)) < a_span;
		}

		std::mutex        g_lock;  // guards the two queues below (sinks may fire off the game thread)
		std::vector<Meal> g_meals;
		std::optional<std::pair<RE::FormID, Clock::time_point>> g_ownMeal;  // a meal AddMeal already counted
		int               g_pets = 0;

		// Game thread only.
		std::optional<Clock::time_point> g_lastPet;
		std::optional<Clock::time_point> g_eatingUntil;

		bool MenusAllowEating()
		{
			const auto ui = RE::UI::GetSingleton();
			if (!ui || ui->GameIsPaused()) {
				return false;
			}
			for (const auto name : { RE::DialogueMenu::MENU_NAME, RE::GiftMenu::MENU_NAME, RE::ContainerMenu::MENU_NAME, RE::BarterMenu::MENU_NAME }) {
				if (ui->IsMenuOpen(name)) {
					return false;
				}
			}
			return true;
		}

		class Sink :
			public RE::BSTEventSink<RE::TESContainerChangedEvent>,
			public RE::BSTEventSink<RE::TESActivateEvent>
		{
		public:
			static Sink* GetSingleton()
			{
				static Sink singleton;
				return &singleton;
			}

			// Food arriving in his inventory, from anywhere: the player, a gift menu, another mod's action.
			RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
			{
				const auto target = g_target.load();
				if (!a_event || !target || a_event->newContainer != target || a_event->oldContainer == target || a_event->itemCount <= 0) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto item = RE::TESForm::LookupByID<RE::AlchemyItem>(a_event->baseObj);
				if (item && item->IsFood()) {
					std::scoped_lock lock(g_lock);
					if (g_ownMeal && g_ownMeal->first == a_event->baseObj && Clock::now() - g_ownMeal->second < 3s) {
						g_ownMeal.reset();  // the item AddMeal already counted, arriving in his inventory
						return RE::BSEventNotifyControl::kContinue;
					}
					g_meals.push_back({ a_event->baseObj, a_event->itemCount, Clock::now() });
					g_lastMeal = Clock::now().time_since_epoch().count();
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* a_event, RE::BSTEventSource<RE::TESActivateEvent>*) override
			{
				const auto target = g_target.load();
				if (!a_event || !target || !a_event->objectActivated || !a_event->actionRef ||
					a_event->objectActivated->GetFormID() != target || !a_event->actionRef->IsPlayerRef()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				std::scoped_lock lock(g_lock);
				++g_pets;
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		bool Contains(std::string_view a_text, std::string_view a_word)
		{
			return std::ranges::search(a_text, a_word, [](char a, char b) {
				return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
			}).begin() != a_text.end();
		}

		// Raw meat, whatever mod made it: the vanilla keyword, any keyword with "raw" in its editor ID
		// (Realistic Needs and Diseases tags dog-safe raw meat RND_STRawMeatDog), or a "Raw Food" effect.
		bool IsRaw(RE::AlchemyItem* a_item)
		{
			if (const auto raw = RE::TESForm::LookupByID<RE::BGSKeyword>(kVendorItemFoodRaw); raw && a_item->HasKeyword(raw)) {
				return true;
			}
			for (std::uint32_t i = 0; i < a_item->GetNumKeywords(); ++i) {
				const auto keyword = a_item->GetKeywordAt(i);
				if (keyword && *keyword && Contains((*keyword)->GetFormEditorID(), "raw")) {
					return true;
				}
			}
			for (const auto effect : a_item->effects) {
				if (effect && effect->baseEffect && Contains(effect->baseEffect->GetName(), "raw")) {
					return true;
				}
			}
			return false;
		}

		// How big a meal one item is, from its effect names: vanilla Survival's "Restore Hunger Very Small /
		// Small / Medium / Large", RND's "Snack / Fresh Fruit / Sweet Candy / Medium Meal / Filling Meal".
		// Food with no such effect counts as an ordinary item.
		double MealSize(RE::AlchemyItem* a_item)
		{
			for (const auto effect : a_item->effects) {
				if (!effect || !effect->baseEffect) {
					continue;
				}
				const std::string_view name = effect->baseEffect->GetName();
				if (Contains(name, "very small") || Contains(name, "snack") || Contains(name, "candy") || Contains(name, "fruit")) {
					return 0.5;
				}
				if (Contains(name, "large") || Contains(name, "filling")) {
					return 1.5;
				}
				if (Contains(name, "medium") || Contains(name, "meal")) {
					return 1.0;
				}
				if (Contains(name, "small")) {
					return 0.75;
				}
			}
			return 1.0;
		}

		void Eat(RE::Actor* a_dog, std::string_view a_dogName, const Settings& a_settings, std::vector<Meal> a_meals)
		{
			double relief = 0.0;
			std::vector<std::string> eaten;

			for (const auto& meal : a_meals) {
				const auto item = RE::TESForm::LookupByID<RE::AlchemyItem>(meal.item);
				if (!item) {
					continue;
				}
				const auto counts = a_dog->GetInventoryCounts([&](RE::TESBoundObject& a_obj) { return a_obj.GetFormID() == meal.item; });
				std::int32_t held = 0;
				for (const auto& [obj, count] : counts) {
					held += count;
				}
				const auto count = (std::min)(meal.count, held);
				if (count <= 0) {
					continue;  // already gone: eaten, taken back or dropped
				}
				a_dog->RemoveItem(item, count, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
				const double perItem = (a_settings.feedPerItem + (IsRaw(item) ? a_settings.feedRawBonus : 0.0)) * MealSize(item);
				relief += perItem * count;

				const std::string name = item->GetName();
				eaten.push_back(count > 1 ? std::format("{} {}", count, name) : name);
			}
			if (eaten.empty()) {
				return;
			}

			relief = (std::min)(relief, a_settings.feedMaxMeal);
			Training::Did("eat");
			Drives::Adjust(Drive::kHunger, -relief, a_settings);
			g_eatingUntil = Clock::now() + kEatingTime;
			Idles::Play(a_dog, Idles::kDogLay, "idleLayStart");  // lie down to eat
			SoundCues::Chew(a_dog, std::chrono::milliseconds(6000));  // his chewing while he eats it

			std::string food;
			for (std::size_t i = 0; i < eaten.size(); ++i) {
				food += (i == 0 ? "" : i + 1 == eaten.size() ? " and " : ", ") + eaten[i];
			}
			logs::info("Ate {} (hunger -{:.0f} -> {:.1f})", food, relief, Drives::Value(Drive::kHunger));
			if (a_settings.events) {
				Bridge::PostEvent(std::format("{} lies down and wolfs down the {}.", a_dogName, food), a_dog->GetFormID(),
					a_settings.narrate ? RE::PlayerCharacter::GetSingleton()->GetFormID() : 0, a_settings.narrate);
			}
		}

		bool IsDrink(std::string_view a_name)
		{
			static const std::array<std::string_view, 11> kDrinks{ "mead", "ale", "wine", "water", "milk", "tea", "juice", "brandy", "rum",
				"beer", "skooma" };
			return std::ranges::any_of(kDrinks, [&](std::string_view a_word) { return Contains(a_name, a_word); });
		}

		// The food in the player's pack that best fits the line: the one named in it (longest name wins, so "Horker Meat"
		// beats "Meat"), else raw meat, else any food that isn't a drink.
		RE::AlchemyItem* PickFood(RE::PlayerCharacter* a_player, std::string_view a_line)
		{
			RE::AlchemyItem* named = nullptr;
			RE::AlchemyItem* raw = nullptr;
			RE::AlchemyItem* any = nullptr;
			std::size_t      namedLength = 0;
			for (const auto& [obj, data] : a_player->GetInventory()) {
				const auto item = obj ? obj->As<RE::AlchemyItem>() : nullptr;
				if (!item || data.first <= 0 || !item->IsFood() || item->IsPoison() || (data.second && data.second->IsQuestObject())) {
					continue;
				}
				const std::string_view name = item->GetName();
				if (name.empty()) {
					continue;
				}
				if (name.size() > namedLength && Contains(a_line, name)) {
					named = item;
					namedLength = name.size();
				}
				if (IsDrink(name)) {
					continue;
				}
				if (!raw && IsRaw(item)) {
					raw = item;
				}
				if (!any) {
					any = item;
				}
			}
			return named ? named : raw ? raw : any;
		}

		bool WeaponDrawn(RE::PlayerCharacter* a_player)
		{
			const auto state = a_player->AsActorState();
			return state && state->IsWeaponDrawn();
		}

		// The drives and the event; the callers decide whether a pet happened. Returns false on cooldown.
		bool ApplyPet(RE::Actor* a_dog, std::string_view a_dogName, std::string_view a_player, std::string_view a_how, const Settings& a_settings)
		{
			const auto now = Clock::now();
			if (g_lastPet && now - *g_lastPet < std::chrono::duration<double>(a_settings.petCooldown)) {
				return false;
			}
			g_lastPet = now;

			Drives::Adjust(Drive::kAffection, -a_settings.petAffection, a_settings);
			Drives::Adjust(Drive::kSeparation, -a_settings.petSeparation, a_settings);
			logs::info("Petted (affection -> {:.1f}, separation -> {:.1f})", Drives::Value(Drive::kAffection), Drives::Value(Drive::kSeparation));
			if (a_settings.events) {
				// As narration he is the one who reacts, so he goes first.
				const auto player = RE::PlayerCharacter::GetSingleton()->GetFormID();
				const auto dog = a_dog->GetFormID();
				Bridge::PostEvent(std::format("{} {} {} behind the ears.", a_player, a_how, a_dogName),
					a_settings.narrate ? dog : player, a_settings.narrate ? player : dog, a_settings.narrate);
			}
			return true;
		}

		// Activation opens whatever dialogue menu other mods give him, so it only counts when switched on.
		void PetByActivation(RE::Actor* a_dog, std::string_view a_dogName, std::string_view a_player, const Settings& a_settings)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player || !a_settings.petOnActivate || WeaponDrawn(player)) {
				return;
			}
			if (a_settings.petRequireSneak && !player->IsSneaking()) {
				return;
			}
			if (ApplyPet(a_dog, a_dogName, a_player, "scratches", a_settings)) {
				Actions::Wake("petted");
				if (!Actions::Wiggle(a_dog, "petted")) {  // the happy wiggle; lying, sitting or just done: a happy bark
					Actions::Bark(a_dog, false);
				}
			}
		}
	}

	void Register()
	{
		if (const auto events = RE::ScriptEventSourceHolder::GetSingleton()) {
			events->AddEventSink<RE::TESContainerChangedEvent>(Sink::GetSingleton());
			events->AddEventSink<RE::TESActivateEvent>(Sink::GetSingleton());
		}
	}

	void SetTarget(RE::FormID a_ref)
	{
		g_target = a_ref;
	}

	void ResetSession()
	{
		{
			std::scoped_lock lock(g_lock);
			g_meals.clear();
			g_pets = 0;
		}
		g_lastPet.reset();
		g_eatingUntil.reset();
	}

	void Process(RE::Actor* a_dog, std::string_view a_dogName, std::string_view a_player, const Settings& a_settings)
	{
		int               pets = 0;
		std::vector<Meal> meals;
		{
			std::scoped_lock lock(g_lock);
			pets = std::exchange(g_pets, 0);
			// Meals wait until the hand-over is done and no menu is open; the rest stay queued.
			if (a_dog && a_dog->Is3DLoaded() && !a_dog->IsDead() && !a_dog->IsInCombat() && MenusAllowEating()) {
				const auto ready = Clock::now() - kMealDelay;
				std::erase_if(g_meals, [&](const Meal& a_meal) {
					if (a_meal.arrived <= ready) {
						meals.push_back(a_meal);
						return true;
					}
					return false;
				});
			}
		}

		if (!a_dog || a_dog->IsDead()) {
			return;
		}
		if (pets > 0) {
			PetByActivation(a_dog, a_dogName, a_player, a_settings);
		}
		if (!meals.empty()) {
			Eat(a_dog, a_dogName, a_settings, std::move(meals));
		}
	}

	void PetByHotkey(RE::Actor* a_dog, std::string_view a_dogName, std::string_view a_player, const Settings& a_settings)
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto ui = RE::UI::GetSingleton();
		if (!player || !a_dog || !a_dog->Is3DLoaded() || a_dog->IsDead() || (ui && (ui->GameIsPaused() || ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME)))) {
			return;
		}
		if (player->IsInCombat() || a_dog->IsInCombat()) {
			logs::info("Pet hotkey: not during combat");
			return;
		}
		if (WeaponDrawn(player)) {
			logs::info("Pet hotkey: sheathe your weapon first");
			return;
		}
		const auto dogPos = a_dog->GetPosition();
		const auto playerPos = player->GetPosition();
		const auto dogCell = a_dog->GetParentCell();
		const auto playerCell = player->GetParentCell();
		if (!dogCell || !playerCell ||
			((dogCell->IsInteriorCell() || playerCell->IsInteriorCell()) ? dogCell != playerCell : a_dog->GetWorldspace() != player->GetWorldspace())) {
			return;
		}
		if (dogPos.GetDistance(playerPos) > a_settings.petRange) {
			logs::info("Pet hotkey: {} is too far away ({:.0f} units)", a_dogName, dogPos.GetDistance(playerPos));
			return;
		}
		if (!ApplyPet(a_dog, a_dogName, a_player, "kneels and scratches", a_settings)) {
			return;
		}

		// Face him, kneel (vanilla IdleSearchBody), stand back up after a moment; he sits for it and barks.
		Actions::Wake("petted");
		player->SetHeading(std::atan2(dogPos.x - playerPos.x, dogPos.y - playerPos.y));
		Idles::Play(player, Idles::kSearchBody, "IdleKneeling");
		Idles::Play(a_dog, Idles::kDogSit, "idleSitStart");
		std::thread([] {
			std::this_thread::sleep_for(2500ms);
			SKSE::GetTaskInterface()->AddTask([] {
				if (const auto player = RE::PlayerCharacter::GetSingleton()) {
					Idles::Play(player, Idles::kStopLoose, "IdleStop");
				}
			});
		}).detach();
		std::thread([dog = a_dog->GetFormID()] {
			std::this_thread::sleep_for(2000ms);  // time to sit down first
			SKSE::GetTaskInterface()->AddTask([dog] {
				if (const auto actor = RE::TESForm::LookupByID<RE::Actor>(dog)) {
					Actions::Bark(actor, true);  // a happy bark while sitting
				}
			});
		}).detach();
	}

	bool FeedFromPlayer(RE::Actor* a_dog, std::string_view a_line)
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !a_dog || !a_dog->Is3DLoaded() || a_dog->IsDead() || a_dog->IsInCombat()) {
			return false;
		}
		// The food may already be on its way: the trade or gift menu is open, or something just arrived.
		const auto ui = RE::UI::GetSingleton();
		if (ui && (ui->IsMenuOpen(RE::GiftMenu::MENU_NAME) || ui->IsMenuOpen(RE::BarterMenu::MENU_NAME) || ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME))) {
			logs::info("Talk to feed: a menu is open, the food goes through it");
			return false;
		}
		if (Within(g_lastMeal, kHandedOverGap)) {
			logs::info("Talk to feed: food just reached him another way");
			return false;
		}
		const auto food = PickFood(player, a_line);
		if (!food) {
			logs::info("Talk to feed: nothing to eat in the player's pack");
			Notify::About(a_dog->GetFormID(), "You have no food for {}.");
			return false;
		}
		// Into his inventory: the container sink then treats it like any other meal.
		player->RemoveItem(food, 1, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, a_dog);
		logs::info("Talk to feed: gave {}", food->GetName());
		Notify::About(a_dog->GetFormID(), std::format("You give {{}} the {}.", food->GetName()));
		return true;
	}

	void AddMeal(RE::FormID a_item, std::int32_t a_count)
	{
		std::scoped_lock lock(g_lock);
		g_meals.push_back({ a_item, a_count, Clock::now() });
		g_lastMeal = Clock::now().time_since_epoch().count();
		g_ownMeal = std::pair{ a_item, Clock::now() };
	}

	void NoteEating()
	{
		g_eatingUntil = Clock::now() + kEatingTime;
	}

	void PromiseFood()
	{
		g_promised = Clock::now().time_since_epoch().count();
	}

	bool FoodComing()
	{
		return Within(g_promised, kPromiseTime) || Within(g_lastMeal, kHandedOverGap);
	}

	std::string_view State()
	{
		return g_eatingUntil && Clock::now() < *g_eatingUntil ? "eating"sv : "awake"sv;
	}
}
