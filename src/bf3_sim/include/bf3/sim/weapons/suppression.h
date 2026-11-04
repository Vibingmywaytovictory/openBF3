#pragma once

#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <cstdint>
#include <type_traits>

namespace bf3::sim::weapons {

inline constexpr float kSuppressionPerMultiplier = 0.01f;
inline constexpr float kSuppressionLevelSteps = 65535.0f;

struct SuppressionDef {
	bool present = false;
	float sphere_radius = 0;
	float decay = 0;
	float fall_off_delay = 0;
	float fall_off_multiplier = 1;
	float low_threshold = 0;
	float high_threshold = 0;
	float ui_threshold = 0;
	bool aborts_regeneration = false;
	bool check_line_of_sight = false;
};

core::Result<SuppressionDef> make_suppression_def(const core::gameplay::SoldierDef &p_soldier);

struct SuppressionState {
	uint16_t level = 0;
	Tick hit_tick = 0;

	bool operator==(const SuppressionState &) const = default;
};
static_assert(std::is_trivially_copyable_v<SuppressionState>);

float suppression_level(const SuppressionDef &p_def, const SuppressionState &p_state, Tick p_tick);

SuppressionState add_suppression(
		const SuppressionDef &p_def, const SuppressionState &p_state, Tick p_tick, float p_amount);

float suppression_multiplier(const ProjectileSimDef &p_projectile, float p_distance);

enum class SegmentEnd : uint8_t { Flying = 0, Ended = 1 };

bool passes_near(
		const Vec3 &p_from, const Vec3 &p_to, bool p_first, SegmentEnd p_end, const Vec3 &p_centre, float p_radius);

}
