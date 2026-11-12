#pragma once

#include "bf3/sim/sim_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace bf3::sim::vehicles {

enum class HullZone : uint8_t { Front = 0, Rear = 1, Left = 2, Right = 3 };
inline constexpr size_t kHullZoneCount = 4;

inline constexpr float kZoneAngleScaleDegrees = 90.0f;
inline constexpr float kAbandonMinimumLeft = 1e-6f;
inline constexpr float kHorizontalDirectionFloor = 1e-6f;

struct AngleOfImpactRule {
	bool enabled = false;
	float zone12_degrees = 0;
	float zone23_degrees = 0;
	std::array<float, 3> multiplier{ 1, 1, 1 };
};

float angle_of_impact_factor(const AngleOfImpactRule &p_rule, float p_degrees);

struct HealthZoneRule {
	float max_health = 0;
	float max_shield = 0;
	float min_damage_angle = 0;
	float damage_angle_multiplier = 1;
	bool use_damage_angle = false;
};

float zone_angle_factor(const HealthZoneRule &p_zone, float p_degrees);

struct DifficultyRule {
	float friendly_damage_modifier = 1;
	float vehicle_damage_modifier = 1;
};

struct DamageParties {
	uint8_t vehicle_team = 0;
	uint8_t attacker_team = 0;
	bool attacker_is_player = false;
	bool attacker_is_occupant = false;
	bool human_occupant = false;
	bool friendly_fire = false;
	std::optional<DifficultyRule> difficulty;
};

float damage_modifier(float p_armor_multiplier, const DamageParties &p_parties);

struct VehicleDamageRule {
	std::array<HealthZoneRule, kHullZoneCount> zones{};
	bool share_damage = false;
	AngleOfImpactRule angle_of_impact;
	float armor_multiplier = 1;
	float disabled_threshold = 0;
	float regeneration_delay = 0;
	float regeneration_rate = 0;
	float decay_delay = 0;
	float decay_rate = 0;
	float critical_damage_time = 0;
	float pre_explosion_time = 0;
	float pre_destruction_threshold = 0;
	float upside_down_degrees = 0;
	float upside_down_damage = 0;
	float upside_down_delay = 0;
};

enum class VehicleCondition : uint8_t { Healthy = 0, Disabled = 1, Destroyed = 2 };

namespace vehicle_events {
inline constexpr uint8_t kDisabled = 1u << 0;
inline constexpr uint8_t kHealthy = 1u << 1;
inline constexpr uint8_t kDestroyed = 1u << 2;
inline constexpr uint8_t kRegenerationStarted = 1u << 3;
inline constexpr uint8_t kFullHealth = 1u << 4;
inline constexpr uint8_t kExploded = 1u << 5;
inline constexpr uint8_t kUpsideDownHit = 1u << 6;
}

struct VehicleHealth {
	std::array<float, kHullZoneCount> health{};
	std::array<float, kHullZoneCount> shield{};
	VehicleCondition condition = VehicleCondition::Healthy;
	float regeneration_timer = 0;
	float decay_timer = 0;
	float destroyed_timer = 0;
	bool explode_now = false;
	bool exploded = false;
	bool upside_down = false;
	float upside_down_timer = 0;
	float abandon_timer = 0;
};

float full_vehicle_health(const VehicleDamageRule &p_rule);
float vehicle_health(const VehicleDamageRule &p_rule, const VehicleHealth &p_state);
VehicleHealth spawned_vehicle_health(const VehicleDamageRule &p_rule);

struct HullBox {
	Vec3 centre{};
	Vec3 half{ 1, 1, 1 };
};

struct HullHit {
	HullZone zone = HullZone::Front;
	float impact_degrees = 0;
	float zone_degrees = 0;
	bool zone_angle = false;
};

HullHit hull_hit(const HullBox &p_box, const Vec3 &p_local_point, const Vec3 &p_local_direction);

struct VehicleHit {
	float damage = 0;
	HullZone zone = HullZone::Front;
	bool modifiers = true;
	float impact_degrees = 0;
	bool zone_angle = false;
	float zone_degrees = 0;
	float modifier = 1;
};

struct VehicleDamageOutcome {
	bool applied = false;
	bool absorbed = false;
	float damage = 0;
	float health_before = 0;
	float health_after = 0;
	uint8_t events = 0;
};

VehicleDamageOutcome apply_vehicle_damage(
		const VehicleDamageRule &p_rule, VehicleHealth &r_state, const VehicleHit &p_hit);

struct VehicleTickInput {
	float dt = kTickSeconds;
	bool regeneration_disabled = false;
	float up_y = 1;
	bool abandon_applies = false;
	bool abandoned = false;
	float time_until_abandoned = 0;
	float time_until_abandoned_is_destroyed = 0;
};

struct VehicleTickOutcome {
	uint8_t events = 0;
	float upside_down_damage = 0;
	float abandon_damage = 0;
	float decay_damage = 0;
	float regenerated = 0;
};

VehicleTickOutcome step_vehicle_health(
		const VehicleDamageRule &p_rule, VehicleHealth &r_state, const VehicleTickInput &p_input);

}
