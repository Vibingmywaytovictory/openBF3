#pragma once

#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <cstdint>
#include <optional>
#include <type_traits>

namespace bf3::sim::weapons {

inline constexpr uint32_t kSuppressionCounterCap = 65535;
inline constexpr float kSuppressionAmountScale = 1000.0f;
inline constexpr float kSuppressionFallOffPerSecondSquared = 10000.0f;
inline constexpr float kSuppressionLogSpan = 9.0f;
inline constexpr Tick kSuppressionUpdateTicks = 2;

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
	uint16_t counter = 0;
	Tick hit_tick = 0;
	bool suppressed = false;

	bool operator==(const SuppressionState &) const = default;
};
static_assert(std::is_trivially_copyable_v<SuppressionState>);

uint16_t suppression_counter(const SuppressionDef &p_def, const SuppressionState &p_state, Tick p_tick);

float suppression_ratio(const SuppressionDef &p_def, const SuppressionState &p_state, Tick p_tick);

bool suppressed(const SuppressionDef &p_def, const SuppressionState &p_state, Tick p_tick);

float suppression_level(const SuppressionDef &p_def, const SuppressionState &p_state, Tick p_tick);

float suppression_scale(const SuppressionDef &p_def, const SuppressionState &p_state, Tick p_tick);

SuppressionState add_suppression(
		const SuppressionDef &p_def, const SuppressionState &p_state, Tick p_tick, uint32_t p_add);

SuppressionState mirror_suppression(
		const SuppressionDef &p_def, const SuppressionState &p_mirror, uint16_t p_counter, Tick p_hit_tick);

struct NearMiss {
	float travelled = 0;
	float miss = 0;
};

std::optional<NearMiss> near_miss(
		const Vec3 &p_from, const Vec3 &p_to, const Vec3 &p_origin, const Vec3 &p_centre, float p_radius);

uint32_t near_miss_amount(const ProjectileSimDef &p_projectile, const SuppressionDef &p_def, const NearMiss &p_miss);

uint32_t near_miss_add(uint32_t p_amount, float p_victim_resist, float p_shooter_boost);

Dispersion suppressed_dispersion(const Dispersion &p_dispersion, const SwayModifier &p_increments, float p_scale);
}
