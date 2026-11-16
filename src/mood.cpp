// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#include "mood.h"

#include "actions.h"
#include "drives.h"
#include "sleep.h"

#include <chrono>

namespace Canine::Mood
{
	namespace
	{
		constexpr std::array<std::string_view, kDriveCount + 1> kNames{ "content", "hungry", "wants affection", "restless",
			"tired", "missing the player" };

		int Strongest(const Settings& a_settings)
		{
			int    best = 0;
			double bestValue = a_settings.tierThresholds[0];
			for (std::size_t d = 0; d < kDriveCount; ++d) {
				const double value = Drives::Value(static_cast<Drive>(d));
				if (value >= bestValue) {
					bestValue = value;
					best = static_cast<int>(d) + 1;
				}
			}
			return best;
		}

		using Clock = std::chrono::steady_clock;

		int               g_lastCode = -1;
		int               g_lastMood = 0;
		Clock::time_point g_barkUntil{};
		Clock::time_point g_bowUntil{};   // the play bow or the wiggle holds its code until then
		int               g_bowCode = kPlayBowCode;
		int               g_poseCode = 0;  // 41-43 while an awake pose holds the code, else 0

		int g_loggedCode = -1;

		// Asleep: from BeginSleep until a tick finds no nap on and his graph no longer lying.
		bool       g_sleeping = false;
		int        g_sleepCode = kSleepingCode;  // 30 + the style picked when this sleep began
		RE::FormID g_trackingOffFor = 0;     // the dog whose head tracking we switched off
		bool       g_trackingWas = true;     // its head-tracking flag before we did
		bool       g_restoreAfterLoad = false;  // first awake tick after a load: undo a mid-sleep save's "off"

		void Write(RE::Actor* a_dog, int a_code)
		{
			const auto owner = a_dog ? a_dog->AsActorValueOwner() : nullptr;
			if (!owner) {
				return;
			}
			if (a_code != g_lastCode || owner->GetActorValue(kActorValue) != static_cast<float>(a_code)) {
				owner->SetActorValue(kActorValue, static_cast<float>(a_code));
				g_lastCode = a_code;
			}
		}

		// The flag vanilla Actor.SetHeadTracking() sets. Held off every tick while he sleeps, in case the
		// engine sets it again; given back as it was once he's up.
		void HeadTracking(RE::Actor* a_dog, bool a_on)
		{
			const auto state = a_dog ? a_dog->AsActorState() : nullptr;
			if (!state) {
				return;
			}
			if (!a_on) {
				if (g_trackingOffFor != a_dog->GetFormID()) {
					g_trackingWas = state->actorState2.headTracking != 0;
					g_trackingOffFor = a_dog->GetFormID();
					logs::info("Sleep: head tracking off (was {})", g_trackingWas ? "on" : "off");
				}
				state->actorState2.headTracking = 0;
			} else if (g_trackingOffFor == a_dog->GetFormID()) {
				state->actorState2.headTracking = g_trackingWas ? 1 : 0;
				g_trackingOffFor = 0;
				logs::info("Sleep: head tracking back {}", g_trackingWas ? "on" : "off");
			}
		}
	}

	void BeginSleep(RE::Actor* a_dog)
	{
		if (!a_dog) {
			return;
		}
		if (!g_sleeping) {
			std::string why;
			const auto  style = Sleep::Choose(a_dog, why);
			g_sleepCode = kSleepingCode + static_cast<int>(style);
			logs::info("Animation mood: sleeping {} (code {}): {}", Sleep::Name(style), g_sleepCode, why);
		}
		g_sleeping = true;
		g_barkUntil = {};
		HeadTracking(a_dog, false);
		Write(a_dog, g_sleepCode);
	}

