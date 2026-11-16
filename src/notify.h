// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#pragma once

#include "settings.h"

// Top-left notes through Papyrus Debug.Notification (this CommonLib has no native wrapper). Game thread.
namespace Canine::Notify
{
	inline void Show(std::string a_text)
	{
		if (!SnapshotSettings().notify) {
			return;
		}
		const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		if (!vm) {
			return;
		}
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
		std::unique_ptr<RE::BSScript::IFunctionArguments>          args{ RE::MakeFunctionArguments(std::move(a_text)) };
		vm->DispatchStaticCall("Debug", "Notification", args.get(), none);
	}

	// "{}" in a_text becomes the actor's name.
	inline void About(RE::FormID a_actor, std::string_view a_text)
	{
		const auto  actor = a_actor ? RE::TESForm::LookupByID<RE::Actor>(a_actor) : nullptr;
		const char* name = actor ? actor->GetDisplayFullName() : nullptr;
		const std::string_view shown = name && *name ? name : "Your dog";
		Show(std::vformat(a_text, std::make_format_args(shown)));
	}
}
