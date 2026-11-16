// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Canine Drives.
#pragma once

#include <nlohmann/json.hpp>

// Reading model or SkyrimNet JSON without trusting its shape: json::value() throws when a key holds the wrong type
// ("dream": null, "confidence": "high"), so every field is read through these instead.
namespace JsonField
{
	inline std::string Text(const nlohmann::json& a_json, const char* a_key, std::string a_fallback = {})
	{
		if (a_json.is_object()) {
			if (const auto it = a_json.find(a_key); it != a_json.end() && it->is_string()) {
				return it->get<std::string>();
			}
		}
		return a_fallback;
	}

	inline double Number(const nlohmann::json& a_json, const char* a_key, double a_fallback)
	{
		if (a_json.is_object()) {
			if (const auto it = a_json.find(a_key); it != a_json.end() && it->is_number()) {
				return it->get<double>();
			}
		}
		return a_fallback;
	}
}
