// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#include "runes.h"

#include "bridge.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <mutex>

namespace Canine::Runes
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr auto       kPlugin = "SN_Dog.esp"sv;
		constexpr RE::FormID kHjalti = 0x800;
		// His skin per stage: no runes, then forelegs, + hind legs, + head, + back.
		constexpr std::array<RE::FormID, 5> kSkins{ 0x80F, 0x813, 0x814, 0x815, 0x80D };
		constexpr int                       kStages = 4;
		constexpr float                     kWitnessRange = 2000.0f;
		constexpr std::uint32_t             kVersion = 1;
		const RE::BSFixedString             kBody{ "HuskyBodyBare" };

		struct State
		{
			RE::FormID ref = 0;   // the dog the count belongs to
			int        souls = 0;  // dragon souls he has witnessed
		};
		State g_state;  // game thread (and the co-save callbacks)

		// A soul taken shows as a rise in DragonsAbsorbed, the global the vanilla dragon death sequence
		// (MQKillDragonScript) adds 1 to for every soul the player absorbs. The player's DragonSouls can't be used:
		// it rises and falls in the same script call when a soul is spent at once (Mirmulnir: MQ104 zeroes it and
		// unlocks Fus right after the +1), so a once-a-second look never sees the rise.
		constexpr auto       kSkyrim = "Skyrim.esm"sv;
		constexpr RE::FormID kDragonsAbsorbed = 0x01C0F2;
		std::optional<float> g_absorbed;     // DragonsAbsorbed last tick
		std::optional<float> g_playerSouls;  // fallback when the global can't be found: the player's dragon souls

		// A moment of light: up to `level` (a share of the strongest glow), held, then fading out.
		struct Pulse
		{
			float             level = 0.0f;
			Clock::time_point holdUntil{};
			Clock::time_point fadeUntil{};
			bool              shimmer = false;  // a song's glow wavers
		};
		std::mutex                    g_pulseLock;
		std::array<Pulse, 6>          g_pulses{};
		float                         g_glow = 0.0f;  // game thread: the share lit right now (0..1)
		std::optional<Clock::time_point> g_lastTick;

		std::mutex  g_factLock;
		std::string g_fact;

		int Stage(const Settings& a_s)
		{
			return Step(g_state.souls, a_s.soulsPerUnlock, kStages);
		}

		RE::TESNPC* HjaltiBase()
		{
			const auto data = RE::TESDataHandler::GetSingleton();
			return data ? data->LookupForm<RE::TESNPC>(kHjalti, kPlugin) : nullptr;
		}

		RE::TESObjectARMO* Skin(int a_stage)
		{
			const auto data = RE::TESDataHandler::GetSingleton();
			return data ? data->LookupForm<RE::TESObjectARMO>(kSkins[static_cast<std::size_t>(std::clamp(a_stage, 0, kStages))], kPlugin) : nullptr;
		}

		// Where the runes are, by stage.
		std::string_view Where(int a_stage)
		{
			switch (a_stage) {
			case 1:
				return "on his forelegs";
			case 2:
				return "on his forelegs and hind legs";
			case 3:
				return "on his forelegs, his hind legs and his brow";
			default:
				return "on his forelegs, his hind legs, his brow and down the length of his back";
			}
		}

		// What a new stage adds.
		std::string_view Newest(int a_stage)
		{
			switch (a_stage) {
			case 1:
				return "into the fur of his forelegs";
			case 2:
				return "into his hind legs";
			case 3:
				return "across his brow";
			default:
				return "down the length of his back";
			}
		}

		std::string_view Lit(float a_glow)
		{
			if (a_glow < 0.05f) {
				return "dark, like old scars in the fur";
			}
			if (a_glow < 0.3f) {
				return "glowing faintly";
			}
			if (a_glow < 0.7f) {
				return "glowing brightly";
			}
			return "blazing with light";
		}

		std::string Name(RE::TESObjectREFR* a_ref, std::string_view a_fallback)
		{
			const char* name = a_ref ? a_ref->GetDisplayFullName() : nullptr;
			return name && *name ? name : std::string(a_fallback);
		}

		// Game thread: the skin for his stage on his base, his model rebuilt when it changes while he's loaded.
		void ApplySkin(RE::Actor* a_dog, int a_stage)
		{
			const auto base = HjaltiBase();
			const auto skin = Skin(a_stage);
			if (!base || !skin || base->skin == skin) {
				return;
			}
			base->skin = skin;
			logs::info("Runes: skin for stage {} ({:08X})", a_stage, skin->GetFormID());
			if (a_dog && a_dog->GetActorBase() == base && a_dog->Is3DLoaded()) {
				a_dog->DoReset3D(false);  // rebuild his model now (SKSE QueueNiNodeUpdate)
			}
		}

		float PulseLevel(const Pulse& a_pulse, Clock::time_point a_now)
		{
			if (a_pulse.level <= 0.0f || a_now >= a_pulse.fadeUntil) {
				return 0.0f;
			}
			float level = a_pulse.level;
			if (a_now > a_pulse.holdUntil) {
				const float fade = std::chrono::duration<float>(a_pulse.fadeUntil - a_pulse.holdUntil).count();
				const float left = std::chrono::duration<float>(a_pulse.fadeUntil - a_now).count();
				level *= fade > 0.0f ? left / fade : 0.0f;
			}
			if (a_pulse.shimmer) {
				const float t = std::chrono::duration<float>(a_now.time_since_epoch()).count();
				level *= 0.7f + 0.3f * std::sin(t * 2.5f);
			}
			return level;
		}

		// Game thread: the body's glow on his loaded model (only the rune skins carry a glow shader).
		void ApplyGlow(RE::Actor* a_dog, float a_emissive)
		{
			const auto root = a_dog ? a_dog->Get3D(false) : nullptr;
			const auto object = root ? root->GetObjectByName(kBody) : nullptr;
			const auto geometry = object ? object->AsGeometry() : nullptr;
			if (!geometry) {
				return;
			}
			const auto shader = netimmerse_cast<RE::BSLightingShaderProperty*>(geometry->GetGeometryRuntimeData().shaderProperty.get());
			const auto material = shader ? shader->GetBaseMaterial() : nullptr;
			if (!material || material->GetFeature() != RE::BSShaderMaterial::Feature::kGlowMap) {
				return;
			}
			shader->emissiveMult = a_emissive;
		}

		void SetFact(int a_stage, float a_glow, const std::string& a_dogName)
		{
			std::string fact;
			if (a_stage > 0) {
				fact = std::format("Runes are worked into {}'s coat {}, appearing as he witnessed dragons' souls taken. "
								   "Right now they are {}.",
					a_dogName, Where(a_stage), Lit(a_glow));
			}
			std::scoped_lock lock(g_factLock);
			g_fact = std::move(fact);
		}

		// Game thread: the nearest living person (not the player, not him, not hostile) who can see him, a stranger before
		// one of the player's companions.
		RE::Actor* Witness(RE::Actor* a_dog, RE::PlayerCharacter* a_player)
		{
			constexpr float kSeeRange = 1500.0f;
			const auto      lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return nullptr;
			}
			RE::Actor* best = nullptr;
			float      bestScore = 0.0f;
			for (const auto& handle : lists->highActorHandles) {
				const auto ptr = handle.get();
				const auto actor = ptr.get();
				if (!actor || actor == a_dog || actor == a_player || actor->IsDead() || !actor->Is3DLoaded() || actor->IsHostileToActor(a_player)) {
					continue;
				}
				const float distance = actor->GetPosition().GetDistance(a_dog->GetPosition());
				bool        unused = false;
				if (distance > kSeeRange || !actor->HasLineOfSight(a_dog, unused)) {
					continue;
				}
				const float score = distance + (actor->IsPlayerTeammate() ? kSeeRange : 0.0f);  // strangers first
				if (!best || score < bestScore) {
					best = actor;
					bestScore = score;
				}
			}
			return best;
		}

		// Game thread, every tick whatever else happens: reads the soul counter and moves the baseline on, so a soul
		// taken while he couldn't be found is never credited later. Returns how many souls were taken since last tick.
		int ReadSoulsTaken()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto values = player ? player->AsActorValueOwner() : nullptr;
			if (!values) {
				return 0;
			}
			const auto data = RE::TESDataHandler::GetSingleton();
			const auto absorbedGlobal = data ? data->LookupForm<RE::TESGlobal>(kDragonsAbsorbed, kSkyrim) : nullptr;
			float      souls = 0.0f;
			std::optional<float> last;
			if (absorbedGlobal) {
				souls = absorbedGlobal->value;
				last = g_absorbed;
				g_absorbed = souls;
			} else {
				souls = values->GetActorValue(RE::ActorValue::kDragonSouls);
				last = g_playerSouls;
				g_playerSouls = souls;
			}
			if (!last || souls <= *last) {
				return 0;  // first look, or (DragonSouls fallback) souls spent on shouts
			}
			return static_cast<int>(std::lround(souls - *last));
		}

		// Game thread: a soul taken while he is near is a soul he witnessed.
		void WatchSouls(RE::Actor* a_dog, const Settings& a_s)
		{
			const int taken = ReadSoulsTaken();
			if (taken <= 0) {
				return;
			}
			const auto player = RE::PlayerCharacter::GetSingleton();
			const bool close = a_dog && player && !a_dog->IsDead() && a_dog->Is3DLoaded() &&
			                  a_dog->GetPosition().GetDistance(player->GetPosition()) <= kWitnessRange;
			if (!close) {
				logs::info("Runes: a dragon soul taken, but he wasn't close enough to see it");
				return;
			}
			const int before = Stage(a_s);
			g_state.souls += taken;
			const int after = Stage(a_s);
			logs::info("Runes: he witnessed a dragon soul taken ({} so far, stage {})", g_state.souls, after);
			Light(Flare::kSoul);
			const auto dogName = Name(a_dog, "The dog");
			// Who took the soul isn't named: not every player is the Dragonborn as far as SkyrimNet knows.
			std::string line;
			if (after > before) {
				line = std::format("As a dragon's soul tears loose nearby, {}'s fur stands on end and a new rune burns itself {}, blazing white.",
					dogName, Newest(after));
			} else if (after > 0) {
				line = std::format("As a dragon's soul tears loose nearby, the runes in {}'s coat flare in answer.", dogName);
			} else {
				line = std::format("As a dragon's soul tears loose nearby, {}'s fur stands on end and he stares, trembling.", dogName);
			}
			// A witness: the nearest person who can see him answers it, as something that happened to the dog. Without
			// one, nobody is named and SkyrimNet picks (in practice him, or whoever the player faces).
			const auto witness = a_s.soulWitness ? Witness(a_dog, player) : nullptr;
			if (witness) {
				logs::info("Runes: {} saw it", Name(witness, "?"));
			}
			Bridge::PostEvent(std::move(line), witness ? witness->GetFormID() : 0, witness ? a_dog->GetFormID() : 0, a_s.narrate);
		}
	}

	void Update(RE::Actor* a_dog, const Settings& a_settings)
	{
		const auto now = Clock::now();
		const float dt = g_lastTick ? std::chrono::duration<float>(now - *g_lastTick).count() : 0.0f;
		g_lastTick = now;
		const auto base = HjaltiBase();
		if (!a_settings.runesEnabled || !a_dog || !base || a_dog->GetActorBase() != base) {
			// Not watching for him right now: still move the soul baseline on, so a soul taken meanwhile isn't
			// credited to him later.
			if (ReadSoulsTaken() > 0) {
				logs::info("Runes: a dragon soul taken while he couldn't be found (not counted)");
			}
			if (!a_settings.runesEnabled && a_dog && base && a_dog->GetActorBase() == base) {
				// Switched off: his plain coat, unlit (the count itself is kept for when they come back on).
				ApplySkin(a_dog, 0);
				g_glow = 0.0f;
				if (a_dog->Is3DLoaded()) {
					ApplyGlow(a_dog, 0.0f);
				}
			}
			SetFact(0, 0.0f, "");
			return;
		}
		if (g_state.ref != a_dog->GetFormID()) {
			if (g_state.ref) {
				logs::info("Runes: a different dog ({:08X}), the count starts over", a_dog->GetFormID());
			}
			g_state = { a_dog->GetFormID(), 0 };
		}
		WatchSouls(a_dog, a_settings);
		const int stage = Stage(a_settings);
		ApplySkin(a_dog, stage);

		// The brightest moment wins, over a faint glow while he fights. Rising is quick, not instant.
		float target = a_dog->IsInCombat() ? 0.15f : 0.0f;
		{
			std::scoped_lock lock(g_pulseLock);
			for (const auto& pulse : g_pulses) {
				target = (std::max)(target, PulseLevel(pulse, now));
			}
		}
		g_glow = target > g_glow ? (std::min)(target, g_glow + dt * 3.0f) : target;
		if (stage > 0 && a_dog->Is3DLoaded()) {
			ApplyGlow(a_dog, g_glow * static_cast<float>(a_settings.runeMaxGlow));
		}
		SetFact(stage, g_glow, Name(a_dog, "The dog"));
	}

	void Light(Flare a_kind)
	{
		const auto now = Clock::now();
		Pulse      pulse;
		switch (a_kind) {
		case Flare::kSoul:
			pulse = { 1.0f, now + 4s, now + 14s, false };
			break;
		case Flare::kGift:
			pulse = { 0.8f, now + 1s, now + 4s, false };
			break;
		case Flare::kPowerAttack:
			pulse = { 0.5f, now + 300ms, now + 1800ms, false };
			break;
		case Flare::kScent:
			pulse = { 0.3f, now + 2s, now + 5s, false };
			break;
		case Flare::kSong:
			pulse = { 0.35f, now + 15s, now + 19s, true };
			break;
		case Flare::kBite:
			pulse = { 0.35f, now + 200ms, now + 1200ms, false };
			break;
		}
		std::scoped_lock lock(g_pulseLock);
		g_pulses[static_cast<std::size_t>(a_kind)] = pulse;
	}

	int Souls()
	{
		return g_state.souls;
	}

	// The first rune (or gift) comes with the first soul; each further one every a_per after that (default 2: souls 1,
	// 3, 5, 7...).
	int Step(int a_souls, int a_per, int a_cap)
	{
		if (a_souls <= 0) {
			return 0;
		}
		return std::clamp(1 + (a_souls - 1) / (std::max)(a_per, 1), 0, a_cap);
	}

	std::string Fact()
	{
		std::scoped_lock lock(g_factLock);
		return g_fact;
	}

	bool ApplyDebugSet(std::string_view a_text)
	{
		if (a_text == "runes") {
			Light(Flare::kSoul);
			logs::info("debug.set 'runes': lit as for a dragon soul");
			return true;
		}
		if (!a_text.starts_with("souls=")) {
			return false;
		}
		int souls = 0;
		const auto digits = a_text.substr(6);
		std::from_chars(digits.data(), digits.data() + digits.size(), souls);
		g_state.souls = (std::max)(souls, 0);
		Light(Flare::kSoul);
		logs::info("debug.set 'souls={}': stage {}", g_state.souls, Stage(SnapshotSettings()));
		return true;
	}

	void ResetSession()
	{
		g_playerSouls.reset();
		g_absorbed.reset();
		g_lastTick.reset();
		g_glow = 0.0f;
		std::scoped_lock lock(g_pulseLock);
		g_pulses = {};
	}

	void Save(SKSE::SerializationInterface* a_intfc)
	{
		if (!g_state.ref || !a_intfc->OpenRecord(kRecord, kVersion)) {
			return;
		}
		a_intfc->WriteRecordData(g_state.ref);
		a_intfc->WriteRecordData(g_state.souls);
	}

	bool LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type, std::uint32_t a_version)
	{
		if (a_type != kRecord) {
			return false;
		}
		State state;
		if (a_version != kVersion || a_intfc->ReadRecordData(state.ref) != sizeof(state.ref) ||
			a_intfc->ReadRecordData(state.souls) != sizeof(state.souls) || !a_intfc->ResolveFormID(state.ref, state.ref)) {
			return true;
		}
		g_state = state;
		// The skin goes on his base now, before his model loads, so a load shows him as he is with no swap.
		const auto base = HjaltiBase();
		if (const auto skin = Skin(Stage(SnapshotSettings())); base && skin) {
			base->skin = skin;
		}
		return true;
	}

	void Revert()
	{
		g_state = {};
		const auto base = HjaltiBase();
		if (const auto skin = Skin(0); base && skin) {
			base->skin = skin;  // a save without runes starts from his plain coat
		}
		SetFact(0, 0.0f, "");
	}
}
