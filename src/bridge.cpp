// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#include "bridge.h"
#include "jsonfield.h"
#include "senses.h"

#include "actions.h"
#include "drives.h"
#include "training.h"

#include <nlohmann/json.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "SkyrimNet/PublicAPI.h"

namespace Bridge
{
	namespace
	{
		constexpr auto kConfigName = "CanineDrives";
	}

	int Init()
	{
		if (!FindFunctions() || !PublicGetVersion) {
			return 0;
		}
		return PublicGetVersion();
	}

	bool HasConfig()
	{
		return PublicGetPluginConfigValue != nullptr;
	}

	std::string ConfigValue(const std::string& a_path, const std::string& a_default)
	{
		if (!PublicGetPluginConfigValue) {
			return a_default;
		}
		return PublicGetPluginConfigValue(kConfigName, a_path.c_str(), a_default.c_str());
	}

	namespace
	{
		// Logs what SkyrimNetApi.RegisterPersistentEvent or DirectNarration returned (0 = stored, 1 = refused).
		class EventResult : public RE::BSScript::IStackCallbackFunctor
		{
		public:
			explicit EventResult(std::string a_line) :
				_line(std::move(a_line))
			{}

			void operator()(RE::BSScript::Variable a_result) override
			{
				if (!a_result.IsInt() || a_result.GetSInt() != 0) {
					logs::warn("SkyrimNet did not store the event '{}'", _line);
				}
			}

			void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

		private:
			std::string _line;
		};
	}

	// The C++ PublicRegisterEvent refused these silently in game (2026-11-01), so this goes through
	// SkyrimNet's Papyrus RegisterPersistentEvent, which other mods in the same setup use successfully. Narration goes
	// through DirectNarration, its sibling with reactions on.
	void PostEvent(std::string a_line, RE::FormID a_originator, RE::FormID a_target, bool a_narrate)
	{
		const auto function = a_narrate ? "DirectNarration"sv : "RegisterPersistentEvent"sv;
		const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		if (!vm) {
			return;
		}
		auto originator = a_originator ? RE::TESForm::LookupByID<RE::Actor>(a_originator) : nullptr;
		auto target = a_target ? RE::TESForm::LookupByID<RE::Actor>(a_target) : nullptr;
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result{ new EventResult(a_line) };
		std::unique_ptr<RE::BSScript::IFunctionArguments>          args{ RE::MakeFunctionArguments(std::string(a_line), std::move(originator), std::move(target)) };
		if (!vm->DispatchStaticCall("SkyrimNetApi", function, args.get(), result)) {
			logs::warn("SkyrimNetApi.{} could not be called", function);
		}
	}

	void WatchDialogue(LineHandler a_onLine)
	{
		if (!PublicRegisterEventCallback) {
			logs::warn("This SkyrimNet has no event callbacks: no leak repair, and the behavior loop can't hear conversations");
			return;
		}
		auto handler = std::make_shared<LineHandler>(std::move(a_onLine));
		for (const auto type : { "dialogue", "dialogue_npc", "dialogue_player_text", "dialogue_player_stt", "dialogue_player",
				 "dialogue_player_telepathy", "dialogue_npc_telepathy" }) {
			PublicRegisterEventCallback(type, [handler, type = std::string(type)](const char* a_json) {
				try {
				const auto event = nlohmann::json::parse(a_json ? a_json : "", nullptr, false);
				if (!event.is_object()) {
					return;
				}
				const auto formId = [&](const char* a_key) {
					const auto it = event.find(a_key);
					return it != event.end() && it->is_number_integer() ? it->get<RE::FormID>() : RE::FormID{ 0 };
				};
				const RE::FormID speaker = formId("originatingActorFormId");
				const RE::FormID target = formId("targetActorFormId");
				std::string      text;
				// "data" is itself JSON in a string; the line is under "dialogue" or "text".
				if (const auto it = event.find("data"); it != event.end()) {
					const auto data = it->is_string() ? nlohmann::json::parse(it->get<std::string>(), nullptr, false) : *it;
					text = JsonField::Text(data, "dialogue", JsonField::Text(data, "text"));
				}
				(*handler)(type, speaker, target, text);
				} catch (const std::exception& e) {
					logs::warn("Dialogue event could not be read ({})", e.what());
				}
			});
		}
	}

