#pragma once

#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/core/result.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/spawn.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/weapons/ballistics.h"
#include "bf3/sim/weapons/hit_target.h"
#include "bf3/sim/weapons/range_lane.h"
#include "bf3/sim/weapons/weapon_sim.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bf3::sim::weapons {

std::vector<StaticTriangle> range_wall_triangles(std::span<const float> p_wall_distances, float p_eye_height, float p_extent = 3'500.0f,
		float p_depth = 150.0f, float p_wall_half_width = 20.0f, uint32_t p_material_value = 0);

enum class FireScript : uint8_t {
	Hold = 0,

	Tap = 1,
	TapEvery = 2,
	ReloadAt = 3,
	Throw = 4,
};

struct RangeTargetSpec {
	float distance = 0;
	Pose pose = Pose::Stand;
	HitZone aim_zone = HitZone::Chest;
};

struct RangeScenario {
	uint16_t weapon = 0;
	std::vector<RangeTargetSpec> targets;
	Pose shooter_pose = Pose::Stand;
	bool ads = true;
	FireScript script = FireScript::Hold;
	uint32_t ticks = 600;
	uint32_t tap_period = 0;
	uint32_t reload_tick = 0;
	uint32_t hold_ticks = 0;
	uint64_t match_seed = 1;
	uint32_t player_slot = 0;

	bool exact_aim = false;
	uint32_t settle_ticks = 180;
};

struct RangeTargetResult {
	float distance = 0;
	std::array<uint32_t, kHitZoneCount> hits{};
	float damage = 0;
	float health = 0;
	bool killed = false;
	uint32_t killing_shot = 0;
	uint32_t shots_at_kill = 0;
	SimTime kill_time = 0;
};

struct RangeResult {
	RangeLane lane;
	std::vector<ShotEvent> shots;
	std::vector<ImpactEvent> impacts;
	std::vector<DamageEvent> damage;
	std::vector<DetonationEvent> detonations;
	std::vector<RangeTargetResult> targets;
	std::vector<uint32_t> weapon_events;
	uint64_t shot_list_hash = 0;
	uint64_t impact_list_hash = 0;
	size_t refused_projectiles = 0;
	size_t peak_live_projectiles = 0;

	size_t static_hits = 0;
	size_t invisible_passes = 0;
	size_t penetrations = 0;
	size_t collision_before_raycast = 0;
	double seconds_wall = 0;
};

core::Result<RangeResult> run_range(const WeaponTable &p_table, const CollisionWorld &p_world, const core::gameplay::SoldierDef &p_soldier,
		std::span<const core::gameplay::HitCapsule> p_capsules, const RangeLane &p_lane, const RangeScenario &p_scenario);

struct RangePerfResult {
	std::vector<double> tick_ms;
	double mean_ms = 0;
	double p50_ms = 0;
	double p99_ms = 0;
	double max_ms = 0;
	size_t peak_live_projectiles = 0;
	size_t shots = 0;
	uint32_t shooters = 0;
	float mean_lane_length = 0;
};
core::Result<RangePerfResult> run_range_perf(const WeaponTable &p_table, const CollisionWorld &p_world, std::span<const SpawnPoint> p_spawns,
		float p_eye_height, uint16_t p_weapon, uint32_t p_shooters = 64, uint32_t p_ticks = 600, uint64_t p_match_seed = 1);

struct FireTimingResult {
	std::vector<ShotEvent> shots;
	std::vector<TimedWeaponEvent> timed;
	std::vector<Tick> presses;
	std::vector<SoldierWeaponsState> states;
	Tick first_script_tick = 0;
	uint32_t refused_shots = 0;
	uint32_t timed_dropped = 0;
};
core::Result<FireTimingResult> run_fire_timing(const WeaponTable &p_table, const RangeScenario &p_scenario, float p_eye_height = 1.6f);

uint64_t shot_list_hash(std::span<const ShotEvent> p_shots);
uint64_t impact_list_hash(std::span<const ImpactEvent> p_impacts);

}
