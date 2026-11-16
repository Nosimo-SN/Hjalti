// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#include "actions.h"

#include "CanineDrivesAPI.h"

#include "bridge.h"
#include "combat.h"
#include "dreams.h"
#include "drives.h"
#include "gifts.h"
#include "idles.h"
#include "progression.h"
#include "inputs.h"
#include "mood.h"
#include "soundcues.h"
#include "runes.h"
#include "senses.h"
#include "sleep.h"
#include "notify.h"
#include "training.h"

#include <nlohmann/json.hpp>
#include <map>
#include <set>

#undef AddForm  // winspool.h's AddFormA/W macro shadows BGSListForm::AddForm

namespace Canine::Actions
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr int   kHungryAt = 50;
		constexpr int   kTiredAt = 60;
		constexpr int   kLonelyAt = 50;
		constexpr float kPlayerNear = 800.0f;
		constexpr auto  kAskCooldown = 120s;
		// The gift menu pauses whatever he's saying, so it waits for the line that comes with AskForFood to play out: its
		// length as speaking time plus a margin for the voice to start. With no line at all it opens after kGiftNoLine.
		constexpr auto  kGiftMinDelay = 2500ms;
		constexpr auto  kGiftNoLine = 6s;
		constexpr auto  kGiftMaxWait = 18s;
		constexpr auto  kGiftLineCap = 14s;  // when SkyrimNet doesn't say his line ended: never wait longer than this after it
		constexpr auto  kLineLead = 1500ms;   // from the line being registered to the voice starting
		constexpr auto  kPerWord = 400ms;
		constexpr int   kGiftTries = 30;      // then retry every second while a menu is open (typing to him can pause)
		constexpr double kRestedAt = 15.0;

		constexpr auto  kMaxNap = 10min;          // real time
		constexpr auto  kSettleTime = 10s;        // after lying down, before checking he stayed down (the lie-down takes a while)
		constexpr auto  kUpBeforeRelay = 5s;      // how long he must stay up before he's laid back down
		constexpr int   kMaxRelays = 2;           // times he's laid back down in one nap before it ends
		constexpr float kPlayerMovedOn = 400.0f;  // the player walked this far from where the nap began
		constexpr auto  kSpeechGrace = 20s;       // his own lines this soon after the nap began (or its narration went out) don't wake him
		// isIdleLay turns on as the lie-down starts: wait until he's really down. The sleep styles' lie-downs run
		// up to 3.3 s (the curl), and from a sit the lie-down starts late: at 2 s the pause caught him still standing.
		constexpr auto  kDownBeforePause = 5s;

		constexpr int    kRestlessAt = 40;         // Sniff
		constexpr int    kPlayfulAt = 55;          // Play
		// Play bow: a random idle of his own while restless is high and he's standing about near the player.
		constexpr RE::FormID kDogIdleWarn = 0x037F8F;  // Skyrim.esm DogIdleWarn (aggroWarning2Start); OAR swaps in the bow
		constexpr int    kBowAt = 60;
		constexpr auto   kBowCheck = 20s;          // how often he might
		constexpr int    kBowChance = 25;          // percent per check
		constexpr auto   kBowCooldown = 3min;
		constexpr auto   kBowClip = 4500ms;        // the bow clip is 4.33 s
		constexpr double kBowRelief = 8.0;
		constexpr auto   kBowAgain = 20s;          // as a dialogue action: not twice in a row
		// The happy wiggle (greeting, petting, or a dialogue action): the same warning idle, its clip swapped for code 44.
		constexpr auto   kWiggleClip = 6000ms;     // the wiggle clip is 5.9 s (stage 1, then stronger)
		constexpr auto   kWiggleAgain = 15s;       // not back to back (a greeting and a pet can come together)
		std::atomic<Clock::rep> g_lastWiggleTick{ 0 };  // steady_clock ticks; 0 = never (read by eligibility on workers)
		// The rollover (a dialogue action, sometimes in place of the restless play bow): the same warning idle, code 50.
		constexpr auto   kRollClip = 6700ms;       // the rollover clip is 6.57 s
		constexpr auto   kRollAgain = 90s;         // a roll is a big thing: not often
		constexpr double kRollRelief = 10.0;
		constexpr int    kRollInsteadOfBow = 30;   // percent of the random restless bows that become a roll (if he may)
		std::atomic<Clock::rep> g_lastRollTick{ 0 };  // steady_clock ticks; 0 = never (read by eligibility on workers)
		// The curious head tilt (a dialogue action): the talk-bark idle, standing or sitting, its clip swapped for code 45.
		constexpr auto   kTiltClip = 4400ms;       // the tilt clip is 4.3 s
		constexpr auto   kTiltAgain = 20s;         // a second tilt right after the first looks like a tic
		std::atomic<Clock::rep> g_lastTiltTick{ 0 };  // steady_clock ticks; 0 = never (read by eligibility on workers)
		// The ground sniff (the SniffGround action, and the lead-in to scavenging or a carcass): the standing talk bark, code 46.
		constexpr auto   kGroundSniffClip = 4900ms;  // the sniff clip is 4.8 s
		constexpr auto   kGroundSniffRun = 4500ms;   // a food run starts as his head comes up
		constexpr auto   kGroundSniffAgain = 30s;
		std::atomic<Clock::rep> g_lastGroundSniffTick{ 0 };  // steady_clock ticks; 0 = never (read by eligibility on workers)
		// The air sniff (the SniffAir action, and the lead-in to a hunt): the same carrier under code 47.
		constexpr auto   kAirSniffClip = 4800ms;     // the air sniff clip is 4.75 s
		constexpr auto   kAirSniffRun = 3500ms;      // a hunt starts as his head comes back down
		constexpr auto   kAirSniffAgain = 30s;
		std::atomic<Clock::rep> g_lastAirSniffTick{ 0 };  // steady_clock ticks; 0 = never (read by eligibility on workers)
		// The sneeze (the Sneeze action, and now and then after a ground sniff): the same carrier under code 48.
		constexpr auto   kSneezeClip = 2200ms;       // the sneeze clip is 2.1 s
		constexpr auto   kSneezeAgain = 60s;
		constexpr int    kSneezeAfterSniff = 15;     // percent: a ground sniff (not before food) ends in a sneeze
		std::atomic<Clock::rep> g_lastSneezeTick{ 0 };  // steady_clock ticks; 0 = never (read by eligibility on workers)
		// His voice as dialogue actions: per kind, when he last used it (steady_clock ticks; 0 = never).
		std::array<std::atomic<Clock::rep>, 5> g_lastVoiceTick{};
		constexpr std::array<Clock::duration, 5> kVoiceAgain{ 8s, 8s, 12s, 20s, 20s };  // happy, angry, growl, pout, sad
		constexpr auto   kAngryBarkClip = 1200ms;    // the talk bark clip is 1 s
		bool VoiceCooled(int a_kind)
		{
			const auto last = g_lastVoiceTick[a_kind].load();
			return last == 0 || Clock::now() - Clock::time_point(Clock::duration(last)) >= kVoiceAgain[a_kind];
		}
		// The lying poses as dialogue actions: body language, awake (no nap, AI on, head tracking on). He holds the pose a
		// while, then gets up; his follow AI may stand him up sooner, which ends it too.
		constexpr auto   kPoseMin = 45s;
		constexpr auto   kPoseMax = 90s;
		constexpr auto   kPoseDownWait = 8s;       // the lie-down hasn't registered by then: it didn't take
		constexpr auto   kPoseUpFor = 3s;          // up this long (isIdleLay drops briefly during talk barks) counts as up
		constexpr RE::FormID kDogIdleStop = 0x0960DF;  // Skyrim.esm DogIdleStop
		std::atomic<Clock::rep> g_lastBowTick{ 0 };  // steady_clock ticks; 0 = never (read by eligibility on workers)
		constexpr int    kTooTiredToPlay = 70;
		constexpr auto   kPlayTime = 14s;
		// The movement calls go through Papyrus, which can run seconds behind when the script engine is busy, so Play's
		// clock starts once he's actually running, and both give the call this long to take.
		constexpr auto   kStartWait = 30s;
		constexpr auto   kSniffReach = kStartWait;  // to get to them; if he can't, the sniff ends quietly
		constexpr auto   kSniffTime = 6s;          // nosing about once he's there, then back to following
		constexpr float  kSniffArrived = 220.0f;   // close enough to count as there (the narration waits for it)
		constexpr float  kLapRadiusOutdoors = 480.0f;
		constexpr float  kLapRadiusIndoors = 270.0f;
		// Play can't use the offset (SeverActions' follow package keeps its own offset from the player and overwrites ours;
		// an offset from anyone else, as in Sniff, works) or PathToReference (the follow package outranks it). So it lays a
		// package over the follow one for the length of the play: SeverActions' own run-to-linked-ref travel package, through
		// PapyrusUtil's override stack at a priority above SeverActions' follow (95), with his travel link pointed at an
		// invisible marker that hops a third of the way round the player each time he gets there.
		constexpr float      kLapStep = 1.05f;           // radians: a sixth of the way round, so he runs the rim, not through the middle
		constexpr float      kLead = 1.1f;               // radians the carrot stays ahead of him once he's running
		// The package completes when he arrives (within 150), and a completed package doesn't always restart, so the marker
		// hops on just before he gets there, checked four times a second; if he stalls anyway, his AI is reset to pick it up
		// again. Hopping much earlier turned him back through the middle, straight at the player.
		constexpr auto       kLegCheck = 250ms;
		constexpr float      kWaypointReached = 210.0f;  // just outside the package's arrival radius (150): he reaches the rim first
		constexpr auto       kStallTime = 1500ms;
		constexpr int        kPlayPriority = 100;
		constexpr RE::FormID kXMarker = 0x3B;            // Skyrim.esm XMarker
		constexpr RE::FormID kTravelRun = 0x02B053;      // SeverActions.esp SeverActions_TravelRun
		constexpr RE::FormID kTravelTarget = 0x02B050;   // SeverActions.esp SeverActions_TravelTargetKW (its location)
		// Skyrim.esm DoNothing: a plain Wait (no time limit, movement stopped, no position to keep). During a nap it
		// replaces the follow package as an override, so his AI stays on. (SeverActions' radius-0 HoldPosition wait
		// walked him back to his exact spot once the lie-down had shifted him, standing him up.)
		constexpr RE::FormID kNapHold = 0x0654E2;
		constexpr auto       kSeverPlugin = "SeverActions.esp"sv;
		// Indoors a point on a circle often lands in a wall or a table, and he stands stuck. There he gets zoomies instead:
		// a dash from one person or piece of furniture to the next (things that stand where he can reach), and back past
		// the player once along the way.
		constexpr float       kRouteRange = 900.0f;
		constexpr float       kRouteNearest = 200.0f;    // stops this close to the player are no run at all
		constexpr float       kRouteFloor = 120.0f;      // same floor as the player: no stops upstairs or in the cellar
		constexpr std::size_t kRouteStops = 6;
		constexpr float       kStopReached = 230.0f;     // on to the next just before the package's arrival radius (150)
		constexpr float  kSniffRange = 1200.0f;
		constexpr std::size_t kSniffChoices = 3;    // he picks among the nearest few
		constexpr auto   kNoHeadway = 8s;          // a sniff that hasn't brought him kHeadway closer in this long ends
		constexpr float  kHeadway = 150.0f;
		constexpr double kPlayRelief = 35.0;
		constexpr double kPlayTiring = 5.0;
		constexpr double kSniffRelief = 15.0;

		// Greet: the player comes back to him after being away (he isn't following them).
		constexpr float      kGreetSeeRange = 1500.0f;  // the player comes into view this close
		constexpr float      kAwayRange = 3000.0f;      // farther than this (or him not loaded) counts as away again
		constexpr int        kMissedAt = 65;            // separation this strong: he has been left a while
		constexpr auto       kGreetCooldown = 5min;
		constexpr auto       kAwayFor = 60s;            // real time apart before it counts as being left
		constexpr auto       kGreetTime = 4s;           // fussing over them once he's there
		constexpr int        kGreetBarks = 3;           // when he can't get to them
		constexpr auto       kGreetBarkGap = 1100ms;
		constexpr RE::FormID kFollowerFaction = 0x05C84E;  // Skyrim.esm CurrentFollowerFaction
		constexpr RE::FormID kSeverFollow = 0x16A790;      // SeverActions.esp SeverActions_FollowV2Pool

		// Warm (Frostfall): he presses against a cold player and shares his body heat.
		constexpr auto       kFrostfallPlugin = "Frostfall.esp"sv;
		constexpr auto       kUpdatePlugin = "Update.esm"sv;
		constexpr RE::FormID kFrostfallRunning = 0x06DCFB;       // FrostfallRunning (2 = running), original / Frostfall 2026 .esp
		constexpr RE::FormID kFrostfallRunningEsl = 0x000A9A;    // the same in the ESL-compacted Frostfall 2026 build
		constexpr RE::FormID kFrostfallExposureLevel = 0xCC0E06;  // FrostfallExposureLevelReadOnly (Update.esm): 2+ is cold
		constexpr int        kColdAt = 2;
		constexpr auto       kWarmCooldown = 5min;
		constexpr float      kWarmFirst = 10.0f;    // exposure taken off when he settles against them
		constexpr float      kWarmLater = 3.0f;     // and again every kWarmEvery while he stays pressed close
		constexpr auto       kWarmEvery = 10s;
		constexpr auto       kWarmFor = 60s;
		constexpr float      kWarmClose = 400.0f;

		// Food: snapping up food lying about (Scavenge) or nosing it out of someone's pack (Snatch).
		constexpr float      kFoodRange = 1200.0f;
		constexpr float      kFoodReached = 120.0f;  // right up at the table's edge
		constexpr auto       kFoodSniff = 1200ms;    // nose at it before he takes it
		constexpr auto       kFoodCooldown = 3min;
		constexpr auto       kFoodScanEvery = 3s;

		// Sing: howling along with a bard. Once per song, with a long cooldown; no restlessness needed, so a request
		// ("sing with the bard!") can start it, and the behavior loop offers it when he's restless.
		constexpr double     kSingRelief = 20.0;  // at their side, walking with them (a follower trails a little)

		enum class RoamKind { kPlay, kSniff, kGreet, kWarm, kScavenge, kSnatch, kHunt, kCarcass, kFish };
		struct Roam
		{
			RoamKind          kind;
			RE::FormID        dog = 0;
			RE::FormID        target = 0;  // who he keeps his offset from: the player (Play) or the one he sniffs
			Clock::time_point until;
			RE::NiPoint3      playerPos;
			float             angle = 0.0f;
			float             radius = 0.0f;
			bool              arrived = false;  // Sniff: he reached them; Play: he got going. Either way the event went out
			Clock::time_point legSince;         // Play: when the marker last moved
			std::optional<Clock::time_point> stillSince;  // Play: standing still since then
			std::uint32_t     id = 0;           // Play: which run the leg checker belongs to
			std::vector<RE::ObjectRefHandle> route;  // Play indoors: the stops of his zoomies, in order; empty outdoors
			std::size_t       stop = 0;
			float             closest = 0.0f;       // Sniff: the nearest he has got to them so far, and when
			Clock::time_point closestAt;
			bool              eating = false;       // Carcass: past the sniff, tearing into it
			bool              kill = false;         // Carcass: he ran this one down himself
			bool              pounced = false;      // Hunt: past the stalking, the fight is on
			std::optional<Clock::time_point> castUntil;  // Hunt: no way through, he's casting about for the trail until then
			Clock::time_point castNext;
			bool              castRight = false;
			RE::FormID        produce = 0;          // Fish: a flora fish's produce (Salmon Meat), handed over if his catch gave nothing
		};

		// Play's current waypoint in the world: the offset is in the player's frame (x to the right, y ahead).
		RE::NiPoint3 Waypoint(RE::Actor* a_player, float a_angle, float a_radius)
		{
			const float x = a_radius * std::sin(a_angle);
			const float y = a_radius * std::cos(a_angle);
			const float heading = a_player->GetAngleZ();
			auto        point = a_player->GetPosition();
			point.x += x * std::cos(heading) + y * std::sin(heading);
			point.y += -x * std::sin(heading) + y * std::cos(heading);
			return point;
		}
		// Where he stands now, as an angle round the player in the same frame.
		float AngleAround(RE::Actor* a_player, RE::Actor* a_dog)
		{
			const auto  d = a_dog->GetPosition() - a_player->GetPosition();
			const float heading = a_player->GetAngleZ();
			return std::atan2(d.x * std::cos(heading) - d.y * std::sin(heading), d.x * std::sin(heading) + d.y * std::cos(heading));
		}

		std::optional<Roam>  g_roam;  // game thread
		// Greet: armed while the player is away, fired once when they come back into view.
		bool                 g_greetArmed = false;
		std::optional<Clock::time_point> g_awaySince;  // the player has been away from him since
		std::optional<Clock::time_point> g_lastGreet;
		// Warm: Frostfall's cold, read on the game thread for the eligibility check on SkyrimNet workers.
		std::atomic_bool     g_playerCold{ false };
		std::atomic<Clock::rep> g_lastWarm{ 0 };
		std::atomic<Clock::rep> g_lastAffection{ 0 };
		constexpr auto          kAffectionCooldown = 90s;
		std::optional<Clock::time_point> g_warmUntil;  // he's pressed against them, warming, until then
		Clock::time_point    g_warmNext;
		bool                 g_warmedOnce = false;
		std::atomic<std::uint32_t> g_sangSong{ 0 };  // the song he last howled along with
		// Bleedout: dogs have no bleedout pose, so an essential dog knocked down would just stand there.
		std::atomic_bool        g_downed{ false };
		Clock::time_point       g_nextDownPose{};
		// Food within his reach, found on the game thread for the eligibility checks on SkyrimNet workers.
		std::atomic_bool        g_looseFood{ false };
		std::atomic_bool        g_carriedFood{ false };
		std::atomic<Clock::rep> g_lastFood{ 0 };
		Clock::time_point       g_nextFoodScan{};
		RE::FormID              g_foodItem = 0;  // what he is after on a Snatch, and what he took
		std::atomic<Clock::rep>    g_lastSing{ 0 };
		std::atomic_bool     g_roaming{ false };
		// The roam has him still on purpose (at a carcass, sniffing and eating it): the watchdog shouldn't expect
		// him to move (2026-11-15: it reset his graph mid-meal, 10 s into the sniff and the eat).
		std::atomic_bool     g_roamStill{ false };
		RE::ObjectRefHandle  g_marker;  // Play's waypoint, placed once and reused
		std::uint32_t        g_roamIds = 0;

		RE::BGSListForm* g_foodList = nullptr;
		// Nap state, game thread (g_napping and g_lying are also read by eligibility on SkyrimNet workers).
		std::atomic_bool  g_lying{ false };
		Clock::time_point g_napStarted;
		Clock::time_point g_graceFrom;     // his own lines within kSpeechGrace of this don't wake him
		Clock::time_point g_lastLayDown;
		RE::NiPoint3      g_napPlayerPos;
		int               g_relays = 0;
		std::optional<Clock::time_point> g_upSince;
		std::optional<Clock::time_point> g_downSince;  // lying without a break since then
		RE::FormID        g_napDog = 0;    // the dog napping, for the wake-up note
		std::optional<std::string> g_settledLine;  // the nap's event, waiting until he's settled
		RE::FormID        g_settled = 0;   // the napping dog once he has settled (lain down long enough), or 0
		RE::FormID        g_holding = 0;   // the dog holding the wait package for his nap, or 0
		RE::FormID        g_aiPaused = 0;  // the dog whose AI we switched off once he settled, or 0
		bool              g_layPending = false;  // the nap began; he lies down once the wait package runs
		constexpr auto    kHoldWait = 3s;        // the longest he waits for it (the Papyrus call can run behind)
		Clock::time_point g_asleepSince;   // when he settled; a long enough nap can bring a dream
		std::atomic_bool            g_napping{ false };
		// Pose state, game thread (g_posing is also read by eligibility on SkyrimNet workers).
		struct PoseState
		{
			RE::FormID                       dog = 0;
			Sleep::Style                     style = Sleep::Style::kRelax;
			Clock::time_point                started{};
			Clock::time_point                until{};
			RE::NiPoint3                     playerPos{};
			std::optional<Clock::time_point> downSince;
			std::optional<Clock::time_point> upSince;
		};
		std::optional<PoseState>    g_pose;
		std::atomic_bool            g_posing{ false };
		std::atomic<Clock::rep>     g_lastAsk{ 0 };  // steady_clock ticks; 0 = never
		std::atomic<Clock::rep>     g_dogAudioEnd{ 0 };  // SkyrimNet's "SkyrimNet_AudioEnded" for him: his line finished playing
		std::atomic<Clock::rep>     g_dogLineAt{ 0 };  // his latest line: when it was registered, and how many words
		std::atomic_int             g_dogLineWords{ 0 };

		bool Calm(RE::Actor* a_dog)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			return a_dog && player && !a_dog->IsDead() && !a_dog->IsInCombat() && !player->IsInCombat() &&
			       a_dog->GetPosition().GetDistance(player->GetPosition()) <= kPlayerNear;
		}

		bool AtLeast(RE::Actor* a_dog, Drive a_drive, int a_value)
		{
			const auto value = Drives::Peek(a_dog, a_drive);
			return value && *value >= a_value;
		}

		// His own sounds, played on him directly (an idle's built-in sound is lost whenever the idle doesn't play).
		struct Sound
		{
			RE::FormID       id;
			std::string_view plugin;
		};
		// His own voice (SN_Dog.esp sound records, 2026-11-14), in place of the vanilla husky whine, sniff and bark.
		constexpr Sound kWhine{ 0x000820, "SN_Dog.esp"sv };       // CanineDrivesWhinePoutSD: a whine he means (begging); the idle whines (81F) are quieter
		constexpr Sound kSniffSound{ 0x00081A, "SN_Dog.esp"sv };  // CanineDrivesSniffSD
		constexpr Sound kShake{ 0x00287B, "Dawnguard.esm"sv };  // NPCDogHuskyIdleShake
		constexpr Sound kBarkSound{ 0x00081C, "SN_Dog.esp"sv };  // CanineDrivesBarkSD (happy bark)
		constexpr Sound kHowlSound{ 0x0D8D4B, "Skyrim.esm"sv };     // NPCWolfHowl (dogs have no howl of their own)

		void PlaySound(RE::Actor* a_actor, Sound a_sound)
		{
			const auto data = RE::TESDataHandler::GetSingleton();
			const auto descriptor = data ? data->LookupForm<RE::BGSSoundDescriptorForm>(a_sound.id, a_sound.plugin) : nullptr;
			const auto audio = RE::BSAudioManager::GetSingleton();
			const auto node = a_actor ? a_actor->Get3D() : nullptr;
			if (!descriptor || !audio || !node) {
				logs::info("Sound {:06X}:{} not played: {}", a_sound.id, a_sound.plugin,
					!descriptor ? "no such sound" : !audio ? "no audio manager" : "he isn't loaded");
				return;
			}
			RE::BSSoundHandle handle;
			const bool built = audio->GetSoundHandle(handle, descriptor) && handle.IsValid();
			if (built) {
				handle.SetPosition(a_actor->GetPosition());
				handle.SetObjectToFollow(node);
			}
			if (built && handle.Play()) {
				logs::info("Sound {:06X}:{} played", a_sound.id, a_sound.plugin);
				return;
			}
			// The native path failed: Papyrus Sound.Play(him), the way scripts play sounds.
			const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			RE::TESObjectREFR* source = a_actor;
			std::unique_ptr<RE::BSScript::IFunctionArguments> args{ RE::MakeFunctionArguments(std::move(source)) };
			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
			const bool sent = vm &&
			                  vm->DispatchMethodCall(vm->handlePolicy->GetHandleForObject(RE::BGSSoundDescriptorForm::FORMTYPE, descriptor),
								  "Sound", "Play", args.get(), none);
			logs::info("Sound {:06X}:{}: native play failed ({}), {}", a_sound.id, a_sound.plugin, built ? "Play refused" : "no handle",
				sent ? "sent through Papyrus Sound.Play" : "Papyrus Sound.Play failed too");
		}

		void SoundLater(RE::FormID a_actor, std::chrono::milliseconds a_delay, Sound a_sound)
		{
			std::thread([a_actor, a_delay, a_sound] {
				std::this_thread::sleep_for(a_delay);
				SKSE::GetTaskInterface()->AddTask([a_actor, a_sound] { PlaySound(RE::TESForm::LookupByID<RE::Actor>(a_actor), a_sound); });
			}).detach();
		}


		// Game thread. True once the menu opened, or when it should not open any more.
		bool TryOpenGiftMenu(RE::FormID a_dog)
		{
			const auto dog = RE::TESForm::LookupByID<RE::Actor>(a_dog);
			if (!dog || !Calm(dog)) {
				const auto player = RE::PlayerCharacter::GetSingleton();
				logs::info("AskForFood: no gift menu ({})", !dog ? "he's gone" : dog->IsInCombat() || (player && player->IsInCombat()) ? "a fight" :
				                                                                      std::format("{:.0f} units from you", player ? dog->GetPosition().GetDistance(player->GetPosition()) : 0.0f));
				return true;  // he wandered off or a fight started: drop it
			}
			if (Inputs::FoodComing()) {
				logs::info("AskForFood: food is already on its way, no gift menu");
				return true;
			}
			const auto ui = RE::UI::GetSingleton();
			if (!ui || ui->GameIsPaused() || ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME) || ui->IsMenuOpen(RE::GiftMenu::MENU_NAME) ||
				ui->IsMenuOpen(RE::BarterMenu::MENU_NAME) || ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME)) {
				return false;  // try again in a moment
			}
			RE::BSTSmartPointer<RE::IGiftMenuScriptCallback> callback;
			RE::GiftMenu::OpenMenuAsGifter(dog, &callback, g_foodList, true);
			logs::info("AskForFood: gift menu opened");
			return true;
		}

		// The vanilla Papyrus natives Actor.KeepOffsetFromActor / ClearKeepOffsetFromActor (CommonLib has no wrapper).
		void CallOnActor(RE::Actor* a_actor, std::string_view a_function, RE::BSScript::IFunctionArguments* a_args,
			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> a_result = {})
		{
			const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			if (!vm || !a_actor) {
				return;
			}
			const auto handle = vm->handlePolicy->GetHandleForObject(RE::Actor::FORMTYPE, a_actor);
			if (!vm->DispatchMethodCall(handle, "Actor", a_function, a_args, a_result)) {
				logs::warn("Actor.{} could not be called", a_function);
			}
		}

		// Older builds moved Sniff with KeepOffsetFromActor; a save from then may still carry the offset.
		void ClearOffset(RE::Actor* a_dog)
		{
			std::unique_ptr<RE::BSScript::IFunctionArguments> args{ RE::MakeFunctionArguments() };
			CallOnActor(a_dog, "ClearKeepOffsetFromActor", args.get());
		}

		std::mt19937& Random()
		{
			static std::mt19937 engine{ std::random_device{}() };
			return engine;
		}

		// Play's marker, in the player's cell; placed the first time.
		RE::TESObjectREFR* Marker(RE::Actor* a_player)
		{
			auto marker = g_marker.get();
			if (!marker || marker->IsDeleted()) {
				const auto base = RE::TESForm::LookupByID<RE::TESBoundObject>(kXMarker);
				marker = base ? a_player->PlaceObjectAtMe(base, false) : nullptr;
				g_marker = marker ? marker->CreateRefHandle() : RE::ObjectRefHandle{};
			} else if (marker->GetParentCell() != a_player->GetParentCell()) {
				marker->MoveTo(a_player);
			}
			return marker.get();
		}

		RE::TESPackage* RunPackage()
		{
			const auto data = RE::TESDataHandler::GetSingleton();
			return data ? data->LookupForm<RE::TESPackage>(kTravelRun, kSeverPlugin) : nullptr;
		}

		RE::TESPackage* WaitPackage()
		{
			const auto data = RE::TESDataHandler::GetSingleton();
			return data ? data->LookupForm<RE::TESPackage>(kNapHold, "Skyrim.esm"sv) : nullptr;
		}

		RE::BGSKeyword* TravelTarget()
		{
			const auto data = RE::TESDataHandler::GetSingleton();
			return data ? data->LookupForm<RE::BGSKeyword>(kTravelTarget, kSeverPlugin) : nullptr;
		}

		// Re-evaluates his AI once a queued Papyrus call has run, so the override change takes at once.
		class Reevaluate : public RE::BSScript::IStackCallbackFunctor
		{
		public:
			explicit Reevaluate(RE::FormID a_actor) :
				_actor(a_actor)
			{}

			void operator()(RE::BSScript::Variable) override
			{
				SKSE::GetTaskInterface()->AddTask([actor = _actor] {
					if (const auto dog = RE::TESForm::LookupByID<RE::Actor>(actor)) {
						dog->EvaluatePackage(true);
					}
				});
			}

			void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

		private:
			RE::FormID _actor;
		};

		// PapyrusUtil's ActorUtil.AddPackageOverride / RemovePackageOverride (SkyrimNet itself depends on PapyrusUtil).
		// a_package: the play package when not given.
		void PackageOverride(RE::Actor* a_dog, bool a_add, RE::TESPackage* a_package = nullptr)
		{
			const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			RE::TESPackage* package = a_package ? a_package : RunPackage();
			if (!vm || !a_dog || !package) {
				return;
			}
			RE::Actor* dog = a_dog;
			std::unique_ptr<RE::BSScript::IFunctionArguments> args{ a_add ?
				RE::MakeFunctionArguments(std::move(dog), std::move(package), static_cast<std::int32_t>(kPlayPriority), static_cast<std::int32_t>(0)) :
				RE::MakeFunctionArguments(std::move(dog), std::move(package)) };
			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result{ new Reevaluate(a_dog->GetFormID()) };
			const auto function = a_add ? "AddPackageOverride"sv : "RemovePackageOverride"sv;
			if (!vm->DispatchStaticCall("ActorUtil", function, args.get(), result)) {
				logs::warn("ActorUtil.{} could not be called (is PapyrusUtil installed?)", function);
			}
		}

		// Unlinks a condition item we put on an idle, wherever it sits now (another of ours may have gone in above it).
		// The item stays allocated: the engine may still be reading the old chain this frame.
		void Unlink(RE::TESIdleForm* a_idle, RE::TESConditionItem* a_item)
		{
			if (!a_idle || !a_item) {
				return;
			}
			for (auto link = &a_idle->conditions.head; *link; link = &(*link)->next) {
				if (*link == a_item) {
					*link = a_item->next;
					return;
				}
			}
		}

		// A first condition "is not this dog" (GetIsID != base) leaves him out of an idle; in memory only.
		RE::TESConditionItem* LeaveOut(RE::TESIdleForm* a_idle, RE::TESNPC* a_base)
		{
			const auto item = new RE::TESConditionItem();
			item->data.functionData.function = RE::FUNCTION_DATA::FunctionID::kGetIsID;
			item->data.functionData.params[0] = a_base;
			item->data.flags.opCode = RE::CONDITION_ITEM_DATA::OpCode::kNotEqualTo;
			item->data.comparisonValue.f = 1.0f;
			item->data.object = RE::CONDITIONITEMOBJECT::kSelf;
			item->next = a_idle->conditions.head;
			a_idle->conditions.head = item;
			return item;
		}

		// While he naps his AI stays on, and it picks idles for him on its own: a bark (his idle "dialogue" plays the
		// talking bark), sit or lie, and the idle stop that stands him up. Asleep, he's left out of all of them; our own
		// lie-down (IdleDogLay, played directly) still works. Off again before the nap ends so the follow package can
		// stand him up.
		void SleepIdles(RE::Actor* a_dog, bool a_on)
		{
			constexpr std::array<RE::FormID, 5> kIdles{ {
				0x0005A084,  // Skyrim.esm DogIdles (the AI's random sit / lie / bark)
				0x00081171,  // DogBarkRunIdle
				0x000BEE73,  // DogBarkTalk (the bark played while he "speaks")
				0x000960DF,  // DogIdleStop (gets up out of an idle)
				0x000960E2,  // DogSitIdleStop
			} };
			static std::array<RE::TESConditionItem*, kIdles.size()> added{};
			const auto base = a_dog ? a_dog->GetActorBase() : nullptr;
			if (!(a_on && base) && std::ranges::none_of(added, [](auto a) { return a != nullptr; })) {
				return;  // nothing held
			}
			for (std::size_t i = 0; i < kIdles.size(); ++i) {
				const auto idle = RE::TESForm::LookupByID<RE::TESIdleForm>(kIdles[i]);
				if (!idle) {
					continue;
				}
				if (added[i]) {
					Unlink(idle, added[i]);
					added[i] = nullptr;
				}
				if (a_on && base) {
					added[i] = LeaveOut(idle, base);
				}
			}
			logs::info("Nap: his own idles {}", a_on && base ? "held (no barks, no getting up)" : "back");
		}

		// Game thread: hops the marker a third of the way round; his travel package re-aims at it. a_reset: he stalled
		// (the package completed), so his AI is reset to start it afresh.
		void NextLeg(RE::Actor* a_dog, RE::Actor* a_player, bool a_reset = false)
		{
			const auto marker = Marker(a_player);
			if (!marker) {
				return;
			}
			g_roam->angle += kLapStep;
			g_roam->legSince = Clock::now();
			g_roam->stillSince.reset();
			marker->SetPosition(Waypoint(a_player, g_roam->angle, g_roam->radius));
			a_dog->EvaluatePackage(true, a_reset);
			if (a_reset) {
				logs::info("Play: he stalled, AI reset");
			}
		}

		// Indoors: the people and furniture he can dash between, on the player's floor, ordered round the player so the
		// zoomies loop rather than zigzag, with the player as one stop. Fewer than two: nowhere to run.
		std::vector<RE::ObjectRefHandle> IndoorRoute(RE::Actor* a_dog, RE::Actor* a_player)
		{
			std::vector<RE::TESObjectREFR*> found;
			const auto cell = a_player->GetParentCell();
			const auto center = a_player->GetPosition();
			if (cell) {
				cell->ForEachReferenceInRange(center, kRouteRange, [&](RE::TESObjectREFR* a_ref) {
					const auto base = a_ref ? a_ref->GetBaseObject() : nullptr;
					if (!base || a_ref == a_dog || a_ref->IsPlayerRef() || a_ref->IsDisabled() || a_ref->IsDeleted() || !a_ref->Is3DLoaded()) {
						return RE::BSContainer::ForEachResult::kContinue;
					}
					const auto pos = a_ref->GetPosition();
					if (std::abs(pos.z - center.z) > kRouteFloor || pos.GetDistance(center) < kRouteNearest) {
						return RE::BSContainer::ForEachResult::kContinue;
					}
					const auto actor = a_ref->As<RE::Actor>();
					bool       unused = false;
					if ((actor ? (!actor->IsDead() && !actor->IsHostileToActor(a_dog)) : base->Is(RE::FormType::Furniture)) &&
						a_player->HasLineOfSight(a_ref, unused)) {  // in the same room: nothing behind a wall or a closed door
						found.push_back(a_ref);
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			}
			std::ranges::shuffle(found, Random());
			if (found.size() > kRouteStops) {
				found.resize(kRouteStops);
			}
			const auto around = [&](RE::TESObjectREFR* a_ref) {
				const auto d = a_ref->GetPosition() - center;
				return std::atan2(d.x, d.y);
			};
			std::ranges::sort(found, [&](RE::TESObjectREFR* a, RE::TESObjectREFR* b) { return around(a) < around(b); });

			std::vector<RE::ObjectRefHandle> route;
			for (std::size_t i = 0; i < found.size(); ++i) {
				route.push_back(found[i]->CreateRefHandle());
				if (i + 1 == found.size() / 2 + 1 && found.size() >= 2) {
					route.push_back(a_player->CreateRefHandle());  // a pass back by the player halfway round
				}
			}
			return found.size() >= 2 ? route : std::vector<RE::ObjectRefHandle>{};
		}

		// Game thread: indoors, on to the next stop of his zoomies.
		void NextStop(RE::Actor* a_dog, bool a_reset = false)
		{
			const auto keyword = TravelTarget();
			if (!keyword || g_roam->route.empty()) {
				return;
			}
			g_roam->stop = (g_roam->stop + 1) % g_roam->route.size();
			g_roam->legSince = Clock::now();
			g_roam->stillSince.reset();
			if (const auto next = g_roam->route[g_roam->stop].get()) {
				a_dog->extraList.SetLinkedRef(next.get(), keyword);
			}
			a_dog->EvaluatePackage(true, a_reset);
			if (a_reset) {
				logs::info("Play: he stalled, AI reset");
			}
		}

		// Game thread, four times a second while he plays: keeps the marker ahead of him (outdoors) or moves him on to the
		// next stop (indoors).
		void CheckLeg(std::uint32_t a_id)
		{
			if (!g_roam || g_roam->kind != RoamKind::kPlay || g_roam->id != a_id) {
				return;
			}
			const auto dog = RE::TESForm::LookupByID<RE::Actor>(g_roam->dog);
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto marker = g_marker.get();
			if (!dog || !player || !dog->Is3DLoaded()) {
				return;
			}
			const auto now = Clock::now();
			if (dog->IsMoving()) {
				g_roam->stillSince.reset();
			} else if (!g_roam->stillSince) {
				g_roam->stillSince = now;
			}
			if (!g_roam->route.empty()) {
				const auto stop = g_roam->route[g_roam->stop].get();
				if (!stop || dog->GetPosition().GetDistance(stop->GetPosition()) <= kStopReached) {
					NextStop(dog);
				} else if (g_roam->arrived && g_roam->stillSince && now - *g_roam->stillSince >= kStallTime && now - g_roam->legSince >= kStallTime) {
					NextStop(dog, true);  // can't get there: skip it
				}
				return;
			}
			if (!marker) {
				return;
			}
			if (g_roam->arrived) {
				// The carrot: once he runs, the marker glides round the circle a fixed angle ahead of him, so he curves along
				// the rim without ever arriving (no package restart, no brake and turn at each corner).
				g_roam->angle = AngleAround(player, dog) + kLead;
				marker->SetPosition(Waypoint(player, g_roam->angle, g_roam->radius));
			} else if (dog->GetPosition().GetDistance(marker->GetPosition()) <= kWaypointReached) {
				NextLeg(dog, player);
			}
			if (g_roam->arrived && g_roam->stillSince && now - *g_roam->stillSince >= kStallTime && now - g_roam->legSince >= kStallTime) {
				// (Only once he got going: before that the override may still be queued in Papyrus.)
				NextLeg(dog, player, true);
			}
		}

		void StartLegChecker(std::uint32_t a_id)
		{
			std::thread([a_id] {
				while (g_roaming) {
					std::this_thread::sleep_for(kLegCheck);
					SKSE::GetTaskInterface()->AddTask([a_id] { CheckLeg(a_id); });
				}
			}).detach();
		}

		// Game thread: lifts the play package and his travel link, so SeverActions' follow takes him back.
		void ReleasePlay(RE::Actor* a_dog)
		{
			if (!a_dog) {
				return;
			}
			PackageOverride(a_dog, false);
			// Clear the link only if it is still one of ours (the marker, a stop of his zoomies, the one he went to): if
			// something else pointed it elsewhere meanwhile, that is theirs to keep.
			const auto keyword = TravelTarget();
			const auto linked = keyword ? a_dog->extraList.GetLinkedRef(keyword) : nullptr;
			if (!linked) {
				return;
			}
			bool ours = linked == g_marker.get().get();
			if (g_roam) {
				ours = ours || linked->GetFormID() == g_roam->target;
				for (const auto& stop : g_roam->route) {
					ours = ours || linked == stop.get().get();
				}
			}
			if (ours) {
				a_dog->extraList.SetLinkedRef(nullptr, keyword);
			} else {
				logs::info("Travel link points at {:08X}, not set by us: left alone", linked->GetFormID());
			}
		}

		void Tell(RE::Actor* a_originator, RE::Actor* a_target, std::string a_line)
		{
			const auto s = SnapshotSettings();
			if (s.events && a_originator) {
				Bridge::PostEvent(std::move(a_line), a_originator->GetFormID(), a_target ? a_target->GetFormID() : 0, s.narrate);
			}
		}

		// A play that never got going: he wanted to run and had nowhere to.
		void TellNoRoom(RE::Actor* a_dog)
		{
			PlaySound(a_dog, kWhine);
			Tell(a_dog, RE::PlayerCharacter::GetSingleton(),
				std::format("{} gives a restless whine and paws at the ground, itching to run but with no room to go.", a_dog->GetDisplayFullName()));
		}

		// Only when his success line never went out, and only for his own failures (not a fight, the player leaving,
		// a nap or him unloading).
		void TellFailure(RE::Actor* a_dog, RE::Actor* a_target, std::string_view a_why)
		{
			if (!a_dog || !a_dog->Is3DLoaded() || g_roam->arrived) {
				return;
			}
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (g_roam->kind == RoamKind::kPlay) {
				if (a_why == "never got going") {
					TellNoRoom(a_dog);
				}
			} else if (g_roam->kind == RoamKind::kGreet) {
				if (a_why == "can't get to them" || a_why == "couldn't reach them") {
					// Too happy to stay quiet: a few barks from where he is.
					Bark(a_dog, false);
					for (int i = 1; i < kGreetBarks; ++i) {
						std::thread([dog = a_dog->GetFormID(), wait = kGreetBarkGap * i] {
							std::this_thread::sleep_for(wait);
							SKSE::GetTaskInterface()->AddTask([dog] {
								if (const auto actor = RE::TESForm::LookupByID<RE::Actor>(dog); actor && actor->Is3DLoaded()) {
									Bark(actor, false);
								}
							});
						}).detach();
					}
					Tell(a_dog, player, std::format("{} spots {} and barks happily from where he stands, tail going, unable to get to them.",
						a_dog->GetDisplayFullName(), player->GetDisplayFullName()));
				}
			} else if (a_target && (a_why == "can't get to them" || a_why == "couldn't reach them")) {
				PlaySound(a_dog, kWhine);
				Tell(a_dog, player, std::format("{} whines in frustration, unable to get to {}.", a_dog->GetDisplayFullName(), a_target->GetDisplayFullName()));
			} else if (a_target && a_why == "they walked off") {
				Tell(a_dog, player, std::format("{} stops and watches {} walk off.", a_dog->GetDisplayFullName(), a_target->GetDisplayFullName()));
			}
		}

		void EndRoam(const char* a_why)
		{
			if (!g_roam) {
				return;
			}
			const auto dog = RE::TESForm::LookupByID<RE::Actor>(g_roam->dog);
			TellFailure(dog, RE::TESForm::LookupByID<RE::Actor>(g_roam->target), a_why);
			if (g_roam->kind == RoamKind::kHunt) {
				Combat::EndHunt(dog, a_why);
			}
			if (g_roam->kind == RoamKind::kPlay) {
				if (const auto marker = g_marker.get(); marker && dog) {
					marker->SetPosition(dog->GetPosition());  // a run still under way ends where he stands
				}
				ReleasePlay(dog);
			} else {
				ReleasePlay(dog);  // Sniff runs on the same package, aimed at the one he sniffs
			}
			if (g_roam->kind == RoamKind::kPlay && dog && dog->Is3DLoaded() && a_why == std::string_view("done")) {
				Bark(dog, false);  // one last happy bark
			}
			constexpr std::array<const char*, 9> kNames{ "Play", "Sniff", "Greet", "Warm", "Scavenge", "Snatch", "Hunt", "Carcass", "Fish" };
			logs::info("{} over ({})", kNames[static_cast<std::size_t>(g_roam->kind)], a_why);
			g_roam.reset();
			g_roaming = false;
			g_roamStill = false;
		}

		bool Play(RE::Actor* a_dog)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!a_dog || !player || g_napping || g_roam) {
				return false;
			}
			const auto cell = a_dog->GetParentCell();
			const bool indoors = cell && cell->IsInteriorCell();
			const auto keyword = TravelTarget();
			if (!RunPackage() || !keyword) {
				logs::warn("Play: SeverActions' travel package isn't loaded");
				return false;
			}
			const auto marker = Marker(player);
			if (!marker) {
				logs::warn("Play: could not place the waypoint marker");
				return false;
			}
			std::vector<RE::ObjectRefHandle> route;
			if (indoors) {
				route = IndoorRoute(a_dog, player);
				if (route.empty()) {
					logs::info("Play: nowhere indoors to run (nobody and no furniture in sight on this floor)");
					TellNoRoom(a_dog);
					return false;
				}
			}
			// The first leg starts from where he stands, so he sets off at once rather than to a point beside him.
			g_roam = Roam{ RoamKind::kPlay, a_dog->GetFormID(), player->GetFormID(), Clock::now() + kStartWait, player->GetPosition(),
				AngleAround(player, a_dog), kLapRadiusOutdoors };
			g_roam->id = ++g_roamIds;
			g_roam->route = std::move(route);
			g_roaming = true;
			g_roamStill = false;
			if (g_roam->route.empty()) {
				a_dog->extraList.SetLinkedRef(marker, keyword);
				NextLeg(a_dog, player);
			} else {
				g_roam->stop = g_roam->route.size() - 1;  // NextStop moves on to the first
				NextStop(a_dog);
				std::string stops;
				for (const auto& handle : g_roam->route) {
					if (const auto ref = handle.get()) {
						stops += std::format("{}{}", stops.empty() ? "" : ", ", ref->GetDisplayFullName());
					}
				}
				logs::info("Play indoors, zoomies past: {}", stops);
			}
			PackageOverride(a_dog, true);
			StartLegChecker(g_roam->id);
			const auto s = SnapshotSettings();
			Drives::Adjust(Drive::kRestless, -kPlayRelief, s);
			Drives::Adjust(Drive::kTired, kPlayTiring, s);
			Training::Did("play");
			logs::info("Action Play (restless -> {:.1f})", Drives::Value(Drive::kRestless));
			return true;
		}

		// Someone loaded, calm and close enough to trot over to: a person or an animal, never the player or a foe.
		RE::Actor* PickSniffTarget(RE::Actor* a_dog)
		{
			const auto lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return nullptr;
			}
			std::vector<RE::Actor*> found;
			for (const auto& handle : lists->highActorHandles) {
				const auto actor = handle.get().get();
				if (!actor || actor == a_dog || actor->IsPlayerRef() || actor->IsDead() || !actor->Is3DLoaded() || actor->IsInCombat() ||
					actor->IsHostileToActor(a_dog) || actor->GetParentCell() != a_dog->GetParentCell() && actor->GetWorldspace() != a_dog->GetWorldspace() ||
					actor->GetPosition().GetDistance(a_dog->GetPosition()) > kSniffRange ||
					std::abs(actor->GetPosition().z - a_dog->GetPosition().z) > kRouteFloor) {  // not upstairs or below
					continue;
				}
				bool unused = false;
				if (!a_dog->HasLineOfSight(actor, unused)) {
					continue;  // a dog sniffs what he can see and smell: nobody behind a wall or a closed door
				}
				found.push_back(actor);
			}
			// Someone standing about rather than walking past: he'd only be chasing them across the room. Among those,
			// one of the nearest few.
			std::vector<RE::Actor*> still;
			std::ranges::copy_if(found, std::back_inserter(still), [](RE::Actor* a) { return !a->IsMoving(); });
			auto& pool = still.empty() ? found : still;
			if (pool.empty()) {
				return nullptr;
			}
			const auto dogPos = a_dog->GetPosition();
			std::ranges::sort(pool, {}, [&](RE::Actor* a) { return a->GetPosition().GetDistance(dogPos); });
			const auto nearest = (std::min)(pool.size(), kSniffChoices);
			return pool[std::uniform_int_distribution<std::size_t>(0, nearest - 1)(Random())];
		}

		bool Sniff(RE::Actor* a_dog)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!a_dog || !player || g_napping || g_roam) {
				return false;
			}
			const auto target = PickSniffTarget(a_dog);
			if (!target) {
				logs::info("Sniff: nobody close enough to sniff");
				return false;
			}
			// The same run package as Play, aimed at them: it stops him within 150 of them, nose to them. (The keep-offset mode
			// this used before lost to SeverActions' follow package more often than not.)
			const auto keyword = TravelTarget();
			if (!RunPackage() || !keyword) {
				logs::warn("Sniff: SeverActions' travel package isn't loaded");
				return false;
			}
			a_dog->extraList.SetLinkedRef(target, keyword);
			PackageOverride(a_dog, true);
			g_roam = Roam{ RoamKind::kSniff, a_dog->GetFormID(), target->GetFormID(), Clock::now() + kSniffReach, player->GetPosition() };
			g_roam->closest = a_dog->GetPosition().GetDistance(target->GetPosition());
			g_roam->closestAt = Clock::now();
			g_roaming = true;
			g_roamStill = false;
			Training::Did("sniff");
			Drives::Adjust(Drive::kRestless, -kSniffRelief, SnapshotSettings());
			logs::info("Action Sniff ({}; restless -> {:.1f})", target->GetDisplayFullName(), Drives::Value(Drive::kRestless));
			return true;
		}

		// He runs to the player on the run package (as Sniff does to anyone else).
		bool RunToPlayer(RE::Actor* a_dog, RoamKind a_kind)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto keyword = TravelTarget();
			if (!a_dog || !player || g_napping || g_roam || !RunPackage() || !keyword) {
				return false;
			}
			a_dog->extraList.SetLinkedRef(player, keyword);
			PackageOverride(a_dog, true);
			g_roam = Roam{ a_kind, a_dog->GetFormID(), player->GetFormID(), Clock::now() + kSniffReach, player->GetPosition() };
			g_roam->closest = a_dog->GetPosition().GetDistance(player->GetPosition());
			g_roam->closestAt = Clock::now();
			g_roaming = true;
			g_roamStill = false;
			return true;
		}

		// Whether he goes with the player now: a follower in any of the usual ways.
		bool Following(RE::Actor* a_dog)
		{
			if (a_dog->IsPlayerTeammate()) {
				return true;
			}
			if (const auto faction = RE::TESForm::LookupByID<RE::TESFaction>(kFollowerFaction); faction && a_dog->GetFactionRank(faction, false) >= 0) {
				return true;
			}
			const auto data = RE::TESDataHandler::GetSingleton();
			const auto follow = data ? data->LookupForm<RE::TESPackage>(kSeverFollow, kSeverPlugin) : nullptr;
			return follow && a_dog->GetCurrentPackage() == follow;
		}

		// Game thread, every tick. Armed while the player is away from him; when they come back into view and he has
		// missed them, he runs over to greet them.
		void UpdateGreet(RE::Actor* a_dog, const Settings& a_settings)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!a_dog || !player || a_dog->IsDead() || !a_settings.greetEnabled) {
				return;
			}
			const bool  loaded = a_dog->Is3DLoaded();
			const bool  sameSpace = loaded && (a_dog->GetParentCell() == player->GetParentCell() ||
                                              a_dog->GetWorldspace() && a_dog->GetWorldspace() == player->GetWorldspace());
			const float distance = sameSpace ? a_dog->GetPosition().GetDistance(player->GetPosition()) : (std::numeric_limits<float>::max)();
			// Away means really left behind: not following, and apart for a while. A follower is briefly unloaded at every
			// door the player goes through, which doesn't count.
			if (distance > kAwayRange && !Following(a_dog)) {
				if (!g_awaySince) {
					g_awaySince = Clock::now();
				} else if (!g_greetArmed && Clock::now() - *g_awaySince >= kAwayFor) {
					logs::info("Greet: player away, armed");
					g_greetArmed = true;
				}
				return;
			}
			g_awaySince.reset();
			if (!g_greetArmed || distance > kGreetSeeRange || Following(a_dog)) {
				return;
			}
			if (a_dog->IsInCombat() || player->IsInCombat() || g_napping || g_roam) {
				return;
			}
			bool unused = false;
			if (!a_dog->HasLineOfSight(player, unused)) {
				return;  // not until he sees them
			}
			g_greetArmed = false;
			const auto separation = Drives::Value(Drive::kSeparation);
			if (separation < kMissedAt || g_lastGreet && Clock::now() - *g_lastGreet < kGreetCooldown) {
				logs::info("Greet: player back, but not missed enough or greeted too recently (separation {:.0f})", separation);
				return;
			}
			if (RunToPlayer(a_dog, RoamKind::kGreet)) {
				g_lastGreet = Clock::now();
				logs::info("Greet: player back after a while (separation {:.0f}), running over", separation);
			}
		}

		// Frostfall's player exposure level, or -1 when Frostfall isn't running.
		int ExposureLevel()
		{
			const auto data = RE::TESDataHandler::GetSingleton();
			// Any Frostfall edition: the EditorID (globals keep theirs in the game), then either build's FormID.
			static RE::TESGlobal* running = nullptr;
			if (!running) {
				running = RE::TESForm::LookupByEditorID<RE::TESGlobal>("FrostfallRunning");
				if (!running && data) {
					running = data->LookupForm<RE::TESGlobal>(kFrostfallRunning, kFrostfallPlugin);
				}
				if (!running && data) {
					running = data->LookupForm<RE::TESGlobal>(kFrostfallRunningEsl, kFrostfallPlugin);
				}
			}
			const auto level = data ? data->LookupForm<RE::TESGlobal>(kFrostfallExposureLevel, kUpdatePlugin) : nullptr;
			if (!running || !level || running->value < 1.5f) {
				return -1;
			}
			return static_cast<int>(std::lround(level->value));
		}

		void WarmPlayer(float a_amount)
		{
			const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			std::unique_ptr<RE::BSScript::IFunctionArguments> args{ RE::MakeFunctionArguments(-a_amount, -1.0f) };
			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
			if (!vm || !vm->DispatchStaticCall("FrostUtil", "ModPlayerExposure", args.get(), none)) {
				logs::warn("Warm: FrostUtil.ModPlayerExposure could not be called");
			}
		}

		// Game thread, every tick: the cold check for eligibility, and the warmth while he lies against them.
		void UpdateWarm(RE::Actor* a_dog)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			g_playerCold = player && !player->IsInCombat() && ExposureLevel() >= kColdAt;
			if (!g_warmUntil) {
				return;
			}
			const auto now = Clock::now();
			const bool close = a_dog && player && a_dog->Is3DLoaded() && a_dog->GetPosition().GetDistance(player->GetPosition()) <= kWarmClose;
			if (now >= *g_warmUntil || !close || ExposureLevel() < 0) {
				logs::info("Warm: over ({})", now >= *g_warmUntil ? "done" : !close ? "apart" : "Frostfall stopped");
				g_warmUntil.reset();
				return;
			}
			if (now >= g_warmNext) {
				WarmPlayer(g_warmedOnce ? kWarmLater : kWarmFirst);
				logs::info("Warm: exposure -{:.0f}", g_warmedOnce ? kWarmLater : kWarmFirst);
				g_warmedOnce = true;
				g_warmNext = now + kWarmEvery;
			}
		}

		bool Warm(RE::Actor* a_dog)
		{
			if (!g_playerCold || !RunToPlayer(a_dog, RoamKind::kWarm)) {
				return false;
			}
			g_lastWarm = Clock::now().time_since_epoch().count();
			Training::Did("warm");
			logs::info("Action Warm (exposure level {})", ExposureLevel());
			return true;
		}

		bool SingReady(RE::Actor* a_dog)
		{
			const auto song = Senses::SongNearby();
			const auto last = g_lastSing.load();
			const auto cooldown = std::chrono::duration<double, std::ratio<60>>(SnapshotSettings().singCooldown);
			const bool cooled = last == 0 || Clock::now() - Clock::time_point(Clock::duration(last)) >= cooldown;
			return a_dog && song && song->id != g_sangSong && cooled;
		}

		bool Sing(RE::Actor* a_dog)
		{
			const auto song = Senses::SongNearby();
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!a_dog || !player || !song || g_napping || g_roam) {
				return false;
			}
			g_sangSong = song->id;
			g_lastSing = Clock::now().time_since_epoch().count();
			Idles::Play(a_dog, Idles::kDogSit, "idleSitStart");
			SoundLater(a_dog->GetFormID(), 900ms, kHowlSound);
			SoundLater(a_dog->GetFormID(), 4500ms, kHowlSound);
			const auto bard = RE::TESForm::LookupByID<RE::Actor>(song->bard);
			const char* bardName = bard ? bard->GetDisplayFullName() : nullptr;
			Tell(a_dog, bard ? bard : player,
				std::format("{} sits back on his haunches, points his nose skyward and howls along with {}'s song, loud and wildly off-key.",
					a_dog->GetDisplayFullName(), bardName && *bardName ? bardName : "the bard"));
			const auto s = SnapshotSettings();
			Drives::Adjust(Drive::kRestless, -kSingRelief, s);
			Training::Did("sing");
			Runes::Light(Runes::Flare::kSong);
			logs::info("Action Sing (along with {}; restless -> {:.1f})", bardName ? bardName : "?", Drives::Value(Drive::kRestless));
			return true;
		}

		// Something to eat: food, not a drink (wine, mead and ale count as "food" too, but are drunk with the potion sound).
		bool IsFood(RE::TESForm* a_form)
		{
			constexpr RE::FormID kPotionUse = 0x0B6435;  // Skyrim.esm ITMPotionUse
			const auto item = a_form ? a_form->As<RE::AlchemyItem>() : nullptr;
			const auto sound = item ? item->data.consumptionSound : nullptr;
			return item && item->IsFood() && !item->IsPoison() && !(sound && sound->GetFormID() == kPotionUse);
		}

		// "Farkas's", but "the Companions'".
		std::string Possessive(std::string a_name)
		{
			if (a_name.empty()) {
				return a_name;
			}
			return a_name.back() == 's' ? a_name + "'" : a_name + "'s";
		}

		// Whose it is, for the narration: a person, the player, a faction, or "" when nobody's.
		std::string OwnerName(RE::TESObjectREFR* a_ref)
		{
			const auto owner = a_ref ? a_ref->GetOwner() : nullptr;
			if (!owner) {
				return "";
			}
			if (const auto player = RE::PlayerCharacter::GetSingleton(); player && owner == player->GetActorBase()) {
				return player->GetDisplayFullName();
			}
			const char* name = owner->GetName();
			return name && *name ? name : "someone";
		}

		// The nearest food lying loose that he can see, on his own floor.
		RE::TESObjectREFR* FindLooseFood(RE::Actor* a_dog, bool a_allowOwned)
		{
			const auto cell = a_dog->GetParentCell();
			if (!cell) {
				return nullptr;
			}
			RE::TESObjectREFR* best = nullptr;
			float              bestDistance = kFoodRange;
			const auto         center = a_dog->GetPosition();
			cell->ForEachReferenceInRange(center, kFoodRange, [&](RE::TESObjectREFR* a_ref) {
				const auto base = a_ref ? a_ref->GetBaseObject() : nullptr;
				if (!base || !IsFood(base) || a_ref->IsDisabled() || a_ref->IsDeleted() || !a_ref->Is3DLoaded() ||
					std::abs(a_ref->GetPosition().z - center.z) > kRouteFloor || !a_allowOwned && a_ref->GetOwner()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				// No line-of-sight test: it fails for small things on a table (the ray meets the tabletop). His nose
				// finds it anyway; the same-cell, same-floor checks keep it to what's in the room with him.
				const float distance = a_ref->GetPosition().GetDistance(center);
				if (distance < bestDistance) {
					best = a_ref;
					bestDistance = distance;
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return best;
		}

		// The nearest calm person near him carrying food, and one of their food items.
		std::pair<RE::Actor*, RE::TESBoundObject*> FindFoodCarrier(RE::Actor* a_dog)
		{
			const auto lists = RE::ProcessLists::GetSingleton();
			std::pair<RE::Actor*, RE::TESBoundObject*> best{ nullptr, nullptr };
			float bestDistance = kFoodRange;
			if (!lists) {
				return best;
			}
			for (const auto& handle : lists->highActorHandles) {
				const auto actor = handle.get().get();
				if (!actor || actor == a_dog || actor->IsPlayerRef() || actor->IsDead() || !actor->Is3DLoaded() || actor->IsInCombat() || Combat::IsChild(actor) ||
					actor->IsHostileToActor(a_dog) || actor->GetParentCell() != a_dog->GetParentCell() && actor->GetWorldspace() != a_dog->GetWorldspace() ||
					std::abs(actor->GetPosition().z - a_dog->GetPosition().z) > kRouteFloor) {
					continue;
				}
				const float distance = actor->GetPosition().GetDistance(a_dog->GetPosition());
				if (distance >= bestDistance) {
					continue;
				}
				bool unused = false;
				if (!a_dog->HasLineOfSight(actor, unused)) {
					continue;
				}
				const auto food = actor->GetInventory([](RE::TESBoundObject& a_object) { return IsFood(&a_object); });
				for (const auto& [object, data] : food) {
					if (data.first > 0) {
						best = { actor, object };
						bestDistance = distance;
						break;
					}
				}
			}
			return best;
		}

		// Game thread, every few seconds: whether there is food in reach at all (for eligibility and the behavior loop).
		void ScanFood(RE::Actor* a_dog)
		{
			const auto now = Clock::now();
			if (now < g_nextFoodScan) {
				return;
			}
			g_nextFoodScan = now + kFoodScanEvery;
			const bool loaded = a_dog && a_dog->Is3DLoaded() && !a_dog->IsDead();
			const auto s = SnapshotSettings();
			const auto loose = loaded ? FindLooseFood(a_dog, s.takeOwnedFood) : nullptr;
			const auto carrier = loaded && s.takeOwnedFood ? FindFoodCarrier(a_dog).first : nullptr;
			if ((loose != nullptr) != g_looseFood.load() || (carrier != nullptr) != g_carriedFood.load()) {
				logs::info("Food in reach: {} lying about, {} carried", loose ? loose->GetDisplayFullName() : "none",
					carrier ? carrier->GetDisplayFullName() : "nobody");
			}
			g_looseFood = loose != nullptr;
			g_carriedFood = carrier != nullptr;
		}

		bool MenuOpen()
		{
			const auto ui = RE::UI::GetSingleton();
			if (!ui) {
				return false;
			}
			for (const auto name : { RE::GiftMenu::MENU_NAME, RE::ContainerMenu::MENU_NAME, RE::BarterMenu::MENU_NAME, RE::InventoryMenu::MENU_NAME }) {
				if (ui->IsMenuOpen(name)) {
					return true;
				}
			}
			return false;
		}

		bool FoodReady(RE::Actor* a_dog)
		{
			if (MenuOpen()) {
				return false;  // not while the player is in a trade or inventory window (it would only play out after)
			}
			const auto last = g_lastFood.load();
			const bool cooled = last == 0 || Clock::now() - Clock::time_point(Clock::duration(last)) >= kFoodCooldown;
			return cooled && !g_napping && !g_roaming && AtLeast(a_dog, Drive::kHunger, kHungryAt);
		}

		bool Scavenge(RE::Actor* a_dog)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto keyword = TravelTarget();
			if (!a_dog || !player || g_napping || g_roam || !RunPackage() || !keyword) {
				return false;
			}
			const auto food = FindLooseFood(a_dog, SnapshotSettings().takeOwnedFood);
			if (!food) {
				logs::info("Scavenge: no food in reach");
				return false;
			}
			a_dog->extraList.SetLinkedRef(food, keyword);
			PackageOverride(a_dog, true);
			g_roam = Roam{ RoamKind::kScavenge, a_dog->GetFormID(), food->GetFormID(), Clock::now() + kSniffReach, player->GetPosition() };
			g_roam->closest = a_dog->GetPosition().GetDistance(food->GetPosition());
			g_roam->closestAt = Clock::now();
			g_roaming = true;
			g_roamStill = false;
			g_lastFood = Clock::now().time_since_epoch().count();
			logs::info("Action Scavenge ({}{}, {:.0f} units away)", food->GetDisplayFullName(), food->GetOwner() ? ", owned" : "", g_roam->closest);
			return true;
		}

		bool Snatch(RE::Actor* a_dog)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto keyword = TravelTarget();
			if (!a_dog || !player || g_napping || g_roam || !RunPackage() || !keyword || !SnapshotSettings().takeOwnedFood) {
				return false;
			}
			const auto [carrier, item] = FindFoodCarrier(a_dog);
			if (!carrier || !item) {
				logs::info("Snatch: nobody near is carrying food");
				return false;
			}
			a_dog->extraList.SetLinkedRef(carrier, keyword);
			PackageOverride(a_dog, true);
			g_roam = Roam{ RoamKind::kSnatch, a_dog->GetFormID(), carrier->GetFormID(), Clock::now() + kSniffReach, player->GetPosition() };
			g_roam->closest = a_dog->GetPosition().GetDistance(carrier->GetPosition());
			g_roam->closestAt = Clock::now();
			g_roaming = true;
			g_roamStill = false;
			g_foodItem = item->GetFormID();
			g_lastFood = Clock::now().time_since_epoch().count();
			logs::info("Action Snatch ({} from {})", item->GetName(), carrier->GetDisplayFullName());
			return true;
		}

		// Game thread: Scavenge's own run (its target is an item, not a person).
		void ProcessScavenge(RE::Actor* a_dog)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto food = RE::TESForm::LookupByID<RE::TESObjectREFR>(g_roam->target);
			const char* why = nullptr;
			if (!a_dog || a_dog->GetFormID() != g_roam->dog || a_dog->IsDead() || !a_dog->Is3DLoaded() || !player) {
				why = "gone";
			} else if (a_dog->IsInCombat() || player->IsInCombat()) {
				why = "a fight";
			} else if (!food || food->IsDisabled() || food->IsDeleted() || !food->GetParentCell()) {
				why = "someone got it first";
			} else if (!g_roam->arrived && Clock::now() - g_roam->closestAt >= kNoHeadway) {
				why = "can't get to it";
			} else if (!g_roam->arrived && Clock::now() >= g_roam->until) {
				why = "couldn't reach it";
			}
			if (why) {
				EndRoam(why);
				return;
			}
			const float d = a_dog->GetPosition().GetDistance(food->GetPosition());
			if (d <= g_roam->closest - kHeadway) {
				g_roam->closest = d;
				g_roam->closestAt = Clock::now();
			}
			if (!g_roam->arrived) {
				// Right up to it, or as close as the table lets him (stalled near it for a moment).
				const bool stalledNear = d <= 200.0f && Clock::now() - g_roam->closestAt >= 2s;
				if (d > kFoodReached && !stalledNear) {
					return;
				}
				// There: a moment nosing at it first, so it's plain what he's after.
				g_roam->arrived = true;
				g_roam->until = Clock::now() + kFoodSniff;
				PlaySound(a_dog, kSniffSound);
				logs::info("Scavenge: at the {} ({:.0f} units), sniffing", food->GetDisplayFullName(), d);
				return;
			}
			if (Clock::now() < g_roam->until) {
				return;
			}
			// Then he takes it (into his inventory, where the feeding eats it), and the event goes out.
			const auto owner = OwnerName(food);
			const std::string item = food->GetDisplayFullName();
			const auto s = SnapshotSettings();
			Training::Did(owner.empty() ? "scavenge" : "steal");
			Tell(a_dog, player, owner.empty() ?
				std::format("{} noses out a {} lying nearby and wolfs it down.", a_dog->GetDisplayFullName(), item) :
				std::format("{} snatches {} {} and wolfs it down before anyone can stop him.", a_dog->GetDisplayFullName(), Possessive(owner), item));
			logs::info("Scavenge: took {}{}", item, owner.empty() ? "" : " (" + owner + "'s)");
			if (!owner.empty() && s.chargePlayer) {
				// The engine's own pick-up: an owned item (or one in an owned room) counts as a theft, and since he's
				// the player's companion it lands on them. A dog can't be fined, so the owner comes after his person.
				a_dog->PickUpObject(food, 1, false, false);
			} else {
				// No crime: the food goes straight into his inventory (where the feeding eats it) and the plate leaves
				// the world; the owners still hear of it through the narration.
				if (const auto base = food->GetBaseObject()) {
					Inputs::AddMeal(base->GetFormID(), 1);
					a_dog->AddObjectToContainer(base, nullptr, 1, nullptr);
				}
				food->Disable();
				food->SetDelete(true);
			}
			EndRoam("done");
		}

		// --- hunting and carcasses ---------------------------------------------------------------------------

		constexpr RE::FormID kTypeNPC = 0x013794, kTypeUndead = 0x013796, kTypeDwarven = 0x01397A, kTypeAnimal = 0x013798;
		constexpr RE::FormID kRingOfNamira = 0x02C37B;       // Skyrim.esm DA11RingofNamira
		constexpr RE::FormID kPlayerIsWerewolf = 0x0ED06C;   // Skyrim.esm PlayerIsWerewolf (global)
		constexpr RE::FormID kWerewolfRace = 0x0CDD84;       // Skyrim.esm WerewolfBeastRace
		constexpr RE::FormID kFeedBlood = 0x0F3A8A;          // Skyrim.esm BloodSprayWereWolfEat01, the werewolf's feeding spray
		constexpr Sound      kFeedSound{ 0x101009, "Skyrim.esm"sv };  // NPCWerewolfFeeding

		constexpr float kPreyRange = 3000.0f;
		constexpr float kPreyRise = 400.0f;
		constexpr auto  kHuntFor = 45s;              // then he has lost it
		constexpr auto  kHuntOrder = 60s;            // the order behind it outlasts that, so the hunt is what ends it
		// He tracks it by scent with no fight on, so his companions don't take it for one and blast it. The fight starts
		// only when it's about to end: up close, or within Frost Step's reach when that's ready.
		// Most hunts come before he has Frost Step or Frost Bite, so the pounce is what has to land: close enough that the
		// first bite comes before the prey can bolt (just outside the run package's arrival radius, 150).
		constexpr float kPounce = 200.0f;
		constexpr float kHuntStepRange = 1200.0f;    // on a hunt he steps from no farther than this
		constexpr auto  kScentFirst = 3s;            // nose to the air, before any Frost Step
		constexpr auto  kStalkNudge = 2s;            // standing still on the trail: his AI is reset to pick it up again
		constexpr auto  kStalkNoHeadway = 8s;        // no closer in this long: he starts casting about
		constexpr auto  kCastFor = 10s;
		constexpr auto  kCastEvery = 3s;
		constexpr float kCastSwing = 350.0f;
		constexpr auto  kHuntNoHeadway = 15s;        // after the pounce: no closer in this long, and it has got away
		constexpr float kHuntTooFar = 6000.0f;       // from the player
		constexpr int   kHuntHungry = 50;
		constexpr float kCarcassRange = 1500.0f;
		constexpr float kCarcassReached = 200.0f;
		constexpr float kCarcassStalledNear = 320.0f;  // a big body: as close as he can get
		constexpr auto  kCarcassEat = 8s;
		constexpr auto  kFishChew = 6000ms;   // chewing his catch
		constexpr auto  kCarcassFightGrace = 2s;     // the fight he just won takes a moment to end
		constexpr float kCarcassPlayerFar = 3000.0f;
		constexpr auto  kForageShowFor = 30s;        // a new find stays in his prompt this long

		// Never hunted: farm animals, other dogs, things that would flatten him. By the race's editor ID, so that a mod's
		// cow counts too (WildGoat stays fair game: farm goats are GoatDomesticsRace).
		constexpr std::array<std::string_view, 11> kNotPrey{ "cow", "chicken", "domestic", "horse", "dog", "husky", "mammoth", "netch", "spider",
			"chaurus", "whitestag" };
		// What he can take on is judged by health, not by kind (2026-11-15): prey whose health right now is within
		// killBiteMultiple of his bite (his bite damage plus Frost Bite) dies to his first bite, whatever it is; anything
		// tougher is a real fight, which he only picks while he's in good shape and it has no more health than he does.
		constexpr float kFightFit = 0.75f;   // a real fight only with at least this share of his health
		constexpr float kFightOdds = 1.0f;   // and against prey with at most this many times his health right now
		// Never eaten, even dead: his own kind, and what's no meal for a dog.
		constexpr std::array<std::string_view, 6> kNotMeat{ "dog", "husky", "horse", "spider", "chaurus", "netch" };

		std::atomic_bool        g_preyNear{ false };
		std::atomic_bool        g_carcassNear{ false };
		std::atomic<Clock::rep> g_lastHunt{ 0 };
		Clock::time_point       g_nextHuntScan{};
		std::set<RE::FormID>    g_eatenCarcasses;  // this session; he doesn't go back to a body he's finished
		std::mutex              g_forageLock;
		std::string             g_forage;
		RE::FormID              g_forageNoted = 0;
		std::optional<Clock::time_point> g_forageUntil;
		std::optional<Clock::time_point> g_forageQuietUntil;

		bool HasType(RE::Actor* a_actor, RE::FormID a_keyword)
		{
			const auto race = a_actor->GetRace();
			const auto keyword = RE::TESForm::LookupByID<RE::BGSKeyword>(a_keyword);
			return race && keyword && race->HasKeyword(keyword);
		}

		template <std::size_t N>
		bool RaceIsOneOf(RE::Actor* a_actor, const std::array<std::string_view, N>& a_tokens)
		{
			const auto  race = a_actor->GetRace();
			const char* id = race ? race->GetFormEditorID() : nullptr;
			if (!id) {
				return false;
			}
			std::string lower(id);
			std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return std::ranges::any_of(a_tokens, [&](std::string_view a_token) { return lower.find(a_token) != std::string::npos; });
		}

		// One of the player's own, whatever state they're in.
		bool Companion(RE::Actor* a_actor)
		{
			const auto faction = RE::TESForm::LookupByID<RE::TESFaction>(kFollowerFaction);
			return a_actor->IsPlayerTeammate() || (faction && a_actor->GetFactionRank(faction, false) >= 0);
		}

		// "fox", "sabre cat": a creature's name as it reads mid-sentence.
		std::string CreatureName(RE::Actor* a_actor)
		{
			const char* raw = a_actor ? a_actor->GetDisplayFullName() : nullptr;
			std::string name = raw && *raw ? raw : "animal";
			std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return name;
		}

		std::string WithArticle(const std::string& a_name)
		{
			return (std::string_view("aeiou").find(a_name.front()) != std::string_view::npos ? "an " : "a ") + a_name;
		}

		// The player is a cannibal in the game's own terms: Namira's ring on their finger, or the beast blood.
		bool PlayerEatsPeople()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return false;
			}
			const auto race = player->GetRace();
			const auto global = RE::TESForm::LookupByID<RE::TESGlobal>(kPlayerIsWerewolf);
			bool       yes = (race && race->GetFormID() == kWerewolfRace) || (global && global->value > 0.0f);
			if (!yes) {
				const auto ring = player->GetInventory([](RE::TESBoundObject& a_object) { return a_object.GetFormID() == kRingOfNamira; });
				yes = std::ranges::any_of(ring, [](const auto& a_entry) { return a_entry.second.second && a_entry.second.second->IsWorn(); });
			}
			static std::optional<bool> told;
			if (told != yes) {
				told = yes;
				logs::info("Carcass Meat auto: {}", yes ? "you wear Namira's ring or carry the beast blood, so people are food" : "animals only");
			}
			return yes;
		}

		bool PeopleAreFood(const Settings& a_settings)
		{
			return a_settings.carcassMeat >= 2 || (a_settings.carcassMeat == 1 && PlayerEatsPeople());
		}

		bool IsPerson(RE::Actor* a_actor)
		{
			return HasType(a_actor, kTypeNPC);
		}

		// A body he'd eat from. Children are never food, at any setting.
		bool IsMeat(RE::Actor* a_body, bool a_people)
		{
			if (Combat::IsChild(a_body) || Companion(a_body) || a_body->IsGhost() || g_eatenCarcasses.contains(a_body->GetFormID()) ||
				HasType(a_body, kTypeDwarven) || RaceIsOneOf(a_body, kNotMeat)) {
				return false;
			}
			// The walking dead are bones and dust; a vampire is still a person.
			if (HasType(a_body, kTypeUndead) && !RaceIsOneOf(a_body, std::array<std::string_view, 1>{ "vampire" })) {
				return false;
			}
			return IsPerson(a_body) ? a_people : HasType(a_body, kTypeAnimal);
		}

		// The nearest body he'd eat from, on his floor. No line-of-sight test (as with loose food, his nose finds it, and a
		// body on the ground fails the test too easily); a wall in the way shows up as no headway.
		RE::Actor* FindCarcass(RE::Actor* a_dog, const Settings& a_settings)
		{
			const auto tes = RE::TES::GetSingleton();
			if (!tes) {
				return nullptr;
			}
			const bool people = PeopleAreFood(a_settings);
			const auto center = a_dog->GetPosition();
			RE::Actor* best = nullptr;
			float      bestDistance = kCarcassRange;
			tes->ForEachReferenceInRange(a_dog, kCarcassRange, [&](RE::TESObjectREFR* a_ref) {
				const auto actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
				if (!actor || actor == a_dog || !actor->IsDead() || actor->IsDisabled() || actor->IsDeleted() || !actor->Is3DLoaded() ||
					std::abs(actor->GetPosition().z - center.z) > kRouteFloor || !IsMeat(actor, people)) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				if (const float distance = actor->GetPosition().GetDistance(center); distance < bestDistance) {
					best = actor;
					bestDistance = distance;
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return best;
		}

		// His bite: the race's unarmed damage, what his growth adds (the UnarmedDamage actor value), and Frost Bite.
		struct Bite
		{
			float body = 0.0f;
			float frost = 0.0f;
			float Total() const { return body + frost; }
		};

		Bite BiteOf(RE::Actor* a_dog)
		{
			Bite bite;
			const auto race = a_dog->GetRace();
			const auto values = a_dog->AsActorValueOwner();
			bite.body = (race ? race->data.unarmedDamage : 0.0f) + (values ? values->GetActorValue(RE::ActorValue::kUnarmedDamage) : 0.0f);
			bite.frost = Gifts::FrostBiteDamage();
			return bite;
		}

		float HealthNow(RE::Actor* a_actor)
		{
			const auto values = a_actor ? a_actor->AsActorValueOwner() : nullptr;
			return values ? values->GetActorValue(RE::ActorValue::kHealth) : 0.0f;
		}

		// His first bite kills it: its health right now is within killBiteMultiple of his bite.
		bool KillBite(RE::Actor* a_dog, RE::Actor* a_prey, const Settings& a_settings)
		{
			return a_settings.killBiteMultiple > 0.0 && HealthNow(a_prey) <= BiteOf(a_dog).Total() * static_cast<float>(a_settings.killBiteMultiple);
		}

		// He takes it on: a killing bite, or a fight he's fit for.
		bool CanTake(RE::Actor* a_dog, RE::Actor* a_prey, const Settings& a_settings)
		{
			if (KillBite(a_dog, a_prey, a_settings)) {
				return true;
			}
			const auto  values = a_dog->AsActorValueOwner();
			const float max = values ? values->GetPermanentActorValue(RE::ActorValue::kHealth) : 0.0f;
			const float now = HealthNow(a_dog);
			return max > 0.0f && now >= max * kFightFit && HealthNow(a_prey) <= now * kFightOdds;
		}

		std::string HuntOdds(RE::Actor* a_dog, RE::Actor* a_prey, const Settings& a_settings)
		{
			const auto bite = BiteOf(a_dog);
			return std::format("{} at {:.0f} health; his bite {:.0f} + frost {:.0f} = {:.0f}, x{:.1f} = {:.0f}; he has {:.0f} health",
				CreatureName(a_prey), HealthNow(a_prey), bite.body, bite.frost, bite.Total(), a_settings.killBiteMultiple,
				bite.Total() * a_settings.killBiteMultiple, HealthNow(a_dog));
		}

		// The nearest wild animal within what he can take on, belonging to nobody.
		RE::Actor* FindPrey(RE::Actor* a_dog)
		{
			const auto lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return nullptr;
			}
			RE::Actor* best = nullptr;
			float      bestDistance = kPreyRange;
			const auto s = SnapshotSettings();
			for (const auto* list : { &lists->highActorHandles, &lists->middleHighActorHandles }) {
				for (const auto& handle : *list) {
					const auto ptr = handle.get();
					const auto actor = ptr.get();
					if (!actor || actor == a_dog || actor->IsPlayerRef() || actor->IsDead() || !actor->Is3DLoaded() || actor->IsInCombat() ||
						actor->GetParentCell() != a_dog->GetParentCell() && actor->GetWorldspace() != a_dog->GetWorldspace() ||
						std::abs(actor->GetPosition().z - a_dog->GetPosition().z) > kPreyRise) {
						continue;
					}
					const float distance = actor->GetPosition().GetDistance(a_dog->GetPosition());
					const auto  values = actor->AsActorValueOwner();
					if (distance >= bestDistance || !values || Combat::IsChild(actor) || !HasType(actor, kTypeAnimal) || IsPerson(actor) ||
						RaceIsOneOf(actor, kNotPrey) || Companion(actor) || actor->GetCrimeFaction() || actor->IsEssential() || actor->IsProtected() ||
						!CanTake(a_dog, actor, s)) {
						continue;
					}
					best = actor;
					bestDistance = distance;
				}
			}
			return best;
		}

		bool HuntCooled(const Settings& a_settings)
		{
			const auto last = g_lastHunt.load();
			return last == 0 || Clock::now() - Clock::time_point(Clock::duration(last)) >=
			                        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::ratio<60>>(a_settings.huntCooldown));
		}

		// --- fishing ---------------------------------------------------------------------------------------------
		// Fish are critters, not actors: activators running the vanilla critterFish script (salmon and pond fish in
		// Skyrim.esm; CC Fishing's, Beyond Skyrim's and others' species run the same script). Activating one gives its
		// loot to whoever activates it and removes the fish, so his catch is an activation by him.
		constexpr float kFishRange = 2000.0f;        // he spots fish this far off
		constexpr float kFishPlayerRange = 2500.0f;  // and only while they're this close to the player
		constexpr float kFishReach = 170.0f;         // sideways, nose to fish (his run package stops him at 150: he lunges the rest)
		constexpr float kFishReachDepth = 170.0f;    // the fish swims a little below him at the surface
		constexpr float kSalmonPreference = 0.6f;    // he's fond of salmon: one this much farther still wins
		constexpr auto  kFishFor = 40s;
		constexpr auto  kFishCooldown = 3min;
		constexpr auto  kFishMarkerEvery = 400ms;    // the marker he swims to follows the fish this often
		constexpr auto  kFishNoHeadway = 15s;
		constexpr auto  kFishMealDelay = 1500ms;     // the catch's script hands over the loot a moment later
		std::atomic_bool        g_fishNear{ false };
		std::atomic<Clock::rep> g_lastFish{ 0 };     // steady_clock ticks; 0 = never
		std::map<RE::FormID, std::int32_t> g_fishBefore;  // his inventory just before the catch
		Clock::time_point       g_fishMarkerAt{};

		std::string Lowered(std::string_view a_text)
		{
			std::string out(a_text);
			std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		}

		// A catchable fish: a critter activator whose model is under Critters\Fish\ (salmon, longfin, river betty, ...),
		// or one with the critterFish script bound. The model test came first on 2026-11-15: salmon right by a river camp
		// went unseen, so the script lookup alone isn't to be trusted for spawned critters. The leaping waterfall salmon
		// (Effects\FXAmbWaterfallSalmon, FXfakeCritterScript) are props and match neither.
		std::string_view ModelPath(RE::TESBoundObject* a_base)
		{
			const auto model = a_base ? a_base->As<RE::TESModel>() : nullptr;
			const char* path = model ? model->GetModel() : nullptr;
			return path ? std::string_view(path) : std::string_view{};
		}

		// The ambient schooling salmon (FXAmbWaterSalmon02A and kin) are Flora, harvested like a plant for their meat
		// (2026-11-15: the salmon by a river camp were these). Catchable while not yet harvested; plants never match.
		RE::TESBoundObject* FloraFishProduce(RE::TESObjectREFR* a_ref)
		{
			const auto base = a_ref ? a_ref->GetBaseObject() : nullptr;
			const auto flora = base ? base->As<RE::TESFlora>() : nullptr;
			if (!flora || !flora->produceItem || (a_ref->formFlags & RE::TESObjectREFR::RecordFlags::kHarvested)) {
				return nullptr;
			}
			// Any fish-named harvestable: the schooling salmon, and the Slaughterfish Egg Nest too (the user, 2026-11-15:
			// disgusting, but food all the same). Watch for other fish-named plants that shouldn't count.
			const char* name = a_ref->GetDisplayFullName();
			const auto  low = Lowered(name ? name : "") + " " + Lowered(ModelPath(base));
			const bool  fishy = low.find("salmon") != std::string::npos || low.find("fish") != std::string::npos ||
			                   low.find("fxambwater") != std::string::npos;
			return fishy ? flora->produceItem : nullptr;
		}

		bool IsCritterFish(RE::TESObjectREFR* a_ref)
		{
			const auto base = a_ref ? a_ref->GetBaseObject() : nullptr;
			if (FloraFishProduce(a_ref)) {
				return true;
			}
			if (!base || !base->Is(RE::FormType::Activator)) {
				return false;
			}
			if (Lowered(ModelPath(base)).find("critters\\fish\\") != std::string::npos) {
				return true;
			}
			const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			const auto policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
			if (!policy) {
				return false;
			}
			const auto handle = policy->GetHandleForObject(a_ref->GetFormType(), a_ref);
			RE::BSTSmartPointer<RE::BSScript::Object> object;
			return handle != policy->EmptyHandle() && vm->FindBoundObject(handle, "critterFish", object) && object;
		}

		bool IsSalmon(RE::TESObjectREFR* a_fish)
		{
			const char* name = a_fish ? a_fish->GetDisplayFullName() : nullptr;
			std::string lower = name ? name : "";
			std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return lower.find("salmon") != std::string::npos;
		}

		bool FishCooled()
		{
			const auto last = g_lastFish.load();
			return last == 0 || Clock::now() - Clock::time_point(Clock::duration(last)) >= kFishCooldown;
		}

		// The nearest fish swimming within his reach (salmon first), near the player.
		RE::TESObjectREFR* FindFish(RE::Actor* a_dog)
		{
			const auto tes = RE::TES::GetSingleton();
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!tes || !a_dog || !player) {
				return nullptr;
			}
			RE::TESObjectREFR* best = nullptr;
			float              bestScore = kFishRange;
			const auto         center = a_dog->GetPosition();
			std::string        unlike;  // fish-named things he passed over, for the log
			int                unlikeCount = 0;
			tes->ForEachReferenceInRange(a_dog, kFishRange, [&](RE::TESObjectREFR* a_ref) {
				if (!a_ref || a_ref->IsDisabled() || a_ref->IsDeleted() || !a_ref->Is3DLoaded() ||
					a_ref->GetPosition().GetDistance(player->GetPosition()) > kFishPlayerRange) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				if (!IsCritterFish(a_ref)) {
					const auto  kind = a_ref->GetBaseObject();
					if (!kind || !(kind->Is(RE::FormType::Activator) || kind->Is(RE::FormType::Flora))) {
						return RE::BSContainer::ForEachResult::kContinue;   // only what could have been a fish (not a fishing rod)
					}
					const char* name = a_ref->GetDisplayFullName();
					const auto  low = Lowered(name ? name : "");
					if (low.find("salmon") != std::string::npos || low.find("fish") != std::string::npos) {
						if (++unlikeCount == 1) {
							const auto base = a_ref->GetBaseObject();
							unlike = std::format("{} (base {:08X}, {}, model {})", name, base ? base->GetFormID() : 0u,
								base ? base->GetObjectTypeName() : "?", base ? ModelPath(base) : ""sv);
						}
					}
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const float score = a_ref->GetPosition().GetDistance(center) * (IsSalmon(a_ref) ? kSalmonPreference : 1.0f);
				if (score < bestScore) {
					best = a_ref;
					bestScore = score;
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			// Fish-named things nearby but nothing he could catch: say what they are, once a minute (props such as the
			// leaping waterfall salmon, or a fish mod's critters this test doesn't know yet).
			static Clock::time_point lastUnlike{};
			if (!best && unlikeCount > 0 && Clock::now() - lastUnlike >= 60s) {
				lastUnlike = Clock::now();
				logs::info("Fish: {} fish-named thing(s) near, none he can catch; e.g. {}", unlikeCount, unlike);
			}
			return best;
		}

		std::string FishName(RE::TESObjectREFR* a_fish)
		{
			const char* raw = a_fish ? a_fish->GetDisplayFullName() : nullptr;
			std::string name = raw && *raw ? raw : "fish";
			std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return name;
		}

		// Game thread, every few seconds: what's in reach, for eligibility and the behavior loop, and a line for his prompt
		// when something new catches his nose. Like a scent it is news for a moment and then old, and it only reaches his
		// prompt while he's hungry, so a field of rabbits doesn't fill his talk.
		void ScanHunt(RE::Actor* a_dog)
		{
			const auto now = Clock::now();
			if (now < g_nextHuntScan) {
				return;
			}
			g_nextHuntScan = now + kFoodScanEvery;
			const auto s = SnapshotSettings();
			const bool able = s.hunting && a_dog && a_dog->Is3DLoaded() && !a_dog->IsDead() && !a_dog->IsInCombat();
			const auto prey = able ? FindPrey(a_dog) : nullptr;
			const auto carcass = able ? FindCarcass(a_dog, s) : nullptr;
			const auto fish = able ? FindFish(a_dog) : nullptr;
			if ((prey != nullptr) != g_preyNear.load() || (carcass != nullptr) != g_carcassNear.load() || (fish != nullptr) != g_fishNear.load()) {
				logs::info("Hunting: {} on the air, carcass {}, fish {}", prey ? prey->GetDisplayFullName() : "nothing",
					carcass ? carcass->GetDisplayFullName() : "none", fish ? fish->GetDisplayFullName() : "none");
			}
			g_preyNear = prey != nullptr;
			g_carcassNear = carcass != nullptr;
			g_fishNear = fish != nullptr;

			RE::TESObjectREFR* find = carcass ? carcass : prey;
			if (!find) {
				find = fish;
			}
			if (find && find->GetFormID() != g_forageNoted && (!g_forageQuietUntil || now >= *g_forageQuietUntil)) {
				g_forageNoted = find->GetFormID();
				g_forageUntil = now + kForageShowFor;
				g_forageQuietUntil = *g_forageUntil + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s.smellQuiet));
			}
			std::string fact;
			if (find && g_forageUntil && now < *g_forageUntil && AtLeast(a_dog, Drive::kHunger, kHungryAt)) {
				const auto dogName = a_dog->GetDisplayFullName();
				fact = !carcass && !prey ? std::format("{} has spotted {} swimming in the water close by.", dogName, WithArticle(FishName(fish))) :
				       !carcass ? std::format("{} can smell {} close by.", dogName, WithArticle(CreatureName(prey))) :
				       IsPerson(carcass) ? std::format("{} can smell the body of {} close by.", dogName, carcass->GetDisplayFullName()) :
				                           std::format("{} can smell the carcass of {} close by.", dogName, WithArticle(CreatureName(carcass)));
			}
			// Fish in the water aren't a passing scent: he can see them, and they stay. While any are in reach and he's
			// hungry or restless (as for the Fish action), his prompt keeps saying so, alongside whatever scent is news.
			// (2026-11-15: at a river camp a mudcrab's scent took the only slot and the salmon never reached his talk.)
			if (fish && (AtLeast(a_dog, Drive::kHunger, kHungryAt) || AtLeast(a_dog, Drive::kRestless, kPlayfulAt)) &&
				fact.find("swimming in the water") == std::string::npos) {
				fact += (fact.empty() ? "" : " ") + std::format("{} swimming in the water close by.",
					IsSalmon(fish) ? "Salmon are" : "Fish are");
			}
			std::scoped_lock lock(g_forageLock);
			g_forage = std::move(fact);
		}

		// --- NPCs petting him ---------------------------------------------------------------------------------

		constexpr float kPetReach = 220.0f;      // only someone already this close is offered it
		constexpr float kPetReachSlack = 60.0f;  // by the time they act on it, they may have shifted a little
		constexpr auto  kPetScanEvery = 1s;
		constexpr auto  kPetKneel = 2500ms;

		std::mutex              g_pettersLock;
		std::set<RE::FormID>    g_petters;  // published for eligibility on SkyrimNet's workers
		std::atomic<RE::FormID> g_petDog{ 0 };
		std::atomic<Clock::rep> g_lastNpcPet{ 0 };
		Clock::time_point       g_nextPetScan{};

		bool FreeToBePetted(RE::Actor* a_dog)
		{
			return a_dog && a_dog->Is3DLoaded() && !a_dog->IsDead() && !a_dog->IsInCombat() && !g_napping && !g_roaming && !g_downed;
		}

		// Standing beside him with nothing else going on: no fight, no weapon out, not sitting, sleeping or walking past.
		bool FreeToPet(RE::Actor* a_actor, RE::Actor* a_dog, float a_reach)
		{
			if (!a_actor || a_actor == a_dog || a_actor->IsPlayerRef() || a_actor->IsDead() || !a_actor->Is3DLoaded() || a_actor->IsInCombat() || !IsPerson(a_actor) ||
				a_actor->IsHostileToActor(a_dog) || a_actor->IsMoving() || a_actor->GetParentCell() != a_dog->GetParentCell() && a_actor->GetWorldspace() != a_dog->GetWorldspace()) {
				return false;
			}
			const auto state = a_actor->AsActorState();
			return !state->IsWeaponDrawn() && state->GetSitSleepState() == RE::SIT_SLEEP_STATE::kNormal && !state->IsBleedingOut() &&
			       a_actor->GetPosition().GetDistance(a_dog->GetPosition()) <= a_reach;
		}

		// Game thread, every second: who could pet him right now.
		void ScanPetters(RE::Actor* a_dog)
		{
			const auto now = Clock::now();
			if (now < g_nextPetScan) {
				return;
			}
			g_nextPetScan = now + kPetScanEvery;
			g_petDog = a_dog ? a_dog->GetFormID() : 0;
			std::set<RE::FormID> close;
			const auto           lists = RE::ProcessLists::GetSingleton();
			if (lists && SnapshotSettings().npcPet && FreeToBePetted(a_dog)) {
				for (const auto& handle : lists->highActorHandles) {
					const auto actor = handle.get();
					if (FreeToPet(actor.get(), a_dog, kPetReach)) {
						close.insert(actor->GetFormID());
					}
				}
			}
			std::scoped_lock lock(g_pettersLock);
			g_petters = std::move(close);
		}

		// Game thread: he makes for this body and eats from it.
		bool BeginCarcass(RE::Actor* a_dog, RE::Actor* a_body, bool a_kill)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto keyword = TravelTarget();
			if (!a_dog || !a_body || !player || !RunPackage() || !keyword) {
				return false;
			}
			a_dog->extraList.SetLinkedRef(a_body, keyword);
			PackageOverride(a_dog, true);
			const auto now = Clock::now();
			g_roam = Roam{ RoamKind::kCarcass, a_dog->GetFormID(), a_body->GetFormID(), now + kSniffReach, player->GetPosition() };
			g_roam->closest = a_dog->GetPosition().GetDistance(a_body->GetPosition());
			g_roam->closestAt = now;
			g_roam->legSince = now;
			g_roam->kill = a_kill;
			g_roaming = true;
			g_roamStill = false;
			g_lastFood = now.time_since_epoch().count();
			logs::info("Carcass: going to {} ({:.0f} units away{})", a_body->GetDisplayFullName(), g_roam->closest, a_kill ? ", his kill" : "");
			return true;
		}

		bool EatCarcass(RE::Actor* a_dog)
		{
			if (!a_dog || g_napping || g_roam) {
				return false;
			}
			const auto body = FindCarcass(a_dog, SnapshotSettings());
			if (!body) {
				logs::info("Carcass: nothing to eat from in reach");
				return false;
			}
			return BeginCarcass(a_dog, body, false);
		}

		bool Hunt(RE::Actor* a_dog)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto keyword = TravelTarget();
			if (!a_dog || !player || g_napping || g_roam || !RunPackage() || !keyword) {
				return false;
			}
			const auto prey = FindPrey(a_dog);
			if (!prey) {
				logs::info("Hunt: nothing on the air to chase");
				return false;
			}
			const auto now = Clock::now();
			a_dog->extraList.SetLinkedRef(prey, keyword);
			PackageOverride(a_dog, true);
			g_roam = Roam{ RoamKind::kHunt, a_dog->GetFormID(), prey->GetFormID(), now + kHuntFor, player->GetPosition() };
			logs::info("Hunt: setting off after {}", HuntOdds(a_dog, prey, SnapshotSettings()));
			g_roam->closest = a_dog->GetPosition().GetDistance(prey->GetPosition());
			g_roam->closestAt = now;
			g_roam->legSince = now;
			g_roaming = true;
			g_roamStill = false;
			g_lastHunt = now.time_since_epoch().count();
			PlaySound(a_dog, kSniffSound);
			// A good run takes it all out of him, whether or not he catches anything.
			const auto s = SnapshotSettings();
			Drives::Adjust(Drive::kRestless, -100.0, s);
			Drives::Adjust(Drive::kTired, kPlayTiring, s);
			logs::info("Action Hunt ({}, {:.0f} units away; restless -> {:.1f})", prey->GetDisplayFullName(), g_roam->closest, Drives::Value(Drive::kRestless));
			return true;
		}

		// A hunt that came to nothing: he has been pacing with his nose down, and now he gives it up. Quiet, like any
		// small failure: people notice it if they notice it.
		void LostTrail(RE::Actor* a_dog, const std::string& a_prey, const char* a_why)
		{
			if (a_dog && a_dog->Is3DLoaded()) {
				PlaySound(a_dog, kWhine);
				if (const auto s = SnapshotSettings(); s.events) {
					Bridge::PostEvent(std::format("{} loses the {}'s trail. He casts back and forth with his nose to the ground for a while, then gives it up.",
										  a_dog->GetDisplayFullName(), a_prey),
						a_dog->GetFormID(), 0, false);
				}
			}
			EndRoam(a_why);
		}

		// Game thread: the trail has run into something he can't get past. He casts back and forth across it with his nose
		// down, on the same package, aimed at a marker that swings from side to side.
		void CastAbout(RE::Actor* a_dog, RE::Actor* a_prey, RE::Actor* a_player, Clock::time_point a_now)
		{
			if (a_now < g_roam->castNext) {
				return;
			}
			const auto keyword = TravelTarget();
			const auto marker = Marker(a_player);
			if (!keyword || !marker) {
				return;
			}
			g_roam->castNext = a_now + kCastEvery;
			g_roam->castRight = !g_roam->castRight;
			const auto  from = a_dog->GetPosition();
			const auto  to = a_prey->GetPosition() - from;
			const float length = (std::max)(1.0f, std::hypot(to.x, to.y));
			const float side = (g_roam->castRight ? kCastSwing : -kCastSwing) / length;
			marker->SetPosition(RE::NiPoint3(from.x - to.y * side, from.y + to.x * side, from.z));
			a_dog->extraList.SetLinkedRef(marker, keyword);
			a_dog->EvaluatePackage(true, true);
			PlaySound(a_dog, kSniffSound);
		}

		// Game thread: the fight starts now, at the end of the stalk.
		void Pounce(RE::Actor* a_dog, RE::Actor* a_prey, const char* a_how)
		{
			ReleasePlay(a_dog);
			g_roam->pounced = true;
			g_roam->castUntil.reset();
			g_roam->closestAt = Clock::now();
			const auto s = SnapshotSettings();
			const bool kill = KillBite(a_dog, a_prey, s);
			Combat::Hunt(a_dog, a_prey, std::chrono::duration_cast<std::chrono::seconds>(kHuntOrder), kill);
			logs::info("Hunt: {} ({:.0f} units from the {}; {})", a_how, a_dog->GetPosition().GetDistance(a_prey->GetPosition()), CreatureName(a_prey),
				kill ? "his first bite kills it" : "too tough for one bite: a real fight");
			logs::info("Hunt: {}", HuntOdds(a_dog, a_prey, s));
		}

		// Game thread: his own hunt. Stalking by scent first; after the pounce the fight runs itself, with the order behind
		// it keeping him at it.
		void ProcessHunt(RE::Actor* a_dog)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto prey = RE::TESForm::LookupByID<RE::Actor>(g_roam->target);
			if (!a_dog || a_dog->GetFormID() != g_roam->dog || a_dog->IsDead() || !a_dog->Is3DLoaded() || !player) {
				EndRoam("gone");
				return;
			}
			if (player->IsInCombat()) {
				EndRoam("a fight");  // back to his person
				return;
			}
			if (Hurt(a_dog)) {
				EndRoam("he's hurt");
				return;
			}
			if (Combat::CalledOff()) {
				EndRoam("called off");
				return;
			}
			const std::string name = prey ? CreatureName(prey) : "animal";
			if (!prey || prey->IsDisabled() || prey->IsDeleted()) {
				LostTrail(a_dog, name, "lost it");
				return;
			}
			if (prey->IsDead()) {
				// Someone else may have got there first: he still eats, but it isn't his kill.
				const auto killer = prey->GetActorRuntimeData().myKiller.get();
				const bool his = Combat::BitToDeath(prey->GetFormID()) || killer.get() == a_dog;
				Combat::EndHunt(a_dog, his ? "caught it" : "something else got it");
				if (his) {
					Training::Did("hunt");
				}
				logs::info("Hunt: the {} is down, {}", name, his ? "his kill"s : std::format("killed by {}", killer ? killer->GetDisplayFullName() : "nobody known"));
				ReleasePlay(a_dog);
				g_roam.reset();
				g_roaming = false;
				g_roamStill = false;
				BeginCarcass(a_dog, prey, his);
				return;
			}
			if (g_roam->pounced && !Combat::Hunted()) {
				EndRoam("called off");  // an order of the player's replaced it
				return;
			}
			const auto  now = Clock::now();
			const float distance = a_dog->GetPosition().GetDistance(prey->GetPosition());
			if (distance <= g_roam->closest - kHeadway) {
				g_roam->closest = distance;
				g_roam->closestAt = now;
			}
			if (!prey->Is3DLoaded()) {
				LostTrail(a_dog, name, "it got away");
				return;
			}
			if (now >= g_roam->until) {
				LostTrail(a_dog, name, "out of time");
				return;
			}
			if (a_dog->GetPosition().GetDistance(player->GetPosition()) > kHuntTooFar) {
				LostTrail(a_dog, name, "too far from you");
				return;
			}
			if (g_roam->pounced) {
				if (now - g_roam->closestAt >= kHuntNoHeadway) {
					LostTrail(a_dog, name, "couldn't get to it");
				}
				return;
			}
			// Stalking. Something else started a fight with him: that comes first, unless it's the prey itself.
			if (a_dog->IsInCombat()) {
				const auto aim = a_dog->GetActorRuntimeData().currentCombatTarget.get();
				if (aim && aim.get() != prey) {
					EndRoam("a fight");
				} else {
					Pounce(a_dog, prey, "the fight found him first");
				}
				return;
			}
			if (distance <= kPounce) {
				Pounce(a_dog, prey, "pouncing");
				return;
			}
			if (distance <= kHuntStepRange && now - g_roam->legSince >= kScentFirst && Gifts::StepWithin(a_dog, prey)) {
				Pounce(a_dog, prey, "within Frost Step's reach");
				return;
			}
			if (g_roam->castUntil) {
				if (now >= *g_roam->castUntil) {
					LostTrail(a_dog, name, "couldn't get to it");
				} else {
					CastAbout(a_dog, prey, player, now);
				}
				return;
			}
			if (now - g_roam->closestAt >= kStalkNoHeadway) {
				g_roam->castUntil = now + kCastFor;
				g_roam->castNext = now;
				logs::info("Hunt: no way through to the {}, casting about", name);
				return;
			}
			// The package finishes when he gets near, and doesn't always restart if the prey has moved on.
			if (!a_dog->IsMoving() && now - g_roam->legSince >= kStalkNudge) {
				g_roam->legSince = now - kScentFirst;
				a_dog->EvaluatePackage(true, true);
			}
		}

		bool Fish(RE::Actor* a_dog)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto keyword = TravelTarget();
			if (!a_dog || !player || g_napping || g_posing || g_roam || !RunPackage() || !keyword) {
				return false;
			}
			const auto fish = FindFish(a_dog);
			const auto marker = fish ? Marker(player) : nullptr;
			if (!fish || !marker) {
				logs::info("Fish: no fish in reach");
				return false;
			}
			const auto now = Clock::now();
			marker->SetPosition(fish->GetPosition());
			a_dog->extraList.SetLinkedRef(marker, keyword);
			PackageOverride(a_dog, true);
			g_roam = Roam{ RoamKind::kFish, a_dog->GetFormID(), fish->GetFormID(), now + kFishFor, player->GetPosition() };
			if (const auto produce = FloraFishProduce(fish)) {
				g_roam->produce = produce->GetFormID();
			}
			g_roam->closest = a_dog->GetPosition().GetDistance(fish->GetPosition());
			g_roam->closestAt = now;
			g_roam->legSince = now;
			g_roaming = true;
			g_roamStill = false;
			g_fishMarkerAt = now;
			g_lastFish = now.time_since_epoch().count();
			const auto s = SnapshotSettings();
			Drives::Adjust(Drive::kRestless, -60.0, s);
			Drives::Adjust(Drive::kTired, kPlayTiring, s);
			logs::info("Action Fish ({}, {:.0f} units away)", fish->GetDisplayFullName(), g_roam->closest);
			return true;
		}

		// Game thread: after a fish. He swims to a marker that follows it; the catch is his activation of it.
		void ProcessFish(RE::Actor* a_dog)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!a_dog || a_dog->GetFormID() != g_roam->dog || a_dog->IsDead() || !a_dog->Is3DLoaded() || !player) {
				EndRoam("gone");
				return;
			}
			const auto now = Clock::now();
			if (g_roam->eating) {
				// The catch's loot has arrived: food goes to his feeding by itself; a fish that's an ingredient (the pond
				// fish) he eats here.
				if (now < g_roam->until) {
					return;
				}
				// A flora fish he activated should have dropped its meat into his inventory. If nothing came, hand it over
				// and mark the fish harvested (it leaves the water as it would for the player).
				if (g_roam->produce) {
					bool gainedAny = false;
					for (const auto& [object, count] : a_dog->GetInventoryCounts()) {
						const auto it = g_fishBefore.find(object->GetFormID());
						gainedAny = gainedAny || count > (it == g_fishBefore.end() ? 0 : it->second);
					}
					const auto produce = RE::TESForm::LookupByID<RE::TESBoundObject>(g_roam->produce);
					const auto fishRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(g_roam->target);
					if (!gainedAny && produce) {
						a_dog->AddObjectToContainer(produce, nullptr, 1, nullptr);
						if (fishRef && !(fishRef->formFlags & RE::TESObjectREFR::RecordFlags::kHarvested)) {
							fishRef->formFlags |= RE::TESObjectREFR::RecordFlags::kHarvested;
							fishRef->AddChange(RE::TESForm::ChangeFlags::kFlags);  // the record flags change, so the harvest is saved
						}
						logs::info("Fish: the catch gave him nothing by itself; handed him the {}", produce->GetName());
					}
				}
				const auto s = SnapshotSettings();
				double relief = 0.0;
				std::string eaten;
				bool food = false;
				for (const auto& [object, count] : a_dog->GetInventoryCounts()) {
					const auto it = g_fishBefore.find(object->GetFormID());
					const auto gained = count - (it == g_fishBefore.end() ? 0 : it->second);
					if (gained <= 0) {
						continue;
					}
					if (object->Is(RE::FormType::Ingredient)) {
						a_dog->RemoveItem(object, gained, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
						relief += (s.feedPerItem + s.feedRawBonus) * gained;
						eaten += (eaten.empty() ? "" : " and ") + std::string(object->GetName());
					} else if (const auto item = object->As<RE::AlchemyItem>(); item && item->IsFood()) {
						food = true;  // the container sink has it: he eats it like any meal
					}
				}
				g_fishBefore.clear();
				if (relief > 0.0) {
					Drives::Adjust(Drive::kHunger, -(std::min)(relief, s.feedMaxMeal), s);
					Inputs::NoteEating();
					Idles::Play(a_dog, Idles::kDogLay, "idleLayStart");
					SoundCues::Chew(a_dog, kFishChew);
					Tell(a_dog, player, std::format("{} lies down with his catch and eats the {}, scales and all.", a_dog->GetDisplayFullName(), eaten));
					logs::info("Fish: ate {} (hunger -> {:.1f})", eaten, Drives::Value(Drive::kHunger));
				} else if (!food) {
					logs::info("Fish: the catch gave him nothing to eat");
				}
				EndRoam("done");
				return;
			}
			if (a_dog->IsInCombat() || player->IsInCombat()) {
				EndRoam("a fight");
				return;
			}
			if (Hurt(a_dog)) {
				EndRoam("he's hurt");
				return;
			}
			const auto fish = RE::TESForm::LookupByID<RE::TESObjectREFR>(g_roam->target);
			if (!fish || fish->IsDisabled() || fish->IsDeleted() || !fish->Is3DLoaded()) {
				EndRoam("it got away");
				return;
			}
			if (player->GetPosition().GetDistance(g_roam->playerPos) > kPlayerMovedOn) {
				EndRoam("you moved on");
				return;
			}
			if (now >= g_roam->until) {
				EndRoam("it got away");
				return;
			}
			const auto  me = a_dog->GetPosition();
			const auto  at = fish->GetPosition();
			const float flat = std::hypot(at.x - me.x, at.y - me.y);
			if (flat <= g_roam->closest - kHeadway) {
				g_roam->closest = flat;
				g_roam->closestAt = now;
			}
			if (now - g_roam->closestAt >= kFishNoHeadway) {
				EndRoam("couldn't get to it");
				return;
			}
			// Into the water: the event goes out as he splashes in, so it matches what everyone sees.
			if (!g_roam->pounced && a_dog->IsInWater()) {
				g_roam->pounced = true;
				Tell(a_dog, player, std::format("{} spots {} in the water and splashes in after it, paddling hard.", a_dog->GetDisplayFullName(),
					WithArticle(FishName(fish))));
				logs::info("Fish: he's in the water, {:.0f} units from it", flat);
			}
			if (flat <= kFishReach && std::abs(at.z - me.z) <= kFishReachDepth) {
				// The catch. His inventory first, so the meal is what the fish added.
				g_fishBefore.clear();
				for (const auto& [object, count] : a_dog->GetInventoryCounts()) {
					g_fishBefore[object->GetFormID()] = count;
				}
				const auto name = FishName(fish);
				PlaySound(a_dog, kSniffSound);
				fish->ActivateRef(a_dog, 0, nullptr, 1, false);
				g_roam->arrived = true;
				g_roam->eating = true;
				g_roam->until = now + kFishMealDelay;
				Training::Did("hunt");
				Tell(a_dog, player, std::format("{} plunges his head under and comes up with {} thrashing in his jaws!", a_dog->GetDisplayFullName(),
					WithArticle(name)));
				logs::info("Fish: caught the {}", name);
				return;
			}
			// The marker follows the fish; if he stands about, his package is nudged to pick it up again.
			if (now - g_fishMarkerAt >= kFishMarkerEvery) {
				g_fishMarkerAt = now;
				if (const auto marker = g_marker.get()) {
					marker->SetPosition(at);
				}
				if (!a_dog->IsMoving() && now - g_roam->legSince >= kStalkNudge) {
					g_roam->legSince = now;
					a_dog->EvaluatePackage(true, true);
				}
			}
		}

		// Game thread: to the body, a sniff, then he lies down to it. His hunger drops once he has finished.
		void ProcessCarcass(RE::Actor* a_dog)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto body = RE::TESForm::LookupByID<RE::Actor>(g_roam->target);
			const auto now = Clock::now();
			const char* why = nullptr;
			if (!a_dog || a_dog->GetFormID() != g_roam->dog || a_dog->IsDead() || !a_dog->Is3DLoaded() || !player) {
				why = "gone";
			} else if (player->IsInCombat() || (a_dog->IsInCombat() && now - g_roam->legSince >= kCarcassFightGrace)) {
				why = "a fight";
			} else if (!body || !body->IsDead() || body->IsDisabled() || body->IsDeleted() || !body->Is3DLoaded()) {
				why = "it's gone";
			} else if (a_dog->GetPosition().GetDistance(player->GetPosition()) > kCarcassPlayerFar) {
				why = "you moved on";
			} else if (!g_roam->arrived && now - g_roam->closestAt >= kNoHeadway) {
				why = "can't get to it";
			} else if (!g_roam->arrived && now >= g_roam->until) {
				why = "couldn't reach it";
			}
			if (why) {
				EndRoam(why);
				return;
			}
			const float d = a_dog->GetPosition().GetDistance(body->GetPosition());
			if (d <= g_roam->closest - kHeadway) {
				g_roam->closest = d;
				g_roam->closestAt = now;
			}
			if (!g_roam->arrived) {
				if (d > kCarcassReached && !(d <= kCarcassStalledNear && now - g_roam->closestAt >= 2s)) {
					return;
				}
				g_roam->arrived = true;
				g_roamStill = true;
				g_roam->until = now + kFoodSniff;
				PlaySound(a_dog, kSniffSound);
				logs::info("Carcass: at {} ({:.0f} units), sniffing", body->GetDisplayFullName(), d);
				return;
			}
			if (now < g_roam->until) {
				return;
			}
			if (!g_roam->eating) {
				g_roam->eating = true;
				g_roam->until = now + kCarcassEat;
				const bool person = IsPerson(body);
				Idles::Play(a_dog, Idles::kDogLay, "idleLayStart");
				Inputs::NoteEating();
				SoundCues::Chew(a_dog, std::chrono::duration_cast<std::chrono::milliseconds>(kCarcassEat));
				if (person) {
					// The werewolf's feeding: a spray of blood from the body, and the wet sound of it. Not for animals.
					const auto seconds = std::chrono::duration<float>(kCarcassEat).count();
					if (const auto art = RE::TESForm::LookupByID<RE::BGSArtObject>(kFeedBlood)) {
						body->ApplyArtObject(art, seconds);
					}
					PlaySound(a_dog, kFeedSound);
					SoundLater(a_dog->GetFormID(), 4s, kFeedSound);
				}
				const auto dogName = a_dog->GetDisplayFullName();
				Tell(a_dog, player, person ?
					std::format("{} noses at the body of {}, then lies down and tears into it, eating his fill.", dogName, body->GetDisplayFullName()) :
					g_roam->kill ?
					std::format("{} runs down {} and lies down to tear into the carcass, eating his fill.", dogName, WithArticle(CreatureName(body))) :
					std::format("{} noses at the carcass of {}, then lies down and tears into it, eating his fill.", dogName, WithArticle(CreatureName(body))));
				logs::info("Carcass: eating from {}{}", body->GetDisplayFullName(), person ? " (a person: blood and the feeding sound)" : "");
				return;
			}
			const auto s = SnapshotSettings();
			Drives::Adjust(Drive::kHunger, -s.carcassMeal, s);
			Training::Did("eat");
			g_eatenCarcasses.insert(body->GetFormID());
			a_dog->NotifyAnimationGraph("IdleStop");
			SoundLater(a_dog->GetFormID(), 600ms, kShake);
			logs::info("Carcass: finished {} (hunger -{:.0f} -> {:.1f})", body->GetDisplayFullName(), s.carcassMeal, Drives::Value(Drive::kHunger));
			EndRoam("done");
		}

		// Game thread, every tick: while he's down (bleedout) he lies on the ground; up again once he recovers.
		void UpdateDowned(RE::Actor* a_dog)
		{
			const auto state = a_dog ? a_dog->AsActorState() : nullptr;
			const bool down = state && a_dog->Is3DLoaded() && !a_dog->IsDead() && state->IsBleedingOut();
			const auto now = Clock::now();
			if (down && !g_downed) {
				g_downed = true;
				g_nextDownPose = now;
				if (g_roam) {
					EndRoam("he's down");
				}
				PlaySound(a_dog, kWhine);
				if (const auto player = RE::PlayerCharacter::GetSingleton()) {
					Tell(a_dog, player, std::format("{} collapses onto his side, whimpering, too badly hurt to get up.", a_dog->GetDisplayFullName()));
				}
				const auto values = a_dog->AsActorValueOwner();
				logs::info("Down: he's in bleedout, lying down (health {:.0f}/{:.0f})", values ? values->GetActorValue(RE::ActorValue::kHealth) : -1.0f,
					values ? values->GetPermanentActorValue(RE::ActorValue::kHealth) : -1.0f);
			} else if (!down && g_downed) {
				g_downed = false;
				if (a_dog && a_dog->Is3DLoaded() && !a_dog->IsDead()) {
					a_dog->NotifyAnimationGraph("IdleStop");
				}
				const auto values = a_dog ? a_dog->AsActorValueOwner() : nullptr;
				logs::info("Down: he's back on his feet (health {:.0f}/{:.0f})", values ? values->GetActorValue(RE::ActorValue::kHealth) : -1.0f,
					values ? values->GetPermanentActorValue(RE::ActorValue::kHealth) : -1.0f);
			}
			// The engine's bleedout doesn't hold a dog's pose: lay him (back) down every few seconds while it lasts.
			if (g_downed && now >= g_nextDownPose) {
				g_nextDownPose = now + 3s;
				if (!IsLying(a_dog)) {
					a_dog->NotifyAnimationGraph("idleLayStart");
				}
			}
		}

		Clock::time_point g_nextBowCheck{};
		std::optional<Clock::time_point> g_lastBow;

		// Game thread, every tick: now and then, while restless is high and he stands about near the player, he drops
		// into a play bow.
		void UpdatePlayBow(RE::Actor* a_dog)
		{
			const auto now = Clock::now();
			if (!a_dog || now < g_nextBowCheck) {
				return;
			}
			g_nextBowCheck = now + kBowCheck;
			if (g_napping || g_posing || g_roaming || g_roam || g_downed || !a_dog->Is3DLoaded() || !Calm(a_dog) || a_dog->IsMoving() ||
				!AtLeast(a_dog, Drive::kRestless, kBowAt) || (g_lastBow && now - *g_lastBow < kBowCooldown) || IsLying(a_dog)) {
				return;
			}
			std::int32_t sitting = 0;
			if (a_dog->GetGraphVariableInt("isIdleSitting", sitting) && sitting) {
				return;
			}
			if (std::uniform_int_distribution<int>(1, 100)(Random()) > kBowChance) {
				return;
			}
			const auto lastRoll = g_lastRollTick.load();
			const bool rollReady = lastRoll == 0 || now - Clock::time_point(Clock::duration(lastRoll)) >= kRollAgain;
			if (rollReady && std::uniform_int_distribution<int>(1, 100)(Random()) <= kRollInsteadOfBow &&
				Actions::Rollover(a_dog, "restless")) {
				g_lastBow = now;  // the same cooldown as a bow
				return;
			}
			Actions::PlayBow(a_dog, "restless");
		}

		// Game thread, every tick: keeps the laps going, and ends a roam the moment it should stop.
		void ProcessRoam(RE::Actor* a_dog)
		{
			if (!g_roam) {
				return;
			}
			if (g_roam->kind == RoamKind::kScavenge) {
				ProcessScavenge(a_dog);
				return;
			}
			if (g_roam->kind == RoamKind::kHunt) {
				ProcessHunt(a_dog);
				return;
			}
			if (g_roam->kind == RoamKind::kFish) {
				ProcessFish(a_dog);
				return;
			}
			if (g_roam->kind == RoamKind::kCarcass) {
				ProcessCarcass(a_dog);
				return;
			}
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto target = RE::TESForm::LookupByID<RE::Actor>(g_roam->target);
			const char* why = nullptr;
			if (!a_dog || a_dog->GetFormID() != g_roam->dog || a_dog->IsDead() || !a_dog->Is3DLoaded() || !player) {
				why = "gone";
			} else if (a_dog->IsInCombat() || player->IsInCombat()) {
				why = "a fight";
			} else if (!target || target->IsDead() || !target->Is3DLoaded() || target->IsHostileToActor(a_dog)) {
				why = "lost them";
			} else if (g_roam->kind == RoamKind::kSniff && target->GetPosition().GetDistance(player->GetPosition()) > kSniffRange) {
				why = "they walked off";
			} else if (g_roam->kind != RoamKind::kGreet && player->GetPosition().GetDistance(g_roam->playerPos) > kPlayerMovedOn) {
				why = "you moved on";  // (a greeting runs to the player wherever they walk)
			} else if (g_roam->kind != RoamKind::kPlay && !g_roam->arrived && Clock::now() - g_roam->closestAt >= kNoHeadway) {
				why = "can't get to them";  // stuck behind something: no use pawing at it for the full wait
			} else if (Clock::now() >= g_roam->until) {
				why = g_roam->arrived ? "done" : g_roam->kind != RoamKind::kPlay ? "couldn't reach them" : "never got going";
			}
			if (why) {
				const bool greeted = g_roam->kind == RoamKind::kGreet && g_roam->arrived && why == std::string_view("done");
				EndRoam(why);
				if (greeted) {
					Actions::Wiggle(a_dog, "greeting");  // done fussing over them: the whole back end goes
				}
				return;
			}
			// Diagnostics while we learn whether the offset takes: where he is, whether he moves, what package runs.
			{
				const auto package = a_dog->GetCurrentPackage();
				const auto  marker = g_marker.get();
				const float toWaypoint = g_roam->kind == RoamKind::kPlay && marker ? a_dog->GetPosition().GetDistance(marker->GetPosition()) : 0.0f;
				const auto keyword = TravelTarget();
				const auto linked = keyword ? a_dog->extraList.GetLinkedRef(keyword) : nullptr;
				logs::info("Roam: {:.0f} units from {}, {:.0f} from waypoint, moving={}, package {:08X} type {}, travel link {:08X}{}",
					a_dog->GetPosition().GetDistance(target->GetPosition()), target->GetDisplayFullName(), toWaypoint, a_dog->IsMoving(),
					package ? package->GetFormID() : 0,
					package ? static_cast<int>(package->packData.packType.get()) : -1,
					linked ? linked->GetFormID() : 0, linked && marker && linked == marker.get() ? " (marker)" : "");
			}
			if (g_roam->kind != RoamKind::kPlay && !g_roam->arrived) {
				if (const float d = a_dog->GetPosition().GetDistance(target->GetPosition()); d <= g_roam->closest - kHeadway) {
					g_roam->closest = d;
					g_roam->closestAt = Clock::now();
				}
			}
			if (g_roam->kind == RoamKind::kSniff && !g_roam->arrived && a_dog->GetPosition().GetDistance(target->GetPosition()) <= kSniffArrived) {
				// He's there: now the event goes out (as narration the one being sniffed reacts), and the nosing about starts.
				g_roam->arrived = true;
				g_roam->until = Clock::now() + kSniffTime;
				PlaySound(a_dog, kSniffSound);
				Tell(target, a_dog, std::format("{} trots over to {} and sniffs them over, nose busy and tail wagging.",
					a_dog->GetDisplayFullName(), target->GetDisplayFullName()));
				logs::info("Sniff: reached {}", target->GetDisplayFullName());
			}
			if (g_roam->kind == RoamKind::kGreet && !g_roam->arrived && a_dog->GetPosition().GetDistance(target->GetPosition()) <= kSniffArrived) {
				g_roam->arrived = true;
				g_roam->until = Clock::now() + kGreetTime;
				PlaySound(a_dog, kSniffSound);
				const auto s = SnapshotSettings();
				Drives::Adjust(Drive::kSeparation, -100.0, s);
				Drives::Adjust(Drive::kAffection, -15.0, s);
				Tell(a_dog, target, std::format("{} spots {} and bounds over, tail whipping, sniffing {} all over as if checking where they've been.",
					a_dog->GetDisplayFullName(), target->GetDisplayFullName(), target->GetDisplayFullName()));
				logs::info("Greet: reached {}", target->GetDisplayFullName());
			}
			if (g_roam->kind == RoamKind::kSnatch && !g_roam->arrived && a_dog->GetPosition().GetDistance(target->GetPosition()) <= kSniffArrived) {
				// There: one food item out of their pack and into his (the feeding eats it), and the event goes to them.
				g_roam->arrived = true;
				const auto item = RE::TESForm::LookupByID<RE::TESBoundObject>(g_foodItem);
				if (item && target->GetInventoryCounts()[item] > 0) {
					const char* itemName = item->GetName();
					Training::Did("steal");
					PlaySound(a_dog, kSniffSound);
					// From the one robbed, so SkyrimNet has them react (as Sniff does with the one sniffed).
					Tell(target, a_dog, std::format("{} noses into {} pack and makes off with {}!", a_dog->GetDisplayFullName(),
						Possessive(target->GetDisplayFullName()), itemName && *itemName ? itemName : "something to eat"));
					logs::info("Snatch: took {} from {}", itemName ? itemName : "?", target->GetDisplayFullName());
					target->RemoveItem(item, 1, SnapshotSettings().chargePlayer ? RE::ITEM_REMOVE_REASON::kSteal : RE::ITEM_REMOVE_REASON::kRemove, nullptr, a_dog);
				}
				EndRoam("done");
				return;
			}
			if (g_roam->kind == RoamKind::kWarm && !g_roam->arrived && a_dog->GetPosition().GetDistance(target->GetPosition()) <= kSniffArrived) {
				// There: the run package lets go and he keeps tight at their side (they're likely heading for shelter, so he
				// walks with them rather than lying down), and the warmth starts.
				g_roam->arrived = true;
				Tell(a_dog, target, std::format("{} presses his warm, thick-furred body against {} and sticks tight at their side, sharing his heat.",
					a_dog->GetDisplayFullName(), target->GetDisplayFullName()));
				logs::info("Warm: reached {}, pressing close", target->GetDisplayFullName());
				EndRoam("done");
				g_warmUntil = Clock::now() + kWarmFor;
				g_warmNext = Clock::now();
				g_warmedOnce = false;
				return;
			}
			if (g_roam->kind != RoamKind::kPlay) {
				return;
			}
			if (!g_roam->arrived && a_dog->IsMoving()) {
				// He's off: now the clock starts and the event goes out, so the reaction matches what everyone sees.
				g_roam->arrived = true;
				g_roam->until = Clock::now() + kPlayTime;
				logs::info("Play: he's running");
				Tell(a_dog, target, g_roam->route.empty() ?
					std::format("{} bursts into a mad, happy run, racing in circles around {} and barking.",
						a_dog->GetDisplayFullName(), target->GetDisplayFullName()) :
					std::format("{} gets a sudden fit of the zoomies, dashing around the room past people and furniture and skidding back by {}.",
						a_dog->GetDisplayFullName(), target->GetDisplayFullName()));
			}
		}

		bool AskForFood(RE::Actor* a_dog)
		{
			if (!a_dog) {
				return false;
			}
			g_lastAsk = Clock::now().time_since_epoch().count();
			Training::Did("beg");
			Idles::Play(a_dog, Idles::kDogSit, "idleSitStart");
			SoundLater(a_dog->GetFormID(), 800ms, kWhine);  // a hopeful whine while sitting

			std::thread([dog = a_dog->GetFormID(), asked = Clock::now(), session = Bridge::Session()] {
				std::this_thread::sleep_for(kGiftMinDelay);
				for (;;) {
					const auto now = Clock::now();
					const auto lineAt = Clock::time_point(Clock::duration(g_dogLineAt.load()));
					// Best: SkyrimNet says his line has finished playing (any line ending after the ask).
					if (const auto ended = Clock::time_point(Clock::duration(g_dogAudioEnd.load())); ended > asked && now >= ended + 400ms) {
						logs::info("AskForFood: his line finished playing");
						break;
					}
					if (lineAt >= asked - 5s) {  // the line can be registered just before the action runs
						const auto wait = (std::min)(std::chrono::duration_cast<Clock::duration>(kLineLead + kPerWord * g_dogLineWords.load()),
							std::chrono::duration_cast<Clock::duration>(kGiftLineCap));
						if (now >= lineAt + wait) {
							break;  // his line has had time to play
						}
					} else if (now - asked >= kGiftNoLine) {
						break;  // no line came with it
					}
					if (now - asked >= kGiftMaxWait) {
						break;
					}
					std::this_thread::sleep_for(250ms);
				}
				for (int i = 0; i < kGiftTries; ++i) {
					if (session != Bridge::Session()) {
						return;  // loaded another save meanwhile
					}
					auto done = std::make_shared<std::atomic_bool>(false);
					SKSE::GetTaskInterface()->AddTask([dog, done] { *done = TryOpenGiftMenu(dog); });
					std::this_thread::sleep_for(1s);
					if (*done) {
						return;
					}
				}
				logs::info("AskForFood: no gift menu (a menu stayed open or the game stayed paused)");
			}).detach();
			logs::info("Action AskForFood");
			return true;
		}

		bool Nap(RE::Actor* a_dog)
		{
			if (!a_dog) {
				return false;
			}
			if (g_posing) {
				// Lying in a pose already: switching to the sleep clips under him would snap him. The nap can come after.
				logs::info("Nap: not while he's in a pose");
				return false;
			}
			EndRoam("nap");
			Mood::BeginSleep(a_dog);  // before the idle: OAR picks the lie-down clip as it starts
			// The wait package holds him in place while his AI stays on: the follow package can't pick new idles and
			// stand him up, and a fight still reaches him at once (the package doesn't run in combat).
			// The package switch stands him up if it lands during the lie-down (the override goes through Papyrus and
			// takes effect a moment later), so he lies down once the wait package is running (see UpdateNap).
			if (const auto wait = WaitPackage()) {
				PackageOverride(a_dog, true, wait);
				g_holding = a_dog->GetFormID();
				g_layPending = true;
			} else {
				logs::warn("Nap: the DoNothing package wasn't found; the follow package may stand him up");
				Idles::Play(a_dog, Idles::kDogLay, "idleLayStart");
				g_layPending = false;
			}
			SleepIdles(a_dog, true);  // from here the AI picks none of his idles; our own lie-down still plays
			g_napDog = a_dog->GetFormID();
			Notify::About(g_napDog, "{} lies down for a nap.");
			g_napping = true;
			g_lying = false;  // until his graph says so
			g_napStarted = g_lastLayDown = g_graceFrom = Clock::now();
			Training::Did("nap");
			g_relays = 0;
			g_upSince.reset();
			g_downSince.reset();
			if (const auto player = RE::PlayerCharacter::GetSingleton()) {
				g_napPlayerPos = player->GetPosition();
			}
			logs::info("Action Nap");
			return true;
		}

		// The pose ends: a_standUp asks him to get up (time's up, the player walked off); otherwise he's up already or a
		// fight or nap takes over. The pose code stays (Mood) until he's on his feet, so the get-up clip matches the pose.
		void EndPose(const char* a_why, bool a_standUp)
		{
			if (!g_pose) {
				return;
			}
			const auto dog = RE::TESForm::LookupByID<RE::Actor>(g_pose->dog);
			g_pose.reset();
			g_posing = false;
			if (a_standUp && dog && dog->Is3DLoaded() && !dog->IsDead() && IsLying(dog)) {
				Idles::Play(dog, kDogIdleStop, "IdleStop");
			}
			logs::info("Pose over ({})", a_why);
		}

		// Game thread, every tick while a pose is on.
		void UpdatePose(RE::Actor* a_dog)
		{
			if (!g_pose) {
				return;
			}
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto now = Clock::now();
			if (!a_dog || a_dog->GetFormID() != g_pose->dog || a_dog->IsDead() || !a_dog->Is3DLoaded() || !player) {
				EndPose("gone", false);
				return;
			}
			if (a_dog->IsInCombat() || player->IsInCombat()) {
				EndPose("a fight", false);
				return;
			}
			if (IsLying(a_dog)) {
				if (!g_pose->downSince) {
					g_pose->downSince = now;
					logs::info("Pose: he's down ({})", Sleep::Name(g_pose->style));
				}
				g_pose->upSince.reset();
			} else if (!g_pose->downSince) {
				if (now - g_pose->started >= kPoseDownWait) {
					EndPose("the lie-down didn't take", false);
				}
				return;
			} else if (!g_pose->upSince) {
				g_pose->upSince = now;
			} else if (now - *g_pose->upSince >= kPoseUpFor) {
				EndPose("he got up", false);
				return;
			}
			if (player->GetPosition().GetDistance(g_pose->playerPos) > kPlayerMovedOn) {
				EndPose("you moved on", true);
			} else if (now >= g_pose->until) {
				EndPose("time's up", true);
			}
		}

		// A lying pose as body language (the dialogue actions): the sleep style's body clips under their own code, awake.
		bool StartPose(RE::Actor* a_dog, Sleep::Style a_style)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!a_dog || !player || !a_dog->Is3DLoaded() || a_dog->IsDead() || g_napping || g_posing || g_downed) {
				logs::info("Action pose: {} (not now: napping, posing, down or not loaded)", Sleep::Name(a_style));
				return false;
			}
			if (IsLying(a_dog)) {
				// Swapping the clips under a dog already lying would snap him into the pose.
				logs::info("Action pose: {} (not now: he's already lying down)", Sleep::Name(a_style));
				return false;
			}
			EndRoam("pose");
			if (!Mood::BeginPose(a_dog, Mood::kPoseCode + static_cast<int>(a_style))) {  // the code first: OAR picks the clip as it starts
				return false;
			}
			Idles::Play(a_dog, Idles::kDogLay, "idleLayStart");
			const auto now = Clock::now();
			const auto hold = std::chrono::seconds(std::uniform_int_distribution<int>(
				static_cast<int>(kPoseMin.count()), static_cast<int>(kPoseMax.count()))(Random()));
			g_pose = PoseState{ a_dog->GetFormID(), a_style, now, now + hold, player->GetPosition(), std::nullopt, std::nullopt };
			g_posing = true;
			if (SnapshotSettings().events) {
				const auto name = a_dog->GetDisplayFullName();
				Bridge::PostEvent(
					a_style == Sleep::Style::kRelax   ? std::format("{} flops down and rolls onto his side, legs stretched out, belly bared, at ease.", name) :
					a_style == Sleep::Style::kGuarded ? std::format("{} lies down and rests his chin on his front paws, eyes following what goes on.", name) :
					                                    std::format("{} lies down and curls up tight, his tail wrapped over his nose.", name),
					a_dog->GetFormID(), 0);
			}
			logs::info("Action pose: {} (lying down, awake, for {} s)", Sleep::Name(a_style), hold.count());
			return true;
		}

		void ReleaseHold()
		{
			if (!g_holding) {
				return;
			}
			if (const auto dog = RE::TESForm::LookupByID<RE::Actor>(g_holding)) {
				PackageOverride(dog, false, WaitPackage());
				logs::info("Nap: wait package off, back to following");
			}
			g_holding = 0;
		}

		void EndNap(const char* a_why, bool a_calm = true)
		{
			const auto dog = g_settled ? RE::TESForm::LookupByID<RE::Actor>(g_settled) : nullptr;
			const auto slept = g_settled ? Clock::now() - g_asleepSince : Clock::duration::zero();
			g_napping = false;
			g_lying = false;
			g_settledLine.reset();
			g_settled = 0;
			g_layPending = false;
			SleepIdles(nullptr, false);  // before the hold goes: the follow package needs the idle stop to stand him up
			if (g_aiPaused) {
				if (const auto paused = RE::TESForm::LookupByID<RE::Actor>(g_aiPaused); paused && !paused->IsAIEnabled()) {
					paused->EnableAI(true);
					logs::info("Nap: AI back on");
				}
				g_aiPaused = 0;
			}
			ReleaseHold();
			logs::info("Nap over ({})", a_why);
			if (a_why != std::string_view("gone")) {
				Notify::About(g_napDog, a_why == std::string_view("he won't settle") ? "{} gives up on his nap." : "{} wakes from his nap.");
			}
			if (a_calm && dog && dog->Is3DLoaded()) {
				SoundLater(dog->GetFormID(), 600ms, kShake);  // a good shake on getting up
			}
			// A long, undisturbed nap can bring a dream (not one cut short by a fight).
			const double minutes = SnapshotSettings().dreamNapMinutes;
			if (a_calm && dog && minutes > 0.0 && std::chrono::duration<double, std::ratio<60>>(slept).count() >= minutes) {
				Dreams::Request(dog, "nap");
			}
		}

		bool SeekAffection(RE::Actor* a_dog)
		{
			if (!a_dog) {
				return false;
			}
			g_lastAffection = Clock::now().time_since_epoch().count();
			Training::Did("seek_affection");
			Idles::Play(a_dog, Idles::kDogSit, "idleSitStart");
			SoundLater(a_dog->GetFormID(), 800ms, kWhine);
			logs::info("Action SeekAffection");
			return true;
		}
	}

	void DumpGraph(RE::Actor* a_actor, std::string_view a_label)
	{
		RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
		if (!a_actor || !a_actor->GetAnimationGraphManager(manager) || !manager) {
			logs::info("Graph dump: no animation graph");
			return;
		}
		int total = 0;
		std::string line;
		for (const auto& graph : manager->graphs) {
			const auto behavior = graph ? graph->behaviorGraph : nullptr;
			if (!behavior || !behavior->data || !behavior->data->stringData) {
				continue;
			}
			for (const auto& name : behavior->data->stringData->variableNames) {
				const char* text = name.c_str();
				if (!text || !*text) {
					continue;
				}
				++total;
				// The type isn't stored here, so read all three and keep whatever is non-zero.
				bool         b = false;
				std::int32_t i = 0;
				float        f = 0.0f;
				a_actor->GetGraphVariableBool(text, b);
				a_actor->GetGraphVariableInt(text, i);
				a_actor->GetGraphVariableFloat(text, f);
				if (b || i != 0 || f != 0.0f) {
					line += std::format("{}{}=b{}/i{}/f{:.2f}", line.empty() ? "" : " ", text, b ? 1 : 0, i, f);
				}
			}
		}
		logs::info("Graph dump [{}]: {} variables; non-zero: {}", a_label, total, line);
	}

	void ScentAlert(RE::Actor* a_dog, bool a_threat, std::string_view a_where)
	{
		if (a_threat) {
			Runes::Light(Runes::Flare::kScent);
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!a_dog || !player || !a_dog->Is3DLoaded()) {
			return;
		}
		if (a_threat) {
			Idles::Play(a_dog, kDogIdleWarn, "aggroWarning2Start");  // the warning stance, with the husky growl (no code 40: vanilla)
			Tell(a_dog, player, std::format("{} stops dead, nose in the air and hackles up, and lets out a low growl toward something {}.",
				a_dog->GetDisplayFullName(), a_where));
		} else {
			PlaySound(a_dog, kWhine);
			Tell(a_dog, player, std::format("{}'s ears prick up and his nose works the air; he gives an eager little whine toward something {}.",
				a_dog->GetDisplayFullName(), a_where));
		}
	}

	namespace
	{
		// The game's own "entered bleedout": he lies down the moment he goes down, not at the next tick.
		class BleedoutSink final : public RE::BSTEventSink<RE::TESEnterBleedoutEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESEnterBleedoutEvent* a_event, RE::BSTEventSource<RE::TESEnterBleedoutEvent>*) override
			{
				const auto actor = a_event && a_event->actor ? a_event->actor->As<RE::Actor>() : nullptr;
				if (actor && IsDog(actor)) {
					SKSE::GetTaskInterface()->AddTask([id = actor->GetFormID()] { UpdateDowned(RE::TESForm::LookupByID<RE::Actor>(id)); });
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		BleedoutSink g_bleedoutSink;
	}

	namespace
	{
		// SkyrimNet's "his line has finished playing" (a mod event sent with the speaker as sender).
		class AudioEndedSink final : public RE::BSTEventSink<SKSE::ModCallbackEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* a_event, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
			{
				if (a_event && a_event->eventName == "SkyrimNet_AudioEnded"sv) {
					const auto actor = a_event->sender ? a_event->sender->As<RE::Actor>() : nullptr;
					if (actor && IsDog(actor)) {
						g_dogAudioEnd = Clock::now().time_since_epoch().count();
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		AudioEndedSink g_audioEndedSink;
	}

	void RegisterEvents()
	{
		if (const auto events = RE::ScriptEventSourceHolder::GetSingleton()) {
			events->AddEventSink<RE::TESEnterBleedoutEvent>(&g_bleedoutSink);
		}
		if (const auto mods = SKSE::GetModCallbackEventSource()) {
			mods->AddEventSink(&g_audioEndedSink);
		}
	}

	bool Downed()
	{
		return g_downed;
	}

	bool Hurt(RE::Actor* a_dog)
	{
		const auto values = a_dog ? a_dog->AsActorValueOwner() : nullptr;
		if (!values || g_downed || a_dog->AsActorState()->IsBleedingOut()) {
			return true;
		}
		const float max = values->GetPermanentActorValue(RE::ActorValue::kHealth);
		return max > 0.0f && values->GetActorValue(RE::ActorValue::kHealth) < max * 0.25f;
	}

	bool SongToJoin(RE::Actor* a_dog)
	{
		return !g_napping && !g_roaming && SingReady(a_dog);
	}

	bool FoodInReach()
	{
		return g_looseFood;
	}

	bool FishInReach()
	{
		return g_fishNear && FishCooled();
	}

	bool PreyInReach()
	{
		return g_preyNear && HuntCooled(SnapshotSettings());
	}

	bool CarcassInReach()
	{
		return g_carcassNear;
	}

	std::string ForageFact()
	{
		std::scoped_lock lock(g_forageLock);
		return g_forage;
	}

	bool CanPetHim(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}
		const auto s = SnapshotSettings();
		const auto last = g_lastNpcPet.load();
		if (!s.npcPet || (last != 0 && Clock::now() - Clock::time_point(Clock::duration(last)) < std::chrono::duration<double>(s.npcPetCooldown))) {
			return false;
		}
		std::scoped_lock lock(g_pettersLock);
		return g_petters.contains(a_actor->GetFormID());
	}

	void PettedByNpc(RE::FormID a_actor)
	{
		const auto npc = RE::TESForm::LookupByID<RE::Actor>(a_actor);
		const auto dog = RE::TESForm::LookupByID<RE::Actor>(g_petDog.load());
		if (!npc || !FreeToBePetted(dog) || !FreeToPet(npc, dog, kPetReach + kPetReachSlack)) {
			logs::info("NPC pet: {} isn't free to pet him any more", npc ? npc->GetDisplayFullName() : "someone");
			return;
		}
		g_lastNpcPet = Clock::now().time_since_epoch().count();
		const auto s = SnapshotSettings();
		Drives::Adjust(Drive::kAffection, -s.petAffection, s);
		logs::info("NPC pet: {} ({:.0f} units from him; affection -> {:.1f})", npc->GetDisplayFullName(), npc->GetPosition().GetDistance(dog->GetPosition()), Drives::Value(Drive::kAffection));

		// Face him, kneel where they stand, stand back up after a moment; he sits for it and barks. (As the player's pet.)
		const auto to = dog->GetPosition() - npc->GetPosition();
		npc->SetHeading(std::atan2(to.x, to.y));
		Idles::Play(npc, Idles::kSearchBody, "IdleKneeling");
		Idles::Play(dog, Idles::kDogSit, "idleSitStart");
		std::thread([a_actor, session = Bridge::Session()] {
			std::this_thread::sleep_for(kPetKneel);
			SKSE::GetTaskInterface()->AddTask([a_actor, session] {
				const auto actor = session == Bridge::Session() ? RE::TESForm::LookupByID<RE::Actor>(a_actor) : nullptr;
				if (actor && actor->Is3DLoaded() && !actor->IsDead()) {
					Idles::Play(actor, Idles::kStopLoose, "IdleStop");
				}
			});
		}).detach();
		std::thread([dogId = dog->GetFormID(), session = Bridge::Session()] {
			std::this_thread::sleep_for(2000ms);  // time to sit down first
			SKSE::GetTaskInterface()->AddTask([dogId, session] {
				const auto actor = session == Bridge::Session() ? RE::TESForm::LookupByID<RE::Actor>(dogId) : nullptr;
				if (actor && actor->Is3DLoaded() && !actor->IsDead()) {
					Bark(actor, true);
				}
			});
		}).detach();
		if (s.events) {
			Bridge::PostEvent(std::format("{} kneels and scratches {} behind the ears.", npc->GetDisplayFullName(), dog->GetDisplayFullName()), npc->GetFormID(), dog->GetFormID(), false);
		}
	}

	bool Warming()
	{
		return g_warmUntil.has_value();
	}

	bool IsDog(RE::Actor* a_actor)
	{
		return Drives::Peek(a_actor, Drive::kHunger).has_value();
	}

	void BuildFoodList()
	{
		const auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::BGSListForm>();
		const auto data = RE::TESDataHandler::GetSingleton();
		if (!factory || !data) {
			return;
		}
		g_foodList = factory->Create();
		if (!g_foodList) {
			logs::warn("Could not create the food list; the gift menu will show everything");
			return;
		}
		int count = 0;
		for (const auto item : data->GetFormArray<RE::AlchemyItem>()) {
			if (item && item->IsFood() && !item->IsPoison()) {
				g_foodList->AddForm(item);
				++count;
			}
		}
		logs::info("Food list: {} items", count);
	}

	std::vector<Spec> All()
	{
		return {
			{ "AskForFood",
				"Use when the dog is hungry and wants food from {{ player.name }}: he sits in front of them, whines hopefully and "
				"waits while {{ player.name }} decides what to give him, if anything.",
				[](RE::Actor* a_dog) {
					const auto last = g_lastAsk.load();
					const bool cooled = last == 0 || Clock::now() - Clock::time_point(Clock::duration(last)) >= kAskCooldown;
					return cooled && AtLeast(a_dog, Drive::kHunger, kHungryAt) && Calm(a_dog) && !Hurt(a_dog);
				},
				AskForFood },
			{ "Nap",
				"Only when the dog lies down to rest or sleep: he lies down on the ground nearby, curls up and dozes until something "
				"stirs him or {{ player.name }} moves on. Not for greeting, playing, begging or seeking affection.",
				[](RE::Actor* a_dog) { return !g_napping && AtLeast(a_dog, Drive::kTired, kTiredAt) && Calm(a_dog); },
				Nap },
			{ "SeekAffection",
				"Use when the dog wants attention or comfort from {{ player.name }}: he sits close, leans against their legs and "
				"nudges a hand for a scratch.",
				// Any time he's calm, not only when lonely: a dog wants a scratch after a scare or a reunion whatever the
				// need says. The need still decides how often the behavior loop asks for one on its own.
				[](RE::Actor* a_dog) {
					const auto last = g_lastAffection.load();
					const bool cooled = last == 0 || Clock::now() - Clock::time_point(Clock::duration(last)) >= kAffectionCooldown;
					return cooled && !g_napping && !g_roaming && Calm(a_dog);
				},
				SeekAffection },
			{ "Play",
				"Use when the dog is bursting with energy and wants to play: he races in wide, happy circles around "
				"{{ player.name }}, barking, for a few moments.",
				[](RE::Actor* a_dog) {
					const auto tired = Drives::Peek(a_dog, Drive::kTired);
					return !g_napping && !g_roaming && AtLeast(a_dog, Drive::kRestless, kPlayfulAt) && tired && *tired < kTooTiredToPlay && Calm(a_dog);
				},
				Play },
			{ "Sniff",
				"Use when the dog is restless or curious about someone nearby: he trots over to a person or animal close by, "
				"sniffs them over, then comes back to {{ player.name }}.",
				[](RE::Actor* a_dog) { return !g_napping && !g_roaming && AtLeast(a_dog, Drive::kRestless, kRestlessAt) && Calm(a_dog); },
				Sniff },
			{ "WarmWithBodyHeat",
				"Use when {{ player.name }} is cold, shivering or freezing and the dog presses close to warm or comfort them: he runs "
				"over, leans his body against them and sticks close at their side, sharing his body heat for a while.",
				[](RE::Actor* a_dog) {
					const auto last = g_lastWarm.load();
					const bool cooled = last == 0 || Clock::now() - Clock::time_point(Clock::duration(last)) >= kWarmCooldown;
					return cooled && g_playerCold && !g_napping && !g_roaming && Calm(a_dog);
				},
				Warm, true },
			{ "Sing",
				"Use when a bard is performing nearby and the dog joins in, because the music has him worked up or someone urges him to sing: he sits back and howls along with the "
				"song, loud and off-key, for everyone to hear.",
				[](RE::Actor* a_dog) { return !g_napping && !g_roaming && SingReady(a_dog) && Calm(a_dog); },
				Sing },
			{ "AttackTarget",
				"Use only when {{ player.name }} tells the dog to attack, bite or go after someone by name, or someone is "
				"attacking {{ player.name }} right now: he lunges at them and fights until they are down for good or "
				"{{ player.name }} calls him off. Not for play, warnings or growling, and never against a child.",
				[](RE::Actor* a_dog) { return a_dog && !a_dog->IsDead() && !g_napping && !Hurt(a_dog); },
				nullptr, true,
				R"({"type": "object", "properties": {"target": {"type": "string", "description": "Name of the person or creature to attack"}}, "required": ["target"]})",
				[](RE::Actor* a_dog, const std::string& a_params) {
					// The model doesn't always answer in the schema's shape: besides {"target": "..."} it has sent the schema
					// back filled in ({"properties": {"target": "..."}} or {"properties": {"target": {"description": "..."}}}).
					std::string name = a_params;
					try {
						const auto json = nlohmann::json::parse(a_params);
						const auto text = [](const nlohmann::json& a_value) -> std::string {
							if (a_value.is_string()) {
								return a_value.get<std::string>();
							}
							if (a_value.is_object()) {
								for (const auto* key : { "value", "name", "description" }) {
									if (const auto it = a_value.find(key); it != a_value.end() && it->is_string()) {
										return it->get<std::string>();
									}
								}
							}
							return {};
						};
						std::string found;
						if (json.is_string()) {
							found = json.get<std::string>();
						} else if (json.is_object()) {
							if (const auto it = json.find("target"); it != json.end()) {
								found = text(*it);
							}
							if (const auto props = json.find("properties"); found.empty() && props != json.end() && props->is_object()) {
								if (const auto it = props->find("target"); it != props->end()) {
									found = text(*it);
								}
							}
						}
						if (!found.empty()) {
							name = found;
						}
					} catch (...) {
					}
					logs::info("Action AttackTarget ({})", a_params);
					SKSE::GetTaskInterface()->AddTask([id = a_dog->GetFormID(), name] {
						if (const auto dog = RE::TESForm::LookupByID<RE::Actor>(id)) {
							Combat::Order(dog, name);
						}
					});
					return true;
				} },
			{ "Scavenge",
				"Use when the dog is hungry and smells food lying about nearby, on the floor, a table or a plate: he trots over, "
				"snaps it up and wolfs it down, whoever it belongs to.",
				[](RE::Actor* a_dog) { return g_looseFood && FoodReady(a_dog) && Calm(a_dog); },
				[](RE::Actor* a_dog) { return GroundSniff(a_dog, "scavenge", Scavenge); } },
			{ "Snatch",
				"Use when the dog is hungry and someone close by carries food: he sneaks up, noses into their pack and makes off "
				"with something to eat.",
				[](RE::Actor* a_dog) { return g_carriedFood && FoodReady(a_dog) && Calm(a_dog); },
				Snatch },
			{ "Hunt",
				"Use when the dog is hungry or restless and goes after game he can smell nearby: he takes off after it, runs it "
				"down and eats what he catches. Not for people, pets or farm animals.",
				[](RE::Actor* a_dog) {
					return PreyInReach() && !g_napping && !g_roaming && (AtLeast(a_dog, Drive::kHunger, kHuntHungry) || AtLeast(a_dog, Drive::kRestless, kPlayfulAt)) &&
					       Calm(a_dog) && !Hurt(a_dog);
				},
				[](RE::Actor* a_dog) { return AirSniff(a_dog, "hunt", Hunt); } },
			{ "Fish",
				"Use when the dog is hungry or restless and fish swim in the water close by (he's fond of salmon), or when someone "
				"points out fish for him to catch: he splashes in "
				"after one, swims it down, catches it in his jaws and eats it.",
				[](RE::Actor* a_dog) {
					return g_fishNear && FishCooled() && !g_napping && !g_posing && !g_roaming &&
					       (AtLeast(a_dog, Drive::kHunger, kHuntHungry) || AtLeast(a_dog, Drive::kRestless, kPlayfulAt)) && Calm(a_dog) && !Hurt(a_dog);
				},
				Fish },
			{ "EatCarcass",
				"Use when the dog is hungry and a carcass lies close by: he goes over, lies down to it, tears into it and eats his fill.",
				[](RE::Actor* a_dog) { return g_carcassNear && FoodReady(a_dog) && Calm(a_dog) && !Hurt(a_dog); },
				[](RE::Actor* a_dog) { return GroundSniff(a_dog, "eat carcass", EatCarcass); } },
			// The OAR poses as body language, so the model can pick the one that fits his mood. The lying ones aren't naps:
			// he stays awake (eyes open, head turning to follow), holds the pose a while, then gets up.
			{ "PlayBow",
				"Use when the dog is happy, excited or wants {{ player.name }} to play with him: he drops into a play bow, front end "
				"low with his forelegs stretched out wide, rump high, tail wagging hard, with an eager bark and a couple of whines.",
				[](RE::Actor* a_dog) {
					const auto last = g_lastBowTick.load();
					const bool cooled = last == 0 || Clock::now() - Clock::time_point(Clock::duration(last)) >= kBowAgain;
					return cooled && !g_napping && !g_posing && !g_roaming && Calm(a_dog) && !Hurt(a_dog);
				},
				[](RE::Actor* a_dog) { return PlayBow(a_dog, "dialogue"); } },
			{ "HappyWiggle",
				"Use when the dog is delighted: greeting {{ player.name }} or someone he loves, being praised or fussed over. "
				"He stands wagging so hard that his whole back end wiggles from side to side, mouth open in a happy pant.",
				[](RE::Actor* a_dog) {
					const auto last = g_lastWiggleTick.load();
					const bool cooled = last == 0 || Clock::now() - Clock::time_point(Clock::duration(last)) >= kWiggleAgain;
					return cooled && !g_napping && !g_posing && !g_roaming && Calm(a_dog) && !Hurt(a_dog);
				},
				[](RE::Actor* a_dog) { return Wiggle(a_dog, "dialogue"); } },
			{ "Rollover",
				"Use when the dog is playful, happy and relaxed, showing off or inviting fuss: he flops down on his side, rolls right "
				"over onto his back with his legs loose in the air, rolls on over and scrambles back to his feet.",
				[](RE::Actor* a_dog) {
					const auto last = g_lastRollTick.load();
					const bool cooled = last == 0 || Clock::now() - Clock::time_point(Clock::duration(last)) >= kRollAgain;
					return cooled && !g_napping && !g_posing && !g_roaming && !g_downed && Calm(a_dog) && !Hurt(a_dog) &&
					       !a_dog->AsActorState()->IsSwimming();
				},
				[](RE::Actor* a_dog) { return Rollover(a_dog, "dialogue"); } },
			{ "CuriousTilt",
				"Use when something has the dog's close attention: a sound or sight he's watching that puzzles him, or someone "
				"talking to him or near him whom he listens to intently, an odd or familiar word. He cocks his head to one side, "
				"then the other, ears up, still watching.",
				[](RE::Actor* a_dog) {
					const auto last = g_lastTiltTick.load();
					const bool cooled = last == 0 || Clock::now() - Clock::time_point(Clock::duration(last)) >= kTiltAgain;
					return cooled && !g_napping && !g_posing && !g_roaming && !g_downed && Calm(a_dog);
				},
				[](RE::Actor* a_dog) { return CuriousTilt(a_dog, "dialogue"); } },
			{ "SniffGround",
				"Use when a smell on the ground catches the dog's interest: tracks, a spill, a spot where something passed, or "
				"something down there he's watching. He puts his nose to the ground and works the scent back and forth, sniffing hard.",
				[](RE::Actor* a_dog) {
					const auto last = g_lastGroundSniffTick.load();
					const bool cooled = last == 0 || Clock::now() - Clock::time_point(Clock::duration(last)) >= kGroundSniffAgain;
					return cooled && !g_napping && !g_posing && !g_roaming && !g_downed && Calm(a_dog);
				},
				[](RE::Actor* a_dog) { return GroundSniff(a_dog, "dialogue"); } },
			{ "SniffAir",
				"Use when a scent on the breeze catches the dog's interest: game, smoke, cooking, a stranger or something he can't "
				"see yet. He lifts his nose and samples the air in three sniffs, each one higher, trying to place it.",
				[](RE::Actor* a_dog) {
					const auto last = g_lastAirSniffTick.load();
					const bool cooled = last == 0 || Clock::now() - Clock::time_point(Clock::duration(last)) >= kAirSniffAgain;
					return cooled && !g_napping && !g_posing && !g_roaming && !g_downed && Calm(a_dog);
				},
				[](RE::Actor* a_dog) { return AirSniff(a_dog, "dialogue"); } },
			{ "Sneeze",
				"Use when something tickles the dog's nose: dust, pollen, smoke, pepper, a sharp smell, or when he's playful and "
				"gives a little play sneeze. He sneezes once, sharply, gives his head a quick shake and carries on.",
				[](RE::Actor* a_dog) {
					const auto last = g_lastSneezeTick.load();
					const bool cooled = last == 0 || Clock::now() - Clock::time_point(Clock::duration(last)) >= kSneezeAgain;
					return cooled && !g_napping && !g_posing && !g_roaming && !g_downed && Calm(a_dog);
				},
				[](RE::Actor* a_dog) { return Sneeze(a_dog, "dialogue"); } },
			{ "HappyBark",
				"Use when the dog barks for joy: greeting someone he loves, excited about a game or a walk, praised, or asking for fun.",
				[](RE::Actor*) { return VoiceCooled(0) && !g_napping && !g_posing && !g_roaming; },
				[](RE::Actor* a_dog) { return Voice(a_dog, Vocal::kHappyBark, "dialogue"); } },
			{ "AngryBark",
				"Use when the dog barks in anger or alarm: at a threat, an intruder, a stranger he distrusts, or someone menacing "
				"{{ player.name }}. Sharp, hostile barks.",
				[](RE::Actor*) { return VoiceCooled(1) && !g_napping && !g_posing && !g_roaming; },
				[](RE::Actor* a_dog) { return Voice(a_dog, Vocal::kAngryBark, "dialogue"); } },
			{ "WarningGrowl",
				"Use when the dog warns someone or something off: he stiffens, lowers his head and growls low, a clear sign he will "
				"defend himself or {{ player.name }}.",
				[](RE::Actor*) { return VoiceCooled(2) && !g_napping && !g_roaming; },
				[](RE::Actor* a_dog) { return Voice(a_dog, Vocal::kGrowl, "dialogue"); } },
			{ "PoutingWhine",
				"Use when the dog sulks or pouts: denied a treat, ignored, scolded, left out, or angling for attention, he lets out a "
				"long, put-upon whine.",
				[](RE::Actor*) { return VoiceCooled(3) && !g_napping; },
				[](RE::Actor* a_dog) { return Voice(a_dog, Vocal::kPout, "dialogue"); } },
			{ "SadWhine",
				"Use when the dog is sad, lonely, frightened or hurt: a thin, pained whine.",
				[](RE::Actor*) { return VoiceCooled(4) && !g_napping; },
				[](RE::Actor* a_dog) { return Voice(a_dog, Vocal::kSad, "dialogue"); } },
			{ "LieOnSide",
				"Use when the dog is content and at ease and flops down to lounge: he lies down and rolls onto his side, legs "
				"stretched out, belly bared. He stays awake and gets up again after a while. Not a nap.",
				[](RE::Actor* a_dog) { return !g_napping && !g_posing && !g_roaming && !g_downed && Calm(a_dog); },
				[](RE::Actor* a_dog) { return StartPose(a_dog, Sleep::Style::kRelax); } },
			{ "RestHeadOnPaws",
				"Use when the dog is bored, sulking, waiting patiently or settling in to watch: he lies down with his chin on his "
				"front paws, eyes open, following what goes on. He gets up again after a while. Not a nap.",
				[](RE::Actor* a_dog) { return !g_napping && !g_posing && !g_roaming && !g_downed && Calm(a_dog); },
				[](RE::Actor* a_dog) { return StartPose(a_dog, Sleep::Style::kGuarded); } },
			{ "CurlUp",
				"Use when the dog is cold, sad, hurt in his feelings or wants to withdraw: he lies down and curls up tight, tail "
				"wrapped over his nose, still awake. He gets up again after a while. Not a nap.",
				[](RE::Actor* a_dog) { return !g_napping && !g_posing && !g_roaming && !g_downed && Calm(a_dog); },
				[](RE::Actor* a_dog) { return StartPose(a_dog, Sleep::Style::kCold); } },
		};
	}

	bool Perform(RE::Actor* a_dog, std::string_view a_behavior)
	{
		if (a_behavior == "nap") {
			return Nap(a_dog);
		}
		if (a_behavior == "seek_affection") {
			return SeekAffection(a_dog);
		}
		if (a_behavior == "play") {
			return Play(a_dog);
		}
		if (a_behavior == "sniff") {
			return Sniff(a_dog);
		}
		if (a_behavior == "scavenge") {
			return g_looseFood ? GroundSniff(a_dog, "scavenge", Scavenge) : Scavenge(a_dog);
		}
		if (a_behavior == "sing") {
			return SingReady(a_dog) && Sing(a_dog);
		}
		if (a_behavior == "hunt") {
			return PreyInReach() && AirSniff(a_dog, "hunt", Hunt);
		}
		if (a_behavior == "eat_carcass") {
			return g_carcassNear ? GroundSniff(a_dog, "eat carcass", EatCarcass) : EatCarcass(a_dog);
		}
		if (a_behavior == "fish") {
			return g_fishNear && FishCooled() && Fish(a_dog);
		}
		if (a_behavior == "beg" && a_dog) {
			// Like AskForFood without the gift menu: he asks, and the player answers if they like.
			Training::Did("beg");
			Idles::Play(a_dog, Idles::kDogSit, "idleSitStart");
			SoundLater(a_dog->GetFormID(), 800ms, kWhine);
			return true;
		}
		return false;
	}

	namespace
	{
		bool Sitting(RE::Actor* a_dog);  // below, with StandUpFirst
	}
	void UpdateStandFirst(RE::Actor* a_dog);  // below, with PlayBow and Wiggle
	void UpdateSniffFirst(RE::Actor* a_dog);  // below, with GroundSniff

	void Process(RE::Actor* a_dog, bool, const Settings& a_settings)
	{
		ProcessRoam(a_dog);
		UpdateGreet(a_dog, a_settings);
		UpdateWarm(a_dog);
		ScanFood(a_dog);
		ScanHunt(a_dog);
		ScanPetters(a_dog);
		UpdateDowned(a_dog);
		UpdatePlayBow(a_dog);
		UpdatePose(a_dog);
		UpdateStandFirst(a_dog);
		UpdateSniffFirst(a_dog);
		if (!g_napping) {
			return;
		}
		const auto  player = RE::PlayerCharacter::GetSingleton();
		const auto  now = Clock::now();
		const char* why = nullptr;
		if (g_layPending && a_dog) {
			const auto running = a_dog->GetCurrentPackage() == WaitPackage();
			if (running || now - g_napStarted >= kHoldWait) {
				g_layPending = false;
				g_napStarted = g_lastLayDown = g_graceFrom = now;  // the nap's clocks start with the lie-down
				Idles::Play(a_dog, Idles::kDogLay, "idleLayStart");
				logs::info("Nap: {} lying down", running ? "wait package running," : "no wait package after 3 s,");
			}
			return;
		}
		// Since the latest lie-down began. isIdleLay isn't steady: a talk bark while he lies (before his AI pauses)
		// drops it for seconds while he visibly stays down, so once the lie-down has registered he counts as down.
		if (IsLying(a_dog) && (!g_downSince || *g_downSince < g_lastLayDown)) {
			g_downSince = now;
		}
		const bool down = g_downSince && *g_downSince >= g_lastLayDown;
		g_lying = down;
		if (!a_dog || a_dog->IsDead() || !player) {
			why = "gone";
		} else if (a_dog->IsInCombat() || player->IsInCombat()) {
			why = "a fight";
		} else if (player->GetPosition().GetDistance(g_napPlayerPos) > kPlayerMovedOn) {
			why = "you moved on";
		} else if (Drives::Value(Drive::kTired) <= kRestedAt) {
			why = "rested";
		} else if (now - g_napStarted >= kMaxNap) {
			why = "time limit";
		} else if ((down || g_settled) && Sitting(a_dog)) {
			// Sitting, not lying: "down" sticks once the lie-down registered, so a sit idle afterwards went unseen
			// (2026-11-15: in Sever's camp, whose sandbox package keeps the wait package off, he sat up just as the
			// lie-down counted and his AI paused with him asleep sitting up). Lie him down again, AI on for it.
			g_lying = false;
			if (g_relays >= kMaxRelays) {
				why = "he won't settle";
			} else {
				if (g_aiPaused == a_dog->GetFormID()) {
					a_dog->EnableAI(true);
					g_aiPaused = 0;
				}
				const bool wasSettled = g_settled != 0;
				g_settled = 0;
				g_downSince.reset();
				g_upSince.reset();
				++g_relays;
				g_lastLayDown = now;
				Idles::Play(a_dog, Idles::kDogLay, "idleLayStart");
				logs::info("Nap: he's sitting, not lying{}; lying down again ({}/{})", wasSettled ? " (after settling, AI back on)" : "", g_relays,
					kMaxRelays);
			}
		} else if (down || g_settled) {
			g_upSince.reset();
			// Once he's settled (the graph says "lying" as soon as the lie-down starts, so wait a little), the nap
			// counts as sleep, its narration goes out, and his AI pauses. With it on, something outside his idles (a
			// talk bark sent straight to his graph, another mod's doing) still barked and stood him up within seconds,
			// with any package. The wait package has held him since the nap began, so nothing moves him before this.
			if (!g_settled && down && now - *g_downSince >= kDownBeforePause && now - g_lastLayDown >= kDownBeforePause) {
				g_settled = a_dog->GetFormID();
				g_asleepSince = now;
				if (a_dog->IsAIEnabled()) {
					a_dog->EnableAI(false);
					g_aiPaused = a_dog->GetFormID();
				}
				logs::info("Nap: he's lying down, settled, AI paused");
				if (g_settledLine) {
					const auto s = SnapshotSettings();
					Bridge::PostEvent(std::move(*g_settledLine), a_dog->GetFormID(), 0, s.narrate);
					g_settledLine.reset();
					g_graceFrom = now;  // his sleepy reaction to it mustn't wake him
				}
			}
		} else if (now - g_lastLayDown >= kSettleTime) {
			// Up again while the player is still here (his follow AI picked a new idle, or an animation
			// variant ended standing). A brief shuffle he settles from himself is left alone; once he has
			// stayed up a while, he lies back down, a couple of times at most, rather than fight the AI.
			if (!g_upSince) {
				g_upSince = now;
			} else if (now - *g_upSince >= kUpBeforeRelay) {
				if (g_relays >= kMaxRelays) {
					why = "he won't settle";
				} else {
					++g_relays;
					g_lastLayDown = now;
					g_upSince.reset();
					Idles::Play(a_dog, Idles::kDogLay, "idleLayStart");
					logs::info("Nap: he got up, lying back down ({}/{})", g_relays, kMaxRelays);
				}
			}
		}
		if (why) {
			EndNap(why, why != std::string_view("a fight") && why != std::string_view("gone"));
		}
	}

	namespace
	{
		constexpr RE::FormID kDogSitIdleStop = 0x0960E2;  // Skyrim.esm DogSitIdleStop (Idle_Out): up from a sit
		// The get-up is ignored while one of his random sit idles plays (2026-11-14: about half the requests), so it's asked
		// again every second while he still sits.
		constexpr auto       kStandWait = 6000ms;        // he isn't up by then: dropped
		constexpr auto       kStandAskAgain = 1000ms;
		constexpr auto       kStandSettle = 400ms;       // up this long first (the flag drops as the get-up starts)

		// A play bow or wiggle asked for while he sat: he stands first, then it plays (UpdateStandFirst).
		enum class Clip { kWiggle, kBow, kRoll };
		const char* ClipName(Clip a_clip)
		{
			return a_clip == Clip::kBow ? "Play bow" : a_clip == Clip::kRoll ? "Rollover" : "Wiggle";
		}

		struct StandFirst
		{
			RE::FormID                       dog = 0;
			Clip                             clip = Clip::kWiggle;
			std::string                      why;
			Clock::time_point                until{};
			std::optional<Clock::time_point> upSince;
			Clock::time_point                asked{};  // the last get-up request
		};
		std::optional<StandFirst> g_standFirst;

		bool Sitting(RE::Actor* a_dog)
		{
			std::int32_t sitting = 0;
			return a_dog->GetGraphVariableInt("isIdleSitting", sitting) && sitting;
		}

		// From a sit: the vanilla get-up, and the clip waits for it. True when it's queued.
		bool StandUpFirst(RE::Actor* a_dog, Clip a_clip, const char* a_why)
		{
			if (g_standFirst) {
				logs::info("{} ({}): he's already getting up for one", ClipName(a_clip), a_why);
				return false;
			}
			Idles::Play(a_dog, kDogSitIdleStop, "Idle_Out");
			g_standFirst = StandFirst{ a_dog->GetFormID(), a_clip, a_why, Clock::now() + kStandWait, std::nullopt, Clock::now() };
			logs::info("{} ({}): he's sitting, standing him up first", ClipName(a_clip), a_why);
			return true;
		}

		bool StartPlayBow(RE::Actor* a_dog, const char* a_why)
		{
			if (!Mood::PlayBow(a_dog, kBowClip)) {  // the code first: OAR picks the clip as the idle starts
				return false;
			}
			Idles::Play(a_dog, kDogIdleWarn, "aggroWarning2Start");
			g_lastBow = Clock::now();
			g_lastBowTick = g_lastBow->time_since_epoch().count();
			const auto s = SnapshotSettings();
			Drives::Adjust(Drive::kRestless, -kBowRelief, s);
			if (s.events) {
				Bridge::PostEvent(std::format("{} drops into a play bow, chest low and rump in the air, tail wagging hard.", a_dog->GetDisplayFullName()),
					a_dog->GetFormID(), 0);
			}
			logs::info("Play bow ({}; restless -> {:.1f})", a_why, Drives::Value(Drive::kRestless));
			return true;
		}

		bool StartWiggle(RE::Actor* a_dog, const char* a_why)
		{
			if (!Mood::Wiggle(a_dog, kWiggleClip)) {  // the code first: OAR picks the clip as the idle starts
				return false;
			}
			Idles::Play(a_dog, kDogIdleWarn, "aggroWarning2Start");
			g_lastWiggleTick = Clock::now().time_since_epoch().count();
			if (SnapshotSettings().events) {
				Bridge::PostEvent(std::format("{} wags so hard his whole back end wiggles from side to side, mouth open in a happy pant.",
					a_dog->GetDisplayFullName()), a_dog->GetFormID(), 0);
			}
			logs::info("Wiggle ({})", a_why);
			return true;
		}

		bool StartRollover(RE::Actor* a_dog, const char* a_why)
		{
			if (!Mood::Rollover(a_dog, kRollClip)) {  // the code first: OAR picks the clip as the idle starts
				return false;
			}
			Idles::Play(a_dog, kDogIdleWarn, "aggroWarning2Start");
			g_lastRollTick = Clock::now().time_since_epoch().count();
			const auto s = SnapshotSettings();
			Drives::Adjust(Drive::kRestless, -kRollRelief, s);
			if (s.events) {
				Bridge::PostEvent(std::format("{} flops down onto his side and rolls right over onto his back, legs loose in the air, "
											  "then on over and scrambles back to his feet.", a_dog->GetDisplayFullName()),
					a_dog->GetFormID(), 0);
			}
			logs::info("Rollover ({}; restless -> {:.1f})", a_why, Drives::Value(Drive::kRestless));
			return true;
		}

		bool StartClip(RE::Actor* a_dog, Clip a_clip, const char* a_why)
		{
			return a_clip == Clip::kBow ? StartPlayBow(a_dog, a_why) : a_clip == Clip::kRoll ? StartRollover(a_dog, a_why) :
			                                                                                   StartWiggle(a_dog, a_why);
		}
	}

	void UpdateStandFirst(RE::Actor* a_dog)
	{
		if (!g_standFirst) {
			return;
		}
		auto&       q = *g_standFirst;
		const auto  now = Clock::now();
		const char* what = ClipName(q.clip);
		if (!a_dog || a_dog->GetFormID() != q.dog || a_dog->IsDead() || !a_dog->Is3DLoaded() || g_napping || g_posing || g_downed ||
			a_dog->IsInCombat()) {
			logs::info("{} ({}): dropped while he got up", what, q.why);
			g_standFirst.reset();
			return;
		}
		if (!Sitting(a_dog) && !IsLying(a_dog)) {
			if (!q.upSince) {
				q.upSince = now;
			} else if (now - *q.upSince >= kStandSettle) {
				const auto job = std::move(q);
				g_standFirst.reset();
				logs::info("{} ({}): he's up", what, job.why);
				StartClip(a_dog, job.clip, job.why.c_str());
				return;
			}
		} else {
			q.upSince.reset();
			if (now - q.asked >= kStandAskAgain) {  // a sit can slide into lying down: ask for whichever he's in
				const bool sat = Sitting(a_dog);
				sat ? Idles::Play(a_dog, kDogSitIdleStop, "Idle_Out") : Idles::Play(a_dog, kDogIdleStop, "IdleStop");
				q.asked = now;
				logs::info("{} ({}): still {}, asked him to get up again", what, q.why, sat ? "sitting" : "lying down");
			}
		}
		if (now >= q.until) {
			const auto job = std::move(q);
			g_standFirst.reset();
			logs::info("{} ({}): he didn't get up in time, dropped", what, job.why);
			if (job.clip == Clip::kWiggle && job.why == "petted") {
				Bark(a_dog, true);  // the happy bark he'd have given anyway
			}
		}
	}

	bool PlayBow(RE::Actor* a_dog, const char* a_why)
	{
		if (!a_dog || !a_dog->Is3DLoaded() || a_dog->IsDead() || g_napping || g_posing || IsLying(a_dog)) {
			logs::info("Play bow ({}): not now (napping, lying or not loaded)", a_why);
			return false;
		}
		return Sitting(a_dog) ? StandUpFirst(a_dog, Clip::kBow, a_why) : StartPlayBow(a_dog, a_why);
	}

	bool Wiggle(RE::Actor* a_dog, const char* a_why)
	{
		if (!a_dog || !a_dog->Is3DLoaded() || a_dog->IsDead() || g_napping || g_posing || g_downed || IsLying(a_dog)) {
			logs::info("Wiggle ({}): not now (napping, lying, down or not loaded)", a_why);
			return false;
		}
		const auto now = Clock::now();
		if (const auto last = g_lastWiggleTick.load(); last && now - Clock::time_point(Clock::duration(last)) < kWiggleAgain) {
			logs::info("Wiggle ({}): not again so soon", a_why);
			return false;
		}
		return Sitting(a_dog) ? StandUpFirst(a_dog, Clip::kWiggle, a_why) : StartWiggle(a_dog, a_why);
	}

	bool Rollover(RE::Actor* a_dog, const char* a_why)
	{
		if (!a_dog || !a_dog->Is3DLoaded() || a_dog->IsDead() || g_napping || g_posing || g_downed || IsLying(a_dog) ||
			a_dog->IsInCombat() || a_dog->AsActorState()->IsSwimming()) {
			logs::info("Rollover ({}): not now (napping, lying, down, fighting, swimming or not loaded)", a_why);
			return false;
		}
		if (const auto last = g_lastRollTick.load(); last && Clock::now() - Clock::time_point(Clock::duration(last)) < kRollAgain) {
			logs::info("Rollover ({}): not again so soon", a_why);
			return false;
		}
		return Sitting(a_dog) ? StandUpFirst(a_dog, Clip::kRoll, a_why) : StartRollover(a_dog, a_why);
	}

	bool CuriousTilt(RE::Actor* a_dog, const char* a_why)
	{
		if (!a_dog || !a_dog->Is3DLoaded() || a_dog->IsDead() || g_napping || g_posing || g_downed || IsLying(a_dog) ||
			a_dog->IsInCombat()) {
			logs::info("Curious tilt ({}): not now (napping, lying, down, fighting or not loaded)", a_why);
			return false;
		}
		const auto now = Clock::now();
		if (const auto last = g_lastTiltTick.load(); last && now - Clock::time_point(Clock::duration(last)) < kTiltAgain) {
			logs::info("Curious tilt ({}): not again so soon", a_why);
			return false;
		}
		if (!Mood::CuriousTilt(a_dog, kTiltClip)) {  // the code first: OAR picks the clip as the idle starts
			logs::info("Curious tilt ({}): not now (the animation code is held)", a_why);
			return false;
		}
		// Sitting, the talk bark is a head layer over the sit idle, so he stays seated; standing, it's a full-body clip.
		const bool sitting = Sitting(a_dog);
		const bool played = sitting ? Idles::Play(a_dog, Idles::kDogBarkSit, "idleDogSitBarkStart")
		                            : Idles::Play(a_dog, Idles::kDogBarkStand, "idleDogBarkStart");
		g_lastTiltTick = now.time_since_epoch().count();
		if (SnapshotSettings().events) {
			Bridge::PostEvent(std::format("{} cocks his head to one side, then the other, ears up, watching intently.", a_dog->GetDisplayFullName()),
				a_dog->GetFormID(), 0);
		}
		logs::info("Curious tilt ({}): {}{}", a_why, sitting ? "sitting" : "standing", played ? "" : " (the idle didn't start)");
		return true;
	}

	bool Voice(RE::Actor* a_dog, Vocal a_kind, const char* a_why)
	{
		static constexpr std::array kNames{ "Happy bark", "Angry bark", "Warning growl", "Pouting whine", "Sad whine" };
		const auto  k = static_cast<int>(a_kind);
		const char* name = kNames[k];
		if (!a_dog || !a_dog->Is3DLoaded() || a_dog->IsDead() || g_napping) {
			logs::info("{} ({}): not now (napping or not loaded)", name, a_why);
			return false;
		}
		if (!VoiceCooled(k)) {
			logs::info("{} ({}): not again so soon", name, a_why);
			return false;
		}
		const bool sitting = Sitting(a_dog);
		const bool lying = IsLying(a_dog);
		const char* how = "";
		switch (a_kind) {
		case Vocal::kHappyBark:
			if (lying) {
				SoundCues::Play(a_dog, SoundCues::Cue::kBark);  // the lying bark would stand him up: the sound alone
				how = " (lying: the sound alone)";
			} else {
				Bark(a_dog, sitting);  // the deliberate bark (code 20+): his happy bark markers
			}
			break;
		case Vocal::kAngryBark:
			if (lying || !Mood::AngryBark(a_dog, kAngryBarkClip)) {
				SoundCues::Play(a_dog, SoundCues::Cue::kBarkAngry);
				how = " (the sound alone)";
			} else {
				const auto process = a_dog->GetActorRuntimeData().currentProcess;
				const auto sitBark = RE::TESForm::LookupByID<RE::TESIdleForm>(Idles::kDogBarkSit);
				if (!(sitting && process && sitBark && process->PlayIdle(a_dog, sitBark, nullptr))) {
					Idles::Play(a_dog, Idles::kDogBarkStand, "idleDogBarkStart");
				}
			}
			break;
		case Vocal::kGrowl:
			if (sitting || lying || g_posing) {
				SoundCues::Play(a_dog, SoundCues::Cue::kGrowl);  // the warning stance would stand him up: the growl alone
				how = " (the growl alone)";
			} else {
				Idles::Play(a_dog, kDogIdleWarn, "aggroWarning2Start");  // the warning stance; his voice clip growls
			}
			break;
		case Vocal::kPout:
			SoundCues::Play(a_dog, SoundCues::Cue::kWhinePout);
			break;
		case Vocal::kSad:
			SoundCues::Play(a_dog, SoundCues::Cue::kWhineSad);
			break;
		}
		g_lastVoiceTick[k] = Clock::now().time_since_epoch().count();
		if (SnapshotSettings().events) {
			static constexpr std::array kLines{ "barks happily", "barks angrily, sharp and hostile", "stiffens and growls low, a warning",
				"lets out a long, put-upon whine", "whines, thin and pained" };
			Bridge::PostEvent(std::format("{} {}.", a_dog->GetDisplayFullName(), kLines[k]), a_dog->GetFormID(), 0);
		}
		logs::info("{} ({}){}", name, a_why, how);
		return true;
	}

	namespace
	{
		// A sniff or sneeze under way: waiting for him to get up from a sit, then playing; a food run or hunt may follow.
		enum class Lead
		{
			kGround,  // code 46
			kAir,     // code 47
			kSneeze   // code 48
		};

		struct SniffFirst
		{
			RE::FormID                       dog = 0;
			bool                             (*then)(RE::Actor*) = nullptr;
			std::string                      why;
			bool                             standing = false;  // still getting up from a sit
			Lead                             kind = Lead::kGround;
			Clock::time_point                until{};
			std::optional<Clock::time_point> upSince;
			Clock::time_point                asked{};  // the last get-up request
		};
		std::optional<SniffFirst> g_sniffFirst;

		const char* LeadName(Lead a_kind) { return a_kind == Lead::kAir ? "Air sniff" : a_kind == Lead::kSneeze ? "Sneeze" : "Ground sniff"; }
		const char* SniffName(const SniffFirst& a_job) { return LeadName(a_job.kind); }

		std::atomic<Clock::rep>& LastTick(Lead a_kind)
		{
			return a_kind == Lead::kAir ? g_lastAirSniffTick : a_kind == Lead::kSneeze ? g_lastSneezeTick : g_lastGroundSniffTick;
		}

		Clock::duration Again(Lead a_kind)
		{
			return a_kind == Lead::kAir ? Clock::duration(kAirSniffAgain) : a_kind == Lead::kSneeze ? Clock::duration(kSneezeAgain) :
			                                                                                     Clock::duration(kGroundSniffAgain);
		}

		bool StartGroundSniff(RE::Actor* a_dog, SniffFirst& a_job)
		{
			// the code first: OAR picks the clip as the idle starts
			const bool coded = a_job.kind == Lead::kAir    ? Mood::AirSniff(a_dog, kAirSniffClip) :
			                   a_job.kind == Lead::kSneeze ? Mood::Sneeze(a_dog, kSneezeClip) :
			                                                 Mood::GroundSniff(a_dog, kGroundSniffClip);
			if (!coded) {
				return false;
			}
			Idles::Play(a_dog, Idles::kDogBarkStand, "idleDogBarkStart");
			if (a_job.kind == Lead::kSneeze) {
				Canine::SoundCues::ExpectSneeze(a_dog);  // the sound at the clip's marker, or on a timer if the marker never comes
			}
			const auto now = Clock::now();
			LastTick(a_job.kind) = now.time_since_epoch().count();
			switch (a_job.kind) {
			case Lead::kAir:
				a_job.until = now + (a_job.then ? kAirSniffRun : kAirSniffClip);
				break;
			case Lead::kSneeze:
				a_job.until = now + kSneezeClip;
				break;
			default:
				a_job.until = now + (a_job.then ? kGroundSniffRun : kGroundSniffClip);
				break;
			}
			if (SnapshotSettings().events) {
				const auto name = a_dog->GetDisplayFullName();
				Bridge::PostEvent(a_job.kind == Lead::kAir    ? std::format("{} lifts his nose and samples the air, each sniff higher, trying to place a scent.", name) :
				                  a_job.kind == Lead::kSneeze ? std::format("{} sneezes, sharp and sudden, and gives his head a quick shake.", name) :
				                                                std::format("{} puts his nose to the ground and works a scent back and forth, sniffing hard.", name),
					a_dog->GetFormID(), 0);
			}
			logs::info("{} ({}){}", SniffName(a_job), a_job.why,
				a_job.then ? (a_job.kind == Lead::kAir ? ": the hunt follows" : ": the food run follows") : "");
			return true;
		}

		bool SniffLead(RE::Actor* a_dog, const char* a_why, bool (*a_then)(RE::Actor*), Lead a_kind)
		{
			const char* name = LeadName(a_kind);
			const bool  can = a_dog && a_dog->Is3DLoaded() && !a_dog->IsDead() && !g_napping && !g_posing && !g_downed && !g_roam &&
			                 !g_sniffFirst && !IsLying(a_dog) && !a_dog->IsInCombat();
			if (!can) {
				if (a_then) {
					return a_then(a_dog);  // no sniff now: straight to the food
				}
				logs::info("{} ({}): not now (napping, lying, down, busy or not loaded)", name, a_why);
				return false;
			}
			if (!a_then) {
				const auto last = LastTick(a_kind).load();
				if (last && Clock::now() - Clock::time_point(Clock::duration(last)) < Again(a_kind)) {
					logs::info("{} ({}): not again so soon", name, a_why);
					return false;
				}
			}
			SniffFirst job{ a_dog->GetFormID(), a_then, a_why };
			job.kind = a_kind;
			if (Sitting(a_dog)) {
				Idles::Play(a_dog, kDogSitIdleStop, "Idle_Out");
				job.standing = true;
				job.until = Clock::now() + kStandWait;
				job.asked = Clock::now();
				g_sniffFirst = std::move(job);
				logs::info("{} ({}): he's sitting, standing him up first", name, a_why);
				return true;
			}
			if (!StartGroundSniff(a_dog, job)) {
				return a_then ? a_then(a_dog) : false;
			}
			g_sniffFirst = std::move(job);
			return true;
		}
	}

	bool GroundSniff(RE::Actor* a_dog, const char* a_why, bool (*a_then)(RE::Actor*))
	{
		return SniffLead(a_dog, a_why, a_then, Lead::kGround);
	}

	bool AirSniff(RE::Actor* a_dog, const char* a_why, bool (*a_then)(RE::Actor*))
	{
		return SniffLead(a_dog, a_why, a_then, Lead::kAir);
	}

	bool Sneeze(RE::Actor* a_dog, const char* a_why)
	{
		return SniffLead(a_dog, a_why, nullptr, Lead::kSneeze);
	}

	void UpdateSniffFirst(RE::Actor* a_dog)
	{
		if (!g_sniffFirst) {
			return;
		}
		auto&      q = *g_sniffFirst;
		const auto now = Clock::now();
		if (!a_dog || a_dog->GetFormID() != q.dog || a_dog->IsDead() || !a_dog->Is3DLoaded() || g_napping || g_posing || g_downed ||
			a_dog->IsInCombat()) {
			logs::info("{} ({}): dropped", SniffName(q), q.why);
			g_sniffFirst.reset();
			return;
		}
		if (q.standing) {
			if (!Sitting(a_dog) && !IsLying(a_dog)) {
				if (!q.upSince) {
					q.upSince = now;
				} else if (now - *q.upSince >= kStandSettle) {
					q.standing = false;
					if (!StartGroundSniff(a_dog, q)) {
						const auto job = std::move(q);
						g_sniffFirst.reset();
						if (job.then) {
							job.then(a_dog);
						}
					}
					return;
				}
			} else {
				q.upSince.reset();
				if (now - q.asked >= kStandAskAgain) {  // a sit can slide into lying down: ask for whichever he's in
					const bool sat = Sitting(a_dog);
					sat ? Idles::Play(a_dog, kDogSitIdleStop, "Idle_Out") : Idles::Play(a_dog, kDogIdleStop, "IdleStop");
					q.asked = now;
					logs::info("{} ({}): still {}, asked him to get up again", SniffName(q), q.why, sat ? "sitting" : "lying down");
				}
			}
			if (now >= q.until) {
				const auto job = std::move(q);
				g_sniffFirst.reset();
				logs::info("{} ({}): he didn't get up in time", SniffName(job), job.why);
				if (job.then) {
					job.then(a_dog);
				}
			}
			return;
		}
		if (now >= q.until) {
			const auto job = std::move(q);
			g_sniffFirst.reset();
			if (job.then) {
				logs::info("{} ({}): off after it", SniffName(job), job.why);
				job.then(a_dog);
			} else if (job.kind == Lead::kGround && std::uniform_int_distribution<int>(1, 100)(Random()) <= kSneezeAfterSniff) {
				// a plain ground sniff now and then gets up his nose
				Sneeze(a_dog, "after the ground sniff");
			}
		}
	}

	void Wake(const char* a_why, bool a_speech)
	{
		// The line he speaks while choosing Nap in a conversation arrives just after the nap begins.
		if (g_napping && !(a_speech && Clock::now() - g_graceFrom < kSpeechGrace)) {
			EndNap(a_why);
		}
	}

	void ResumeAfterLoad(RE::Actor* a_dog)
	{
		// A save made mid-roam might keep his offset or the play package (PapyrusUtil saves its overrides); this session
		// no longer knows about either.
		ClearOffset(a_dog);
		SleepIdles(nullptr, false);  // the idle forms outlive a load: a nap's hold on them would stay
		if (a_dog) {
			PackageOverride(a_dog, false);
			PackageOverride(a_dog, false, WaitPackage());  // a save made mid-nap keeps the wait override
			if (const auto keyword = TravelTarget(); keyword && a_dog->extraList.GetLinkedRef(keyword)) {
				// Only ours points at a bare XMarker; SeverActions' own travel uses real destinations.
				if (const auto linked = a_dog->extraList.GetLinkedRef(keyword); linked->GetBaseObject() && linked->GetBaseObject()->GetFormID() == kXMarker) {
					a_dog->extraList.SetLinkedRef(nullptr, keyword);
				}
			}
		}
		// A save made mid-nap keeps his AI off, and this session no longer knows it paused him.
		if (a_dog && !a_dog->IsAIEnabled()) {
			a_dog->EnableAI(true);
			logs::info("His AI was off after loading (a save made mid-nap): back on");
		}
	}

	bool IsLying(RE::Actor* a_dog)
	{
		std::int32_t lying = 0;
		return a_dog && a_dog->GetGraphVariableInt("isIdleLay", lying) && lying == 1;
	}

	bool Napping()
	{
		return g_napping && g_lying;
	}

	bool NapOn()
	{
		return g_napping;
	}

	bool PoseOn()
	{
		return g_posing;
	}

	bool Pose(RE::Actor* a_dog, Sleep::Style a_style)
	{
		return StartPose(a_dog, a_style);
	}

	int FrostfallExposure()
	{
		return ExposureLevel();
	}

	bool Roaming()
	{
		return g_roaming;
	}

	std::uint32_t RoamActivity()
	{
		if (!g_roam) {
			return CDAPI_ACTIVITY_UNKNOWN;
		}
		switch (g_roam->kind) {
		case RoamKind::kPlay:
			return CDAPI_ACTIVITY_PLAYING;
		case RoamKind::kSniff:
			return CDAPI_ACTIVITY_SNIFFING;
		case RoamKind::kGreet:
			return CDAPI_ACTIVITY_GREETING;
		case RoamKind::kWarm:
			return CDAPI_ACTIVITY_WARMING;
		case RoamKind::kScavenge:
		case RoamKind::kSnatch:
			return CDAPI_ACTIVITY_SCAVENGING;
		case RoamKind::kHunt:
			return CDAPI_ACTIVITY_HUNTING;
		case RoamKind::kCarcass:
			return g_roam->eating ? CDAPI_ACTIVITY_EATING : CDAPI_ACTIVITY_SCAVENGING;
		case RoamKind::kFish:
			return CDAPI_ACTIVITY_FISHING;
		}
		return CDAPI_ACTIVITY_UNKNOWN;
	}

	bool RoamHoldsStill()
	{
		return g_roaming && g_roamStill;
	}

	void TellWhenSettled(std::string a_line)
	{
		if (g_napping) {
			g_settledLine = std::move(a_line);
		}
	}

	void HeardDogLine(int a_voicedWords)
	{
		g_dogLineWords = a_voicedWords;
		g_dogLineAt = Clock::now().time_since_epoch().count();
	}

	namespace
	{
		std::atomic_int g_quietLevel{ 0 };  // what QuietIdleBarks last applied; Bark reads it
	}

	void QuietIdleBarks(RE::Actor* a_dog, int a_level)
	{
		// Vanilla dog bark idles, each with the quiet level from which it is off for him: 1 the ones the AI picks at
		// random, 2 also the talking barks played while he speaks (combat barks always stay). An extra first condition
		// "is not this dog" leaves him out of an idle. In memory only (nothing saved, nothing in a plugin); switched
		// back by unlinking the same items.
		struct BarkIdle
		{
			RE::FormID id;
			int        from;
		};
		constexpr std::array<BarkIdle, 7> kBarkIdles{ {
			{ 0x0005A083, 1 },  // Skyrim.esm DogBarkStanding
			{ 0x00081171, 1 },  // DogBarkRunIdle
			{ 0x0010F800, 1 },  // IdleDogBarkStanding
			{ 0x000BEE73, 2 },  // DogBarkTalk (the talking branch)
			{ 0x000BF207, 2 },  // DogBarkStandingTalk
			{ 0x000BF209, 2 },  // DogBarkSittingTalk
			{ 0x000BF208, 2 },  // DogBarkLayTalk
		} };
		static std::array<RE::TESConditionItem*, kBarkIdles.size()> added{};
		static RE::TESNPC* forBase = nullptr;
		static int         applied = 0;

		const auto base = a_dog ? a_dog->GetActorBase() : nullptr;
		const int  level = base ? std::clamp(a_level, 0, 2) : 0;
		if (level == applied && (level == 0 || forBase == base)) {
			return;
		}
		for (std::size_t i = 0; i < kBarkIdles.size(); ++i) {
			const auto idle = RE::TESForm::LookupByID<RE::TESIdleForm>(kBarkIdles[i].id);
			if (!idle) {
				continue;
			}
			if (added[i]) {  // take ours out (the nap's may sit above it on DogBarkTalk)
				Unlink(idle, added[i]);
				added[i] = nullptr;
			}
			if (level >= kBarkIdles[i].from) {
				added[i] = LeaveOut(idle, base);
			}
		}
		forBase = level ? base : nullptr;
		applied = level;
		g_quietLevel = level;
		logs::info("Quiet barks: {}", level == 2 ? "all his bark idles are off (combat barks stay)" : level == 1 ? "his random bark idles are off" : "off");
	}

	void Bark(RE::Actor* a_dog, bool a_sitting)
	{
		if (!a_dog || !a_dog->Is3DLoaded()) {
			return;
		}
		if (g_quietLevel >= 2) {
			PlaySound(a_dog, kBarkSound);  // the bark idles are off for him: the sound alone
		} else {
			Mood::AllowBark(a_dog, 3s);  // our animation swaps would otherwise turn this bark into a quiet move
			// The sitting bark only plays once he's seated; otherwise he barks standing.
			const auto process = a_dog->GetActorRuntimeData().currentProcess;
			const auto sitBark = RE::TESForm::LookupByID<RE::TESIdleForm>(Idles::kDogBarkSit);
			if (!(a_sitting && process && sitBark && process->PlayIdle(a_dog, sitBark, nullptr))) {
				Idles::Play(a_dog, Idles::kDogBarkStand, "idleDogBarkStart");
			}
			logs::info("Bark: {} bark animation", a_sitting ? "sitting (or standing)" : "standing");
		}
	}

	void WhineSound(RE::Actor* a_dog)
	{
		if (a_dog && a_dog->Is3DLoaded()) {
			PlaySound(a_dog, kWhine);
		}
	}

	void HowlSound(RE::Actor* a_dog)
	{
		if (a_dog && a_dog->Is3DLoaded()) {
			PlaySound(a_dog, kHowlSound);
		}
	}

	void BarkSound(RE::Actor* a_dog)
	{
		if (a_dog && a_dog->Is3DLoaded()) {
			PlaySound(a_dog, kBarkSound);
		}
	}

	void ResetSession()
	{
		g_roam.reset();
		g_roaming = false;
		g_roamStill = false;
		g_settled = 0;
		g_holding = 0;
		g_aiPaused = 0;
		g_layPending = false;
		g_nextBowCheck = {};
		g_lastBow.reset();
		g_pose.reset();
		g_posing = false;
		g_lastWiggleTick = 0;
		g_lastRollTick = 0;
		g_fishNear = false;
		g_lastFish = 0;
		g_fishBefore.clear();
		g_standFirst.reset();
		g_napping = false;
		g_lying = false;
		g_lastAsk = 0;
		// Greet, Warm, Sing and the food actions start clean in every save.
		g_greetArmed = false;
		g_awaySince.reset();
		g_lastGreet.reset();
		g_warmUntil.reset();
		g_warmedOnce = false;
		g_playerCold = false;
		g_lastWarm = 0;
		g_sangSong = 0;
		g_lastSing = 0;
		g_looseFood = false;
		g_carriedFood = false;
		g_lastFood = 0;
		g_nextFoodScan = {};
		g_foodItem = 0;
		g_downed = false;
		g_preyNear = false;
		g_carcassNear = false;
		g_lastHunt = 0;
		g_nextHuntScan = {};
		g_eatenCarcasses.clear();
		g_forageNoted = 0;
		g_forageUntil.reset();
		g_forageQuietUntil.reset();
		g_lastNpcPet = 0;
		g_nextPetScan = {};
		g_petDog = 0;
		{
			std::scoped_lock lock(g_pettersLock);
			g_petters.clear();
		}
		std::scoped_lock lock(g_forageLock);
		g_forage.clear();
	}
}