	void Update(RE::Actor* a_dog, const Settings& a_settings)
	{
		if (!a_dog) {
			return;
		}
		g_lastMood = a_settings.enabled ? Strongest(a_settings) : 0;
		const int setting = g_lastMood + (a_settings.quietBarks >= 1 ? 10 : 0);  // what the settings ask for

		if (Actions::NapOn()) {
			if (!g_sleeping) {
				BeginSleep(a_dog);
			}
		} else if (g_sleeping && !Actions::IsLying(a_dog)) {
			g_sleeping = false;  // nap over and he's on his feet
			g_loggedCode = -1;    // log the mood he wakes into
			logs::info("Animation mood: awake again");
		}

		if (g_sleeping) {
			g_poseCode = 0;  // a nap took over
			HeadTracking(a_dog, false);
			Write(a_dog, g_sleepCode);
			return;
		}
		// Rolling over: no head tracking, or his head turns toward someone while he's upside down.
		HeadTracking(a_dog, !(g_bowCode == kRolloverCode && Clock::now() < g_bowUntil));
		if (g_poseCode) {
			if (!Actions::PoseOn() && !Actions::IsLying(a_dog)) {
				g_poseCode = 0;
				g_loggedCode = -1;
				logs::info("Animation mood: pose over, on his feet");
			} else {
				Write(a_dog, g_poseCode);  // awake: head tracking stays as it is
				return;
			}
		}
		if (g_restoreAfterLoad) {
			g_restoreAfterLoad = false;
			if (const auto state = a_dog->AsActorState(); state && !state->actorState2.headTracking) {
				state->actorState2.headTracking = 1;
				logs::info("Sleep: head tracking was off after loading (a save made mid-sleep): back on");
			}
		}
		if (setting != g_loggedCode) {
			g_loggedCode = setting;
			logs::info("Animation mood: {}{} (code {})", kNames[g_lastMood], setting >= 10 ? ", quiet barks" : "", setting);
		}
		const auto now = Clock::now();
		Write(a_dog, now < g_bowUntil ? g_bowCode : now < g_barkUntil ? 20 + g_lastMood : setting);  // a bark of ours: the plain bark clips
	}

	bool BeginPose(RE::Actor* a_dog, int a_code)
	{
		if (g_sleeping || !a_dog) {
			return false;
		}
		g_poseCode = a_code;
		g_barkUntil = {};
		g_bowUntil = {};
		logs::info("Animation mood: awake pose (code {})", a_code);
		Write(a_dog, a_code);
		return true;
	}

	namespace
	{
		bool HoldClipCode(RE::Actor* a_dog, int a_code, std::chrono::milliseconds a_for)
		{
			if (g_sleeping || g_poseCode || !a_dog) {
				return false;
			}
			g_bowCode = a_code;
			g_bowUntil = Clock::now() + a_for;
			Write(a_dog, a_code);
			return true;
		}
	}

	bool PlayBow(RE::Actor* a_dog, std::chrono::milliseconds a_for)
	{
		return HoldClipCode(a_dog, kPlayBowCode, a_for);
	}

	bool Wiggle(RE::Actor* a_dog, std::chrono::milliseconds a_for)
	{
		return HoldClipCode(a_dog, kWiggleCode, a_for);
	}

	bool Rollover(RE::Actor* a_dog, std::chrono::milliseconds a_for)
	{
		return HoldClipCode(a_dog, kRolloverCode, a_for);
	}

	bool CuriousTilt(RE::Actor* a_dog, std::chrono::milliseconds a_for)
	{
		return HoldClipCode(a_dog, kCuriousCode, a_for);
	}

	bool GroundSniff(RE::Actor* a_dog, std::chrono::milliseconds a_for)
	{
		return HoldClipCode(a_dog, kGroundSniffCode, a_for);
	}

	bool AirSniff(RE::Actor* a_dog, std::chrono::milliseconds a_for)
	{
		return HoldClipCode(a_dog, kAirSniffCode, a_for);
	}

	bool Sneeze(RE::Actor* a_dog, std::chrono::milliseconds a_for)
	{
		return HoldClipCode(a_dog, kSneezeCode, a_for);
	}

	bool AngryBark(RE::Actor* a_dog, std::chrono::milliseconds a_for)
	{
		return HoldClipCode(a_dog, kAngryBarkCode, a_for);
	}

	void ResetSession()
	{
		g_lastCode = -1;
		g_loggedCode = -1;
		g_barkUntil = {};
		g_bowUntil = {};
		g_sleeping = false;
		g_poseCode = 0;
		// A save made mid-sleep may hold his head tracking off; give it back on the first awake tick.
		g_trackingOffFor = 0;
		g_trackingWas = true;
		g_restoreAfterLoad = true;
	}

	void AllowBark(RE::Actor* a_dog, std::chrono::milliseconds a_for)
	{
		if (g_sleeping || g_poseCode) {
			return;  // swapping his clips mid-sleep or mid-pose would snap him off his side
		}
		g_barkUntil = Clock::now() + a_for;
		Write(a_dog, 20 + g_lastMood);
	}
}
