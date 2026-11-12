#pragma once

#include "bf3/ai/knowledge/knowledge_context.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::ai {

enum class AiDomain : uint8_t { Any, Infantry, Vehicle, Air };

struct AiSettings {
	int32_t max_bots = -1;
	bool max_bots_include_humans = true;
	float ignore_commander_distance_m = 30.0f;
	int32_t behaviour_count = 0;
	float view_distance_m = -1.0f;
	float stats_view_distance_m = 30.0f;
	bool draw_bot_stats = true;
	float bot_skill = 0.75f;
	float sai_update_frequency = 35.0f;
	uint32_t information_grid_dimension = 32;
	uint32_t sides = 0;
	bool respawn_allowed = true;
	bool draw = false;
	bool auto_spawn_bots = false;
	bool override_menu_settings = false;
	bool enable_vehicle_ai = false;
	bool enable_air_ai = false;
	std::string man_down_equipment;
	std::vector<std::string> vehicles;
	std::vector<float> basic_bot_weights;
	std::array<std::vector<float>, 2> standard_personality;

	static constexpr std::array<float, 6> kArmourClassValues = { 1.0f, 3.0f, 8.0f, 15.0f, 10.0f, 12.0f };

	int battle_strength_count() const { return kBattleStrengthCount; }
	int strategic_strength_count() const { return kStrategicStrengthCount; }
	float armour_class_value(int p_index) const { return kArmourClassValues[size_t(p_index)]; }
	bool domain_enabled(AiDomain p_domain) const;
	int vehicle(std::string_view p_name) const;
	void set_vehicle(const std::string &p_name);
	const float *personality(int p_side) const;

	bool operator==(const AiSettings &) const = default;
};

}
