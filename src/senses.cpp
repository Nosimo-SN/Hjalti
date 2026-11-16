// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#include "senses.h"

#include "actions.h"
#include "bridge.h"
#include "combat.h"
#include "progression.h"
#include "runes.h"
#include "gifts.h"

#include <chrono>
#include <set>

namespace Canine::Senses
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// Smell: hostile actors within this range of him, checked every few seconds.
		constexpr auto  kScentEvery = 1s;
		// He reacts out loud to a new scent this close, once per creature, with this long between reactions.
		constexpr float kAlertShare = 0.75f;  // of the smell range
		constexpr auto  kAlertCooldown = 3min;
		// Music: a bard this close to him, in the same cell.
		constexpr float kSongRange = 2500.0f;
		constexpr auto  kLyricHold = 15s;   // a SkyrimNet song sings a line every ~8 s: still playing this long after one
		constexpr auto  kNewSongGap = 20s;  // quiet this long, and the next music is a new song

		// The quests that run while a bard performs a vanilla song (and the mods' copies of them).
		constexpr std::array<const char*, 7> kBardQuests{ "BardSongs", "BardSongsInstrumental", "FalskaarBardSongs", "CH_BardSongs",
			"CH_BardSongsInstrumental", "AAKingBardSongs", "AAKingBardSongsInstrumental" };

		constexpr RE::FormID kActorTypeNPC = 0x013794, kActorTypeUndead = 0x013796, kActorTypeDaedra = 0x013797,
							 kActorTypeDwarven = 0x01397A;

		std::mutex  g_lock;
		std::string g_scent;    // the scent fact, or ""
		std::string g_music;    // the music fact, or ""
		std::string g_warming;  // the warming fact, or ""
		std::optional<Song> g_song;

		std::atomic<Clock::rep>  g_lyricAt{ 0 };   // SkyrimNet's last sung line, and who sang it
		std::atomic<RE::FormID>  g_lyricBard{ 0 };
		Clock::time_point        g_nextScent{};
		Clock::time_point        g_songSeen{};
		std::uint32_t            g_songIds = 0;
		std::string              g_lastScentLog;
		std::set<RE::FormID>     g_alerted;      // who he has already reacted to (cleared once the air is clean)
		std::optional<Clock::time_point> g_lastAlert;
		constexpr auto                   kScentShowFor = 30s;       // a new scent stays in his prompt this long
		constexpr float                  kScentCloseThreat = 1500.0f;  // a new threat this close breaks the quiet
		std::set<RE::FormID>             g_scentNoted;              // who has had their moment (cleared once the air is clean)
		std::optional<Clock::time_point> g_scentUntil;
		std::optional<Clock::time_point> g_scentQuietUntil;

		std::string Name(RE::TESObjectREFR* a_ref, std::string_view a_fallback)
		{
			const char* name = a_ref ? a_ref->GetDisplayFullName() : nullptr;
			return name && *name ? name : std::string(a_fallback);
		}

		bool HasRaceKeyword(RE::Actor* a_actor, RE::FormID a_keyword)
		{
			const auto race = a_actor->GetRace();
			const auto keyword = RE::TESForm::LookupByID<RE::BGSKeyword>(a_keyword);
			return race && keyword && race->HasKeyword(keyword);
		}

		// What the scent is to a dog's nose.
		std::string ScentOf(RE::Actor* a_actor, int a_count)
		{
			if (HasRaceKeyword(a_actor, kActorTypeUndead)) {
				return "the dry, grave-dust reek of the walking dead";
			}
			if (HasRaceKeyword(a_actor, kActorTypeDwarven)) {
				return "hot oil and old metal, something mechanical";
			}
			if (HasRaceKeyword(a_actor, kActorTypeDaedra)) {
				return "a sharp, burnt, brimstone smell, nothing natural";
			}
			if (HasRaceKeyword(a_actor, kActorTypeNPC)) {
				return a_count > 1 ? "unfamiliar people, steel and leather, more than one" : "an unfamiliar person, steel and leather";
			}
			const auto race = a_actor->GetRace();
			std::string kind = Name(a_actor, race ? race->GetFullName() : "an animal");
			std::ranges::transform(kind, kind.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_count > 1 ? std::format("{}, more than one", kind) : kind;
		}

		// Where something is, as the player faces.
		std::string Direction(RE::Actor* a_player, const RE::NiPoint3& a_where)
		{
			const auto  d = a_where - a_player->GetPosition();
			const float heading = a_player->GetAngleZ();
			const float ahead = d.x * std::sin(heading) + d.y * std::cos(heading);
			const float right = d.x * std::cos(heading) - d.y * std::sin(heading);
			if (std::abs(ahead) >= std::abs(right)) {
				return ahead >= 0.0f ? "ahead" : "behind";
			}
			return right >= 0.0f ? "off to the right" : "off to the left";
		}

		void UpdateScent(RE::Actor* a_dog, RE::PlayerCharacter* a_player, const Settings& a_settings)
		{
			const auto now = Clock::now();
			if (now < g_nextScent) {
				return;
			}
			g_nextScent = now + kScentEvery;
			std::string fact;
			bool        sensed = false;
			const auto  lists = RE::ProcessLists::GetSingleton();
			if (a_settings.smellEnabled && lists && a_dog->Is3DLoaded() && !a_dog->IsDead() && !a_player->IsInCombat()) {
				const float range = static_cast<float>(a_settings.smellRange);
				RE::Actor*  nearest = nullptr;
				float       nearestDistance = range;
				int         count = 0;
				// The fully active actors near the player, and the next tier out (still loaded, updated less often).
				std::vector<RE::Actor*> actors;
				for (const auto* list : { &lists->highActorHandles, &lists->middleHighActorHandles }) {
					for (const auto& handle : *list) {
						if (const auto ptr = handle.get()) {
							actors.push_back(ptr.get());
						}
					}
				}
				for (const auto actor : actors) {
					if (!actor || actor == a_dog || actor->IsPlayerRef() || actor->IsDead() || !actor->Is3DLoaded() ||
						!(actor->IsHostileToActor(a_player) || actor->IsHostileToActor(a_dog))) {
						continue;
					}
					const float distance = actor->GetPosition().GetDistance(a_dog->GetPosition());
					if (distance > range) {
						continue;
					}
					++count;
					if (distance <= nearestDistance) {
						nearest = actor;
						nearestDistance = distance;
					}
				}
				// Prey (an animal that won't fight, like an elk) gets an eager whine; anything else a growl.
				const auto owner = nearest ? nearest->AsActorValueOwner() : nullptr;
				const bool prey = nearest && !HasRaceKeyword(nearest, kActorTypeNPC) && owner && owner->GetActorValue(RE::ActorValue::kAggression) <= 0.0f;
				sensed = nearest != nullptr;
				// A scent is news for a moment, then old: without this, a field full of animals keeps it in every line he
				// speaks. Something new opens a short window, then the air goes quiet for a while; only a threat that's
				// close breaks the quiet. A creature already noted doesn't open another until the air clears.
				if (nearest && !g_scentNoted.contains(nearest->GetFormID()) &&
					(!g_scentQuietUntil || now >= *g_scentQuietUntil || (!prey && nearestDistance < kScentCloseThreat))) {
					g_scentNoted.insert(nearest->GetFormID());
					g_scentUntil = now + kScentShowFor;
					g_scentQuietUntil = *g_scentUntil + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(a_settings.smellQuiet));
				}
				const bool news = g_scentUntil && now < *g_scentUntil;
				if (news && nearest && nearestDistance <= range * kAlertShare && !g_alerted.contains(nearest->GetFormID()) && !a_dog->IsInCombat() &&
					!Actions::Napping() && !Actions::Roaming() && (!g_lastAlert || now - *g_lastAlert >= kAlertCooldown)) {
					g_alerted.insert(nearest->GetFormID());
					g_lastAlert = now;
					logs::info("Smell: reacting to {} ({})", Name(nearest, "?"), prey ? "prey: whine" : "threat: growl");
					Actions::ScentAlert(a_dog, !prey, Direction(a_player, nearest->GetPosition()));
				}
				if (nearest) {
					const auto dogName = Name(a_dog, "The dog");
					const auto distance = nearestDistance < 1200.0f ? "close by" : nearestDistance < 2500.0f ? "not far" :
					                      nearestDistance < 4500.0f ? "some way off" : "far off";
					if (news) {
						fact = std::format("{}'s nose has caught a scent that {}: {}, {} and {}.", dogName, prey ? "has him eager to chase" : "puts his hackles up",
							ScentOf(nearest, count), distance, Direction(a_player, nearest->GetPosition()));
					}
					const auto log = std::format("{} ({})", Name(nearest, "?"), count);
					if (log != g_lastScentLog) {
						logs::info("Smell: {} hostile within {:.0f}, nearest {} at {:.0f}", count, range, Name(nearest, "?"), nearestDistance);
						g_lastScentLog = log;
					}
				}
			}
			if (!sensed && !g_lastScentLog.empty()) {
				logs::info("Smell: nothing hostile on the air any more");
				g_lastScentLog.clear();
				g_alerted.clear();
				g_scentNoted.clear();
			}
			std::scoped_lock lock(g_lock);
			g_scent = std::move(fact);
		}

		std::set<RE::FormID> g_runningQuests;  // bard quests running at the last check
		bool                 g_newPerformance = false;

		// Everyone performing right now: the actors of every running bard quest (its Bard and second musician; never
		// the player) and the last SkyrimNet singer, if one sang recently. A quest that has just started is a new song.
		std::vector<RE::Actor*> Performers()
		{
			std::vector<RE::Actor*> out;
			std::set<RE::FormID>    running;
			for (const auto editorId : kBardQuests) {
				const auto quest = RE::TESForm::LookupByEditorID<RE::TESQuest>(editorId);
				if (!quest || !quest->IsRunning()) {
					continue;
				}
				running.insert(quest->GetFormID());
				if (!g_runningQuests.contains(quest->GetFormID())) {
					g_newPerformance = true;
				}
				for (const auto alias : quest->aliases) {
					const auto ref = alias && alias->GetVMTypeID() == RE::BGSRefAlias::VMTYPEID ? static_cast<RE::BGSRefAlias*>(alias) : nullptr;
					const auto actor = ref ? ref->GetActorReference() : nullptr;
					if (actor && !actor->IsPlayerRef()) {
						out.push_back(actor);
					}
				}
			}
			g_runningQuests = std::move(running);
			if (const auto at = g_lyricAt.load(); at != 0 && Clock::now() - Clock::time_point(Clock::duration(at)) <= kLyricHold) {
				if (const auto singer = RE::TESForm::LookupByID<RE::Actor>(g_lyricBard.load())) {
					out.push_back(singer);
				}
			}
			return out;
		}

		void UpdateSong(RE::Actor* a_dog)
		{
			const auto now = Clock::now();
			// The nearest performer in his room and in range, whichever way they were found.
			RE::Actor* bard = nullptr;
			float      bardDistance = kSongRange;
			for (const auto performer : Performers()) {
				if (!a_dog->Is3DLoaded() || !performer->Is3DLoaded() || performer->GetParentCell() != a_dog->GetParentCell()) {
					continue;
				}
				if (const float d = performer->GetPosition().GetDistance(a_dog->GetPosition()); d <= bardDistance) {
					bard = performer;
					bardDistance = d;
				}
			}
			const bool nearby = bard != nullptr;
			std::scoped_lock lock(g_lock);
			if (!nearby) {
				g_song.reset();
				g_music.clear();
				return;
			}
			if (!g_song || now - g_songSeen > kNewSongGap || g_newPerformance) {
				g_newPerformance = false;
				g_song = Song{ bard->GetFormID(), ++g_songIds };
				logs::info("Music: {} is performing near him (song {})", Name(bard, "a bard"), g_songIds);
			}
			g_songSeen = now;
			g_music = std::format("{} is playing and singing nearby; the music fills {}'s ears.", Name(bard, "A bard"), Name(a_dog, "the dog"));
		}
	}

	void Start()
	{
		Bridge::WatchSinging([](RE::FormID a_bard) {
			g_lyricBard = a_bard;
			g_lyricAt = Clock::now().time_since_epoch().count();
		});
	}

	void ResetSession()
	{
		g_lyricAt = 0;
		g_lyricBard = 0;
		g_nextScent = {};
		g_songSeen = {};
		g_lastScentLog.clear();
		g_alerted.clear();
		g_lastAlert.reset();
		g_scentNoted.clear();
		g_scentUntil.reset();
		g_scentQuietUntil.reset();
		g_runningQuests.clear();
		g_newPerformance = false;
		std::scoped_lock lock(g_lock);
		g_scent.clear();
		g_music.clear();
		g_warming.clear();
		g_song.reset();
	}

	void Update(RE::Actor* a_dog, const Settings& a_settings)
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!a_dog || !player) {
			return;
		}
		UpdateScent(a_dog, player, a_settings);
		UpdateSong(a_dog);
		std::string warming;
		if (Actions::Downed()) {
			warming = std::format("{} is down, lying hurt on the ground, too badly wounded to get up yet.", Name(a_dog, "The dog"));
		} else if (Actions::Warming()) {
			warming = std::format("{} is pressed close at {}'s side, sharing his body heat: {} is cold.", Name(a_dog, "The dog"),
				Name(player, "the player"), Name(player, "the player"));
		}
		std::scoped_lock lock(g_lock);
		g_warming = std::move(warming);
	}

	std::optional<Song> SongNearby()
	{
		std::scoped_lock lock(g_lock);
		return g_song;
	}

	std::string DecoratorNow(RE::Actor* a_actor)
	{
		if (!Actions::IsDog(a_actor)) {
			return "";
		}
		std::string out = Progression::SizeFact();
		if (!out.empty()) {
			out += "\n";
		}
		if (const auto runes = Runes::Fact(); !runes.empty()) {
			out += runes + "\n";
		}
		if (const auto gifts = Gifts::Fact(); !gifts.empty()) {
			out += gifts + "\n";
		}
		if (const auto fight = Combat::Awareness(); !fight.empty()) {
			out += fight + "\n";
		}
		if (const auto forage = Actions::ForageFact(); !forage.empty()) {
			out += forage + "\n";
		}
		std::scoped_lock lock(g_lock);
		for (const auto* line : { &g_warming, &g_scent, &g_music }) {
			if (!line->empty()) {
				out += *line + "\n";
			}
		}
		return out;
	}
}