	void WatchSinging(std::function<void(RE::FormID a_singer)> a_onLyric)
	{
		if (!PublicRegisterEventCallback) {
			return;
		}
		PublicRegisterEventCallback("dialogue_background", [onLyric = std::move(a_onLyric)](const char* a_json) {
			try {
			const std::string raw = a_json ? a_json : "";
			std::string       low = raw;
			std::ranges::transform(low, low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (low.find("singing") == std::string::npos) {
				return;  // ordinary background chatter
			}
			static std::once_flag shown;
			std::call_once(shown, [&] { logs::info("Sung line seen (first one, for reference): {}", raw.substr(0, 600)); });
			const auto event = nlohmann::json::parse(raw, nullptr, false);
			const auto it = event.is_object() ? event.find("originatingActorFormId") : event.end();
			if (event.is_object() && it != event.end() && it->is_number_integer()) {
				onLyric(it->get<RE::FormID>());
			}
			} catch (const std::exception& e) {
				logs::warn("Background line could not be read ({})", e.what());
			}
		});
	}

	namespace
	{
		std::atomic<std::uint32_t> g_session{ 1 };
	}

	std::uint32_t Session()
	{
		return g_session.load();
	}

	void NewSession()
	{
		++g_session;
	}

	bool AskDecision(const char* a_template, const std::string& a_contextJson, DecisionHandler a_onAnswer)
	{
		if (!PublicSendCustomDecisionToLLM) {
			return false;
		}
		return PublicSendCustomDecisionToLLM(a_template, a_contextJson.c_str(), 0,
			[onAnswer = std::move(a_onAnswer)](const char* a_answer, const char* a_json, int a_success) {
				double confidence = 0.0;
				bool   ok = a_success != 0;
				try {
					if (ok) {
						const auto json = nlohmann::json::parse(a_json ? a_json : "", nullptr, false);
						if (json.is_object() && json.contains("answers") && json["answers"].is_object()) {
							for (const auto& [id, answer] : json["answers"].items()) {
								confidence = JsonField::Number(answer, "confidence", 1.0);
								break;
							}
						}
					} else {
						logs::warn("Decision request failed: {}", a_json ? a_json : "");
					}
				} catch (const std::exception& e) {
					ok = false;
					logs::warn("Decision reply could not be read ({})", e.what());
				}
				try {
					onAnswer(ok, ok && a_answer ? a_answer : "", confidence);
				} catch (const std::exception& e) {
					logs::warn("Decision handler failed ({})", e.what());
				}
			});
	}

	bool AskPrompt(const char* a_prompt, const std::string& a_contextJson, ReplyHandler a_onReply)
	{
		if (!PublicSendCustomPromptToLLM) {
			return false;
		}
		return PublicSendCustomPromptToLLM(a_prompt, "", a_contextJson.c_str(),
			[onReply = std::move(a_onReply)](const char* a_response, int a_success) {
				if (!a_success) {
					logs::warn("Prompt request failed: {}", a_response ? a_response : "");
				}
				try {
					onReply(a_success != 0, a_response ? a_response : "");
				} catch (const std::exception& e) {
					logs::warn("Prompt handler failed ({})", e.what());
				}
			});
	}

	std::string RecentEvents(RE::FormID a_actor, int a_max)
	{
		if (!PublicGetRecentEvents) {
			return {};
		}
		const auto events = nlohmann::json::parse(PublicGetRecentEvents(a_actor, a_max, ""), nullptr, false);
		if (!events.is_array()) {
			return {};
		}
		std::vector<std::pair<double, std::string>> lines;
		for (const auto& event : events) {
			if (!event.is_object()) {
				continue;
			}
			std::string text;
			if (const auto it = event.find("data"); it != event.end()) {
				const auto data = it->is_string() ? nlohmann::json::parse(it->get<std::string>(), nullptr, false) : *it;
				if (data.is_object()) {
					text = JsonField::Text(data, "dialogue", JsonField::Text(data, "text", JsonField::Text(data, "description")));
				} else if (it->is_string()) {
					text = it->get<std::string>();
				}
			}
			if (text.empty()) {
				continue;
			}
			const auto who = JsonField::Text(event, "originatingActorName");
			const auto time = event.contains("gameTime") && event["gameTime"].is_number() ? event["gameTime"].get<double>() : 0.0;
			lines.emplace_back(time, who.empty() ? text : who + ": " + text);
		}
		std::ranges::stable_sort(lines, {}, &std::pair<double, std::string>::first);
		std::string out;
		for (const auto& [time, line] : lines) {
			out += "- " + line + "\n";
		}
		return out;
	}

	std::string RecentMemories(RE::FormID a_actor, int a_max)
	{
		MemoryQuery query;
		query.maxCount = a_max;
		query.excludeTags = { "dream" };
		query.excludeMemoryTypes = { "KNOWLEDGE" };
		query.orderBy = MemoryOrder::GameTimeDesc;
		const auto memories = nlohmann::json::parse(QueryMemoriesForActor(a_actor, query), nullptr, false);
		if (!memories.is_array()) {
			return {};
		}
		std::vector<std::string> lines;
		for (const auto& memory : memories) {
			const auto text = JsonField::Text(memory, "text", JsonField::Text(memory, "content"));
			if (!text.empty()) {
				lines.push_back("- " + text + "\n");
			}
		}
		std::string out;
		for (auto it = lines.rbegin(); it != lines.rend(); ++it) {  // newest first from SkyrimNet
			out += *it;
		}
		return out;
	}

	bool AddMemory(RE::FormID a_actor, const std::string& a_text, float a_importance, const char* a_type,
		const std::string& a_emotion, const std::string& a_location, const std::string& a_tagsJson)
	{
		return PublicAddMemory &&
		       PublicAddMemory(a_actor, a_text.c_str(), a_importance, a_type, a_emotion.c_str(), a_location.c_str(), a_tagsJson.c_str(), "[]") > 0;
	}

	bool IsBusy(RE::FormID a_actor)
	{
		return PublicIsActorBusy && PublicIsActorBusy(a_actor);
	}

	int RegisterActions()
	{
		if (!PublicRegisterCPPAction || !PublicRegisterCPPSubCategory) {
			logs::warn("This SkyrimNet has no C++ action registration: the dog's actions are off");
			return 0;
		}
		constexpr auto kCategory = "DogBehavior";
		constexpr auto kTriggers = "dialogue";
		// Above the usual 50, so the dog's own category is listed (and checked) before the general ones. The model
		// picks a category by its description first, so it names lying down and resting: a dog's way of doing what
		// furniture actions offer people.
		constexpr int  kPriority = 100;
		if (!PublicRegisterCPPSubCategory(kCategory,
				"A dog's own body and needs: body-language poses that show his mood (flopped on his side, chin on his paws, "
				"curled up tight, a play bow to invite play, a happy full-body wiggle), napping on the ground, asking for food, seeking a scratch, running off some energy, "
				"sniffing someone over, howling along with a bard, snapping up or stealing food. "
				"A dog lies down where he is; he does not use beds, chairs or other furniture.",
				[](RE::Actor* a_actor) { return Canine::Actions::IsDog(a_actor); }, kTriggers, kPriority, "{}", kCategory, "", "")) {
			logs::warn("Action category {} was not registered", kCategory);
		}
		int count = 0;
		for (auto& spec : Canine::Actions::All()) {
			// A top-level action isn't behind the category's dog check, so it carries its own.
			auto eligible = spec.topLevel ? std::function<bool(RE::Actor*)>([inner = spec.eligible](RE::Actor* a_actor) {
				return Canine::Actions::IsDog(a_actor) && inner(a_actor);
			})
			                              : spec.eligible;
			std::function<bool(RE::Actor*, std::string)> run;
			if (spec.executeWithParams) {
				run = [execute = spec.executeWithParams](RE::Actor* a_actor, std::string a_params) {
					return Canine::Actions::IsDog(a_actor) && execute(a_actor, a_params);
				};
			} else {
				run = [execute = spec.execute](RE::Actor* a_actor, std::string) { return Canine::Actions::IsDog(a_actor) && execute(a_actor); };
			}
			const bool ok = PublicRegisterCPPAction(spec.name, spec.description, eligible, run, kTriggers,
				spec.topLevel ? "CPP" : "CPP_NESTED_ACTION", kPriority, spec.schema, spec.topLevel ? "" : kCategory, "", "");
			if (ok) {
				++count;
			} else {
				logs::warn("Action {} was not registered", spec.name);
			}
		}
		// The one action for everyone but him: only offered to someone already right beside him.
		if (PublicRegisterCPPAction("PetDog",
				"Use when this person pets or fusses over the dog right beside them: they kneel where they stand and scratch him behind the ears.",
				[](RE::Actor* a_actor) { return Canine::Actions::CanPetHim(a_actor); },
				[](RE::Actor* a_actor, std::string) {
					if (!Canine::Actions::CanPetHim(a_actor)) {
						return false;
					}
					SKSE::GetTaskInterface()->AddTask([id = a_actor->GetFormID()] { Canine::Actions::PettedByNpc(id); });
					return true;
				},
				kTriggers, "CPP", 50, "{}", "", "", "")) {
			++count;
		} else {
			logs::warn("Action PetDog was not registered");
		}
		return count;
	}

	int RegisterDecorators()
	{
		if (!PublicRegisterDecorator) {
			return 0;
		}
		int count = 0;
		auto add = [&count](const std::string& a_name, const char* a_description, std::function<std::string(RE::Actor*)> a_callback) {
			if (PublicRegisterDecorator(a_name.c_str(), a_description, std::move(a_callback))) {
				++count;
			} else {
				logs::warn("Decorator {} was not registered (name taken?)", a_name);
			}
		};

		using Canine::Drive;
		constexpr std::array<std::pair<Drive, const char*>, Canine::kDriveCount> kValues{ {
			{ Drive::kHunger, "Canine Drives: the dog's hunger, 0-100 (higher = hungrier); empty for other actors" },
			{ Drive::kAffection, "Canine Drives: the dog's need for affection, 0-100; empty for other actors" },
			{ Drive::kRestless, "Canine Drives: the dog's restlessness, 0-100; empty for other actors" },
			{ Drive::kTired, "Canine Drives: the dog's tiredness, 0-100; empty for other actors" },
			{ Drive::kSeparation, "Canine Drives: how much the dog misses the player, 0-100; empty for other actors" },
		} };
		for (const auto& [drive, description] : kValues) {
			add("canine_" + std::string(Canine::kDriveKeys[static_cast<std::size_t>(drive)]), description,
				[drive](RE::Actor* a_actor) { return Canine::Drives::DecoratorValue(a_actor, drive); });
		}
		add("canine_needs", "Canine Drives: the dog's strongest bodily needs as prompt text; empty when content or for other actors",
			[](RE::Actor* a_actor) { return Canine::Drives::DecoratorNeeds(a_actor); });
		// For any actor, not only the dog: SkyrimNet's isCapableOfSpeech is true for a creature that has been
		// set up to talk, and actor_has_keyword doesn't see race keywords, so the race is read here.
		add("canine_is_animal", "Canine Drives: \"true\" when the actor's race is an animal or creature (dogs, chickens, horses...), else empty",
			[](RE::Actor* a_actor) -> std::string {
				const auto race = a_actor ? a_actor->GetRace() : nullptr;
				if (!race) {
					return "";
				}
				constexpr RE::FormID kActorTypeNPC = 0x00013794, kActorTypeCreature = 0x00013795, kActorTypeAnimal = 0x00013798;
				const auto keyword = [&](RE::FormID a_id) {
					const auto form = RE::TESForm::LookupByID<RE::BGSKeyword>(a_id);
					return form && race->HasKeyword(form);
				};
				return keyword(kActorTypeAnimal) || keyword(kActorTypeCreature) || !keyword(kActorTypeNPC) ? "true" : "";
			});
		add("canine_state","Canine Drives: what the dog is doing (awake, eating, napping); empty for other actors",
			[](RE::Actor* a_actor) { return Canine::Drives::DecoratorState(a_actor); });
		// The dog's own name when the actor handed in is the dog, else "". Prompts compare it with decnpc(npc.UUID).name:
		// SkyrimNet once handed the dog to these decorators while rendering another NPC (a Dremora without a bio), so the
		// dog blocks went into his prompt. The name check keeps them out whatever actor arrives.
		add("canine_name", "Canine Drives: the dog's display name when the actor is the tracked dog, else empty",
			[](RE::Actor* a_actor) -> std::string {
				if (Canine::Drives::DecoratorState(a_actor).empty()) {
					return "";
				}
				const char* name = a_actor->GetDisplayFullName();
				return name ? name : "";
			});
		// Diagnostic: which actor canine_state is actually handed (at most once a minute per actor), to see whether
		// SkyrimNet ever passes the dog while rendering someone else.
		add("canine_seen", "Canine Drives: diagnostic, always empty; logs the actor it was handed",
			[](RE::Actor* a_actor) -> std::string {
				static std::mutex lock;
				static std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> last;
				if (a_actor) {
					std::scoped_lock guard(lock);
					const auto now = std::chrono::steady_clock::now();
					if (auto& when = last[a_actor->GetFormID()]; now - when > 60s) {
						when = now;
						logs::info("Decorators handed {} ({:08X})", a_actor->GetDisplayFullName(), a_actor->GetFormID());
					}
				}
				return "";
			});
		add("canine_now", "Canine Drives: what the dog notices or is doing right now (a scent, music, warming the player), as plain sentences; empty for other actors",
			[](RE::Actor* a_actor) { return Canine::Senses::DecoratorNow(a_actor); });
		add("canine_lessons", "Canine Drives: what the dog has learned from the player's praise and scolding, as plain sentences; empty when nothing is clear yet or for other actors",
			[](RE::Actor* a_actor) { return Canine::Training::DecoratorLessons(a_actor); });
		return count;
	}
}
