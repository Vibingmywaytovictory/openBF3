#pragma once

#include "bf3/ai/knowledge/knowledge_context.h"
#include "bf3/core/result.h"
#include "bf3/core/util/key_values.h"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace bf3::ai {

enum class AiDomain : uint8_t { Any, Infantry, Vehicle, Air };

struct AiSettings {
	int32_t max_bots = 64;
	bool max_bots_include_humans = true;
	float bot_skill = 0.75f;
	float view_distance_m = -1.0f;
	float sai_update_frequency = 35.0f;
	uint32_t information_grid_dimension = 32;
	uint32_t sides = 2;
	bool respawn_allowed = true;
	bool auto_spawn_bots = false;
	bool enable_vehicle_ai = false;
	bool enable_air_ai = false;

	static constexpr std::array<float, 6> kArmourClassValues = { 1.0f, 3.0f, 8.0f, 15.0f, 10.0f, 12.0f };

	int battle_strength_count() const { return kBattleStrengthCount; }
	int strategic_strength_count() const { return kStrategicStrengthCount; }
	float armour_class_value(int p_index) const { return kArmourClassValues[size_t(p_index)]; }
	bool domain_enabled(AiDomain p_domain) const;
};

inline constexpr std::string_view kAiSettingsPrefix = "ai.";

std::span<const core::KeyDef<AiSettings>> ai_settings_keys();
core::Status parse_ai_settings(std::string_view p_text, AiSettings &r_settings);
core::KeyOutcome check_ai_setting(std::string_view p_key, std::string_view p_value);
std::string format_ai_settings(const AiSettings &p_settings);

}
