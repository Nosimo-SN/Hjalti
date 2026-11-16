// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#include "behavior.h"

#include <ranges>

#include "actions.h"
#include "bridge.h"
#include "combat.h"
#include "drives.h"
#include "idles.h"
#include "inputs.h"
#include "training.h"

#include <nlohmann/json.hpp>

namespace Canine::Behavior
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr auto  kPoll = 5s;
		constexpr auto  kDogQuiet = 30s;        // after his own last line
		constexpr auto  kRepeatGap = 5min;      // the same behavior again only after this, unless the need is urgent
		constexpr int   kWorthAsking = 40;      // some need at least noticeable, or there's nothing to decide
		constexpr int   kUrgent = 85;
		constexpr float kPlayerNear = 1500.0f;
		constexpr int   kLeakWords = 3;         // real words outside actions and tags before a line counts as speech
		constexpr RE::FormID kPlayer = 0x14;
		constexpr auto  kFeltGap = 30s;         // praise or scolding counts again only after this
		constexpr auto  kFeedDelay = 5s;        // before talk-to-feed hands food over (a trade window may open instead)

		std::atomic<RE::FormID> g_target{ 0 };
		std::atomic_bool        g_inGame{ false };
		std::atomic_bool        g_asking{ false };
		std::atomic_bool        g_hearing{ false };  // a perception decision is out
		std::atomic<Clock::rep> g_lastLine{ 0 };     // anyone's
		std::atomic<Clock::rep> g_lastDogLine{ 0 };  // his

		// Game thread only.
		std::string                      g_lastBehavior;
		std::optional<Clock::time_point> g_lastBehaviorAt;
		std::optional<Clock::time_point> g_nextDue;
		std::optional<Clock::time_point> g_lastFelt;  // last praise or scolding that counted
		std::mt19937                     g_rng{ std::random_device{}() };

		Clock::rep Now() { return Clock::now().time_since_epoch().count(); }

		bool SinceAtLeast(const std::atomic<Clock::rep>& a_when, Clock::duration a_gap)
		{
			const auto when = a_when.load();
			return when == 0 || Clock::now() - Clock::time_point(Clock::duration(when)) >= a_gap;
		}

		std::string Name(RE::TESObjectREFR* a_ref, const char* a_fallback)
		{
			const char* name = a_ref ? a_ref->GetDisplayFullName() : nullptr;
			return name && *name ? name : a_fallback;
		}

		// --- leak repair ---------------------------------------------------------------------------------

		// Words a dog can "say": sounds, not speech.
		bool IsDogSound(const std::string& a_word)
		{
			static const std::array<std::string_view, 24> kSounds{ "woof", "wuff", "ruff", "arf", "bark", "barks", "yip", "yips",
				"yap", "whine", "whines", "whimper", "grr", "grrr", "growl", "hrr", "hrrr", "huff", "howl", "awoo", "bawk", "hmph", "ah", "oh" };
			return std::ranges::find(kSounds, a_word) != kSounds.end();
		}

		// Counts real words outside *actions*, [audio tags] and <internal_thought> blocks.
		int SpokenWords(std::string_view a_text)
		{
			std::string kept;
			bool        action = false;
			int         tag = 0;
			for (std::size_t i = 0; i < a_text.size(); ++i) {
				if (a_text.substr(i).starts_with("<internal_thought>")) {
					const auto end = a_text.find("</internal_thought>", i);
					if (end == std::string_view::npos) {
						break;
					}
					i = end + std::string_view("</internal_thought>").size() - 1;
					continue;
				}
				const char c = a_text[i];
				if (c == '*') {
					action = !action;
				} else if (c == '[') {
					++tag;
				} else if (c == ']' && tag > 0) {
					--tag;
				} else if (!action && tag == 0) {
					kept += c;
				}
			}
			// "**Valiant** projects telepathically to Saga:" leaves its words outside the bold markers, so it
			// counts as speech too, which it is when it comes out loud.
			int         words = 0;
			std::string word;
			auto        flush = [&] {
				if (word.size() >= 2 && !IsDogSound(word)) {
					++words;
				}
				word.clear();
			};
			for (const char c : kept) {
				if (std::isalpha(static_cast<unsigned char>(c)) || c == '\'') {
					word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				} else {
					flush();
				}
			}
			flush();
			return words;
		}

		// Words that are voiced when his line plays: plain text and *action* text, not <internal_thought> blocks or
		// [audio tags]. His lines are mostly actions, so counting only spoken words gave 0 and counting everything
		// (thoughts too) overshot; the gift menu waits for this many.
		int VoicedWords(std::string_view a_text)
		{
			int  words = 0;
			bool inWord = false;
			int  tag = 0;
			for (std::size_t i = 0; i < a_text.size(); ++i) {
				if (a_text.substr(i).starts_with("<internal_thought>")) {
					const auto end = a_text.find("</internal_thought>", i);
					if (end == std::string_view::npos) {
						break;
					}
					i = end + std::string_view("</internal_thought>").size() - 1;
					inWord = false;
					continue;
				}
				const char c = a_text[i];
				if (c == '[') {
					++tag;
				} else if (c == ']' && tag > 0) {
					--tag;
				}
				const bool letter = tag == 0 && (std::isalpha(static_cast<unsigned char>(c)) || c == '\'');
				words += letter && !inWord;
				inWord = letter;
			}
			return words;
		}

		void RepairLeak(RE::FormID a_dog, int a_words)
		{
			const auto dog = RE::TESForm::LookupByID<RE::Actor>(a_dog);
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!dog || !player) {
				return;
			}
			const auto dogName = Name(dog, "The dog");
			const auto playerName = Name(player, "the player");
			logs::info("Spoken line with {} words: posting the mind-link correction", a_words);
			Bridge::PostEvent(std::format("Whatever {} seemed to say just now reached only {}'s mind; to everyone else he only barked and whined.",
								  dogName, playerName),
				dog->GetFormID(), 0);
		}

		// --- hearing: the player's lines to him ------------------------------------------------------------

		bool Contains(std::string_view a_text, std::string_view a_word)
		{
			return !a_word.empty() && std::ranges::search(a_text, a_word, [](char a, char b) {
				return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
			}).begin() != a_text.end();
		}

		// Game thread.
		void React(bool a_ok, const std::string& a_answer, double a_confidence, const std::string& a_line)
		{
			g_hearing = false;
			const auto s = SnapshotSettings();
			logs::info("Heard '{}': {} ({:.2f})", a_line, a_ok ? a_answer : "no answer"s, a_confidence);
			const auto dog = RE::TESForm::LookupByID<RE::Actor>(g_target.load());
			if (!a_ok || a_confidence < s.perceptionConfidence || !dog || dog->IsDead()) {
				return;
			}
			if (a_answer == "praise" || a_answer == "scold") {
				const auto now = Clock::now();
				if (g_lastFelt && now - *g_lastFelt < kFeltGap) {
					return;  // one telling-off or one "good boy" counts, not a stream of them
				}
				g_lastFelt = now;
				const bool praise = a_answer == "praise";
				Drives::Adjust(Drive::kAffection, praise ? -s.praiseAffection : s.scoldAffection, s);
				logs::info("{} (affection -> {:.1f})", praise ? "Praised" : "Scolded", Drives::Value(Drive::kAffection));
				Training::Feedback(praise, s);
				if (praise && dog->Is3DLoaded() && !Actions::Napping()) {
					// A happy bark once the player's voiced line is over, so it isn't lost under it.
					const auto words = std::ranges::count_if(std::string_view(a_line) | std::views::split(' '),
						[](auto&& a_word) { return !std::ranges::empty(a_word); });
					const auto wait = std::clamp(1000ms + 400ms * words, 1500ms, 8000ms);
					std::thread([id = dog->GetFormID(), wait, session = Bridge::Session()] {
						std::this_thread::sleep_for(wait);
						SKSE::GetTaskInterface()->AddTask([id, session] {
							const auto dog = session == Bridge::Session() ? RE::TESForm::LookupByID<RE::Actor>(id) : nullptr;
							if (dog && dog->Is3DLoaded() && !dog->IsDead() && !Actions::Napping()) {
								Actions::Bark(dog, false);
							}
						});
					}).detach();
				}
			} else if (a_answer == "call_off") {
				Combat::CallOff(dog);
			} else if (a_answer == "feed" && s.talkFeed) {
				// Our own AskForFood keeps its gift menu shut now. Still wait a moment: the same words may have SkyrimNet
				// open its trade window, and then the food goes that way.
				Inputs::PromiseFood();
				std::thread([dog = dog->GetFormID(), line = a_line, session = Bridge::Session()] {
					std::this_thread::sleep_for(kFeedDelay);
					SKSE::GetTaskInterface()->AddTask([dog, line, session] {
						if (session != Bridge::Session()) {
							return;  // loaded another save meanwhile
						}
						if (const auto actor = RE::TESForm::LookupByID<RE::Actor>(dog); actor && Inputs::FeedFromPlayer(actor, line)) {
							Actions::Wake("fed");
						}
					});
				}).detach();
			}
		}

		// A player line that named nobody (typed and spoken lines carry no target until SkyrimNet picks who answers):
		// if he's the one who answers it, it was meant for him.
		struct Unclaimed
		{
			std::string       type;
			std::string       line;
			Clock::time_point at;
		};
		std::optional<Unclaimed> g_unclaimed;  // game thread
		constexpr auto           kAnswerWindow = 20s;

		// Game thread: was the line meant for him? Then ask how it was meant. a_answered: he answered it himself.
		void Listen(const std::string& a_type, RE::FormID a_target, const std::string& a_line, bool a_answered = false)
		{
			const auto s = SnapshotSettings();
			const auto dog = RE::TESForm::LookupByID<RE::Actor>(g_target.load());
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!s.perceptionEnabled || !dog || !player || dog->IsDead() || !dog->Is3DLoaded() || a_line.empty()) {
				return;
			}
			const auto dogName = Name(dog, "");
			const bool together = dog->GetParentCell() == player->GetParentCell() || dog->GetWorldspace() == player->GetWorldspace();
			const bool addressed = a_target == dog->GetFormID() ||
			                       (Contains(a_line, dogName) && together &&dog->GetPosition().GetDistance(player->GetPosition()) <= kPlayerNear);
			if (!addressed && !a_answered) {
				if (a_target == 0 && together && dog->GetPosition().GetDistance(player->GetPosition()) <= kPlayerNear) {
					g_unclaimed = Unclaimed{ a_type, a_line, Clock::now() };  // wait and see who answers it
				} else {
					const auto aimed = a_target ? RE::TESForm::LookupByID<RE::Actor>(a_target) : nullptr;
					logs::info("Not for him: '{}' (to {})", a_line, aimed ? Name(aimed, "?") : a_target ? "someone"s : "nobody nearby"s);
				}
				return;
			}
			g_unclaimed.reset();
			if (g_hearing) {
				logs::info("Heard '{}' while still weighing the last line, skipped", a_line);
				return;
			}
			nlohmann::json ctx;
			ctx["dogFormId"] = dog->GetFormID();
			ctx["playerName"] = Name(player, "the player");
			ctx["line"] = a_line;
			ctx["telepathy"] = a_type == "dialogue_player_telepathy";
			ctx["hunger"] = Drives::Peek(dog, Drive::kHunger).value_or(0);

			g_hearing = true;
			const bool sent = Bridge::AskDecision("canine/perception", ctx.dump(), [line = a_line, session = Bridge::Session()](bool a_ok, std::string a_answer, double a_confidence) {
				SKSE::GetTaskInterface()->AddTask([a_ok, answer = std::move(a_answer), a_confidence, line, session] {
					if (session != Bridge::Session()) {
						logs::info("Heard reply from before the last load, dropped");
						return;
					}
					React(a_ok, answer, a_confidence, line);
				});
			});
			if (!sent) {
				g_hearing = false;
			}
		}

		void OnLine(std::string_view a_type, RE::FormID a_speaker, RE::FormID a_lineTarget, std::string_view a_text)
		{
			g_lastLine = Now();
			if (a_speaker == kPlayer) {
				SKSE::GetTaskInterface()->AddTask([type = std::string(a_type), a_lineTarget, line = std::string(a_text)] { Listen(type, a_lineTarget, line); });
				return;
			}
			const auto target = g_target.load();
			if (!target || a_speaker != target) {
				if (a_speaker && (a_type == "dialogue" || a_type == "dialogue_npc")) {
					SKSE::GetTaskInterface()->AddTask([] { g_unclaimed.reset(); });  // someone else answered the player
				}
				return;
			}
			g_lastDogLine = Now();
			// He answered the player's last unaddressed line: it was meant for him after all.
			SKSE::GetTaskInterface()->AddTask([] {
				if (!g_unclaimed) {
					return;
				}
				auto pending = std::move(*g_unclaimed);
				g_unclaimed.reset();
				if (Clock::now() - pending.at <= kAnswerWindow) {
					logs::info("He answered '{}': it was for him", pending.line);
					Listen(pending.type, 0, pending.line, true);
				}
			});
			Actions::HeardDogLine(VoicedWords(a_text));
			// Anything he says means someone has his attention: a nap ends (and his AI comes back on).
			SKSE::GetTaskInterface()->AddTask([] { Actions::Wake("spoken to", true); });
			// Telepathic replies are words by design; only his audible lines are checked.
			if (a_type != "dialogue" && a_type != "dialogue_npc") {
				return;
			}
			const int words = SpokenWords(a_text);
			if (words < kLeakWords || !SnapshotSettings().leakRepair) {
				return;
			}
			SKSE::GetTaskInterface()->AddTask([target, words] { RepairLeak(target, words); });
		}

		// --- behavior loop -------------------------------------------------------------------------------

		bool QuietMenus()
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

		std::string_view EventLine(std::string_view a_behavior)
		{
			if (a_behavior == "beg") {
				return "{dog} sits in front of {player}, whining softly with his eyes on their pack.";
			}
			if (a_behavior == "nap") {
				return "{dog} curls up nearby and dozes off.";
			}
			if (a_behavior == "seek_affection") {
				return "{dog} presses against {player}'s legs and nudges a hand for a scratch.";
			}
			return {};
		}

		// Play and Sniff post their own event from Actions.
		bool SelfTold(std::string_view a_behavior)
		{
			return a_behavior == "play" || a_behavior == "sniff" || a_behavior == "scavenge" || a_behavior == "sing" || a_behavior == "hunt" ||
			       a_behavior == "eat_carcass" || a_behavior == "fish";
		}

		bool ForFood(std::string_view a_behavior)
		{
			return a_behavior == "beg" || a_behavior == "scavenge" || a_behavior == "hunt" || a_behavior == "eat_carcass" || a_behavior == "fish";
		}

		void Execute(bool a_ok, const std::string& a_answer, double a_confidence)
		{
			g_asking = false;
			const auto s = SnapshotSettings();
			logs::info("Behavior loop: {} ({:.2f})", a_ok ? a_answer : "no answer"s, a_confidence);
			if (!a_ok || a_answer == "idle" || (EventLine(a_answer).empty() && !SelfTold(a_answer))) {
				return;
			}
			if (a_confidence < s.loopConfidence) {
				logs::info("Behavior loop: {} not sure enough ({:.2f} < {:.2f}), skipped", a_answer, a_confidence, s.loopConfidence);
				return;
			}
			const auto dog = RE::TESForm::LookupByID<RE::Actor>(g_target.load());
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!dog || !player || !dog->Is3DLoaded() || dog->IsDead() || dog->IsInCombat() || player->IsInCombat() || !QuietMenus()) {
				return;  // the moment passed while the model was thinking
			}
			if (ForFood(a_answer) && Actions::Hurt(dog)) {
				logs::info("Behavior loop: {} skipped, he's too hurt", a_answer);
				return;
			}
			const auto now = Clock::now();
			if (a_answer == g_lastBehavior && g_lastBehaviorAt && now - *g_lastBehaviorAt < kRepeatGap) {
				const Drive drive = ForFood(a_answer) ? Drive::kHunger : a_answer == "nap" ? Drive::kTired :
				                    SelfTold(a_answer)  ? Drive::kRestless : Drive::kAffection;
				if (Drives::Value(drive) < kUrgent) {
					logs::info("Behavior loop: {} again so soon, skipped", a_answer);
					return;
				}
			}
			if (!Actions::Perform(dog, a_answer)) {
				return;
			}
			g_lastBehavior = a_answer;
			g_lastBehaviorAt = now;
			if (s.events && !SelfTold(a_answer)) {
				std::string line(EventLine(a_answer));
				for (const auto& [token, value] : { std::pair{ "{dog}"s, Name(dog, "The dog") }, std::pair{ "{player}"s, Name(player, "the player") } }) {
					for (auto pos = line.find(token); pos != std::string::npos; pos = line.find(token, pos + value.size())) {
						line.replace(pos, token.size(), value);
					}
				}
				if (a_answer == "nap") {
					Actions::TellWhenSettled(std::move(line));  // once he's down and his AI paused, not mid lie-down
				} else {
					Bridge::PostEvent(line, dog->GetFormID(), player->GetFormID(), s.narrate);
				}
			}
		}

		// Game thread: is now a quiet moment, and is there anything to decide?
		void TryLoop()
		{
			const auto s = SnapshotSettings();
			const auto dog = RE::TESForm::LookupByID<RE::Actor>(g_target.load());
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto calendar = RE::Calendar::GetSingleton();
			if (!s.loopEnabled || g_asking || !dog || !player || !calendar || !dog->Is3DLoaded() || dog->IsDead()) {
				return;
			}
			if (dog->IsInCombat() || player->IsInCombat() || !QuietMenus() || Actions::Napping() || Actions::Roaming() || Inputs::State() == "eating") {
				return;
			}
			if (dog->GetParentCell() != player->GetParentCell() && dog->GetWorldspace() != player->GetWorldspace()) {
				return;
			}
			if (dog->GetPosition().GetDistance(player->GetPosition()) > kPlayerNear || Bridge::IsBusy(dog->GetFormID())) {
				return;
			}
			if (!SinceAtLeast(g_lastLine, std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s.loopQuietSeconds))) ||
				!SinceAtLeast(g_lastDogLine, kDogQuiet)) {
				return;
			}

			nlohmann::json ctx;
			int            strongest = 0;
			for (std::size_t d = 0; d < kDriveCount; ++d) {
				const int value = Drives::Peek(dog, static_cast<Drive>(d)).value_or(0);
				ctx[std::string(kDriveKeys[d])] = value;
				strongest = (std::max)(strongest, value);
			}
			if (strongest < kWorthAsking) {
				return;
			}
			ctx["dogFormId"] = dog->GetFormID();
			ctx["needs"] = Drives::DecoratorNeeds(dog);
			const auto cell = dog->GetParentCell();
			ctx["indoors"] = cell && cell->IsInteriorCell();
			ctx["hour"] = static_cast<int>(calendar->GetHour());
			ctx["playerName"] = Name(player, "the player");
			ctx["lessons"] = Training::Text();
			ctx["foodNearby"] = Actions::FoodInReach();
			ctx["preyNearby"] = Actions::PreyInReach();
			ctx["fishNearby"] = Actions::FishInReach();
			ctx["carcassNearby"] = Actions::CarcassInReach();
			ctx["songNearby"] = Actions::SongToJoin(dog);
			ctx["lastBehavior"] = g_lastBehavior;
			ctx["minutesSinceLast"] = g_lastBehaviorAt ?
			                              static_cast<int>(std::chrono::duration_cast<std::chrono::minutes>(Clock::now() - *g_lastBehaviorAt).count()) :
			                              0;

			g_asking = true;
			const bool sent = Bridge::AskDecision("canine/next_behavior", ctx.dump(), [session = Bridge::Session()](bool a_ok, std::string a_answer, double a_confidence) {
				SKSE::GetTaskInterface()->AddTask([a_ok, answer = std::move(a_answer), a_confidence, session] {
					if (session != Bridge::Session()) {
						logs::info("Behavior loop: reply from before the last load, dropped");
						return;
					}
					Execute(a_ok, answer, a_confidence);
				});
			});
			if (!sent) {
				g_asking = false;
			}
		}

		void LoopThread()
		{
			for (;;) {
				std::this_thread::sleep_for(kPoll);
				if (!g_inGame) {
					continue;
				}
				const auto s = SnapshotSettings();
				SKSE::GetTaskInterface()->AddTask([min = s.loopMinSeconds, max = s.loopMaxSeconds] {
					const auto now = Clock::now();
					if (!g_nextDue) {
						g_nextDue = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(min));
						return;
					}
					if (now < *g_nextDue) {
						return;
					}
					std::uniform_real_distribution<double> wait(min, max);
					g_nextDue = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(wait(g_rng)));
					TryLoop();
				});
			}
		}
	}

	void Start()
	{
		Bridge::WatchDialogue(OnLine);
		std::thread(LoopThread).detach();
	}

	void SetTarget(RE::FormID a_ref)
	{
		g_target = a_ref;
	}

	void SetInGame(bool a_inGame)
	{
		g_inGame = a_inGame;
	}

	void ResetSession()
	{
		g_lastBehavior.clear();
		g_lastBehaviorAt.reset();
		g_nextDue.reset();
		g_lastFelt.reset();
		g_asking = false;
		g_hearing = false;
	}
}
