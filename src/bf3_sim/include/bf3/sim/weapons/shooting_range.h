#pragma once
// The headless shooting range (docs/design/m2a-weapons.md §10): a scripted shooter and stationary stand-in
// targets at set distances on a real static world (Metro) or a synthetic one, run through the same
// step_soldier_weapons -> ProjectileWorld -> damage path the server will run. It is how the §7.1 rows are
// reproduced end to end (plan §6 M2a exit "shooting stationary bot targets on Metro reproduces §7.1") before the
// GameServer integration exists, and what `bf3dump range` and ctest sim.weapons.range run.
//
// The shooter is not a motor soldier: it stands at the lane's eye point, its context is fixed (stance, zoom,
// Base move) and its script writes the Commands (hold, taps, bursts, reloads, a throw), so every shot time and
// direction is a function of the def, the script and the seed alone. Targets never move (stationary stand-ins of
// hit_target.h, facing the shooter); each keeps its own health (SoldierDef MaxHealth) and records hits per zone
// and the shot that killed it.

#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/core/result.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/spawn.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/weapons/ballistics.h"
#include "bf3/sim/weapons/hit_target.h"
#include "bf3/sim/weapons/weapon_sim.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bf3::sim::weapons {

// A straight, level line of sight at standing eye height from a spawn point: every ray along it at eye height and
// at 0.3 m above the floor is clear for `length` metres, and the floor below the far end lies within 0.5 m of the
// start's.
struct RangeLane {
	Vec3 feet{}; // shooter feet
	Vec3 eye{}; // feet + stand eye height
	uint16_t yaw = 0; // Command quantisation
	float length = 0; // clear metres
	uint32_t spawn_index = 0;
};
// The longest lane among p_spawns' points (p_headings per point, deterministic order), or nullopt when none is
// at least p_min_length long.
std::optional<RangeLane> find_range_lane(const CollisionWorld &p_world, std::span<const SpawnPoint> p_spawns, float p_eye_height,
		float p_min_length, uint32_t p_headings = 64);

// The synthetic range world (W2 drop and max range; the W3 fallback when Metro has no lane long enough): a level
// ground slab and vertical walls facing a shooter at the origin looking along +Z, eye at p_eye_height above the
// slab. Each wall is p_wall_half_width wide each side of the line of fire and spans y from the slab to 50 m above
// the eye. The slab spans x and z from -100 m to p_extent and lies p_depth below the eye, so a gated flight never
// leaves the world's bounds (ProjectileWorld removes projectiles world_margin = 200 m outside them): W2 uses
// p_extent 3,500 m (past the M98B's 3,250 m) and p_depth 150 m (the M98B falls 122.6 m in its 5 s). A wall at a
// distance d is a quad in the plane z = d. Triangles carry SurfaceTag material p_material_value.
std::vector<StaticTriangle> range_wall_triangles(std::span<const float> p_wall_distances, float p_eye_height, float p_extent = 3'500.0f,
		float p_depth = 150.0f, float p_wall_half_width = 20.0f, uint32_t p_material_value = 0);

enum class FireScript : uint8_t {
	Hold = 0, // fire held for the whole run (automatic); reloads on its own (AlwaysAutoReload) unless the table has
			// infinite_magazine (W1's 10 s rows, W11)
	// One-tick presses as fast as the weapon allows (single, bolt, pump, burst). "When ready" is pinned to the tick
	// that CONTAINS the ready time: the script presses in command T when next_shot_time < tick_end(T) (and the
	// bolt / pump cycle has ended by then), so the shot goes at max(ready time, tick_start(T)) = the exact ready
	// time (870: 28,800,000 apart, not 29,000,000), and releases in command T + 1. For a bolt that waits for the
	// release (M98B) the press-to-press gap is therefore the cycle + 1 tick (docs §5.3).
	Tap = 1,
	TapEvery = 2, // a one-tick press every tap_period ticks
	ReloadAt = 3, // Hold, plus a reload request at reload_tick (tactical-reload timing)
	Throw = 4, // hold-and-release: hold for hold_ticks, release
};

struct RangeTargetSpec {
	float distance = 0; // along the lane from the shooter's feet
	Pose pose = Pose::Stand;
	HitZone aim_zone = HitZone::Chest; // the shooter aims at this zone's capsule centre of the nearest live target
};

struct RangeScenario {
	uint16_t weapon = 0; // def id in the table
	std::vector<RangeTargetSpec> targets;
	Pose shooter_pose = Pose::Stand;
	bool ads = true; // zoom held
	FireScript script = FireScript::Hold;
	uint32_t ticks = 600; // how long the script runs
	uint32_t tap_period = 0;
	uint32_t reload_tick = 0;
	uint32_t hold_ticks = 0;
	uint64_t match_seed = 1;
	uint32_t player_slot = 0;
	// Aim exactly (spread and recoil forced to 0 for the shot's direction; their state still runs and is
	// reported): the zone-multiplier and shots-to-kill rows need every shot on its capsule.
	bool exact_aim = false;
	uint32_t settle_ticks = 180; // after the script: projectiles in flight finish
};

struct RangeTargetResult {
	float distance = 0;
	std::array<uint32_t, kHitZoneCount> hits{};
	float damage = 0;
	float health = 0; // left at the end
	bool killed = false;
	uint32_t killing_shot = 0; // shot_index
	uint32_t shots_at_kill = 0; // shots fired at this target until the kill (that hit or not)
	SimTime kill_time = 0;
};

struct RangeResult {
	RangeLane lane;
	std::vector<ShotEvent> shots;
	std::vector<ImpactEvent> impacts;
	std::vector<DamageEvent> damage;
	std::vector<DetonationEvent> detonations;
	std::vector<RangeTargetResult> targets;
	std::vector<uint32_t> weapon_events; // per tick, weapon_events:: (reload and bolt timing)
	uint64_t shot_list_hash = 0; // shot_list_hash(shots)
	uint64_t impact_list_hash = 0;
	size_t refused_projectiles = 0;
	size_t peak_live_projectiles = 0;
	// Static-world diagnostics summed over the run (ProjectileStepOutput): rays that passed kSoupInvisible triangles
	// (a wall whose only bullet geometry is "collisionmesh" would let bullets through: R-W10), penetrations, and
	// Collision-layer hits with a Raycast-layer hit within 10 cm behind them (R-W2).
	size_t static_hits = 0;
	size_t invisible_passes = 0;
	size_t penetrations = 0;
	size_t collision_before_raycast = 0;
	double seconds_wall = 0; // wall time of the run (reported, not gated: the machine is shared)
};

// Runs one scenario. p_world is the caller's (one owner thread). Fails when the table has no such def, the soldier
// has no stand / crouch / prone pose, or a target would lie past the lane.
core::Result<RangeResult> run_range(const WeaponTable &p_table, const CollisionWorld &p_world, const core::gameplay::SoldierDef &p_soldier,
		std::span<const core::gameplay::HitCapsule> p_capsules, const RangeLane &p_lane, const RangeScenario &p_scenario);

// W11's load (docs §1, wall clock, noisy): p_shooters scripted shooters (Hold, hip, the def p_weapon) at the first
// p_shooters of p_spawns (refused when fewer), each standing at its spawn and aiming level along ITS OWN longest
// clear heading (find_range_lane's search on that one point, 64 headings), so bullets fly their full lanes instead
// of dying in the nearest wall; no targets. p_table must have infinite_magazine (refused otherwise): the load is
// 100 % duty, the worst case, not the ~50 % a real magazine allows. Per tick, the wall time of every shooter's
// step_soldier_weapons plus one ProjectileWorld::step is one sample.
struct RangePerfResult {
	std::vector<double> tick_ms; // one sample per tick, in tick order
	double mean_ms = 0;
	double p50_ms = 0;
	double p99_ms = 0; // nearest rank: the ceil(0.99 n)-th smallest (600 ticks: the 594th; 6 samples may exceed it)
	double max_ms = 0;
	size_t peak_live_projectiles = 0;
	size_t shots = 0;
	uint32_t shooters = 0;
	float mean_lane_length = 0;
};
core::Result<RangePerfResult> run_range_perf(const WeaponTable &p_table, const CollisionWorld &p_world, std::span<const SpawnPoint> p_spawns,
		float p_eye_height, uint16_t p_weapon, uint32_t p_shooters = 64, uint32_t p_ticks = 600, uint64_t p_match_seed = 1);

// The weapon alone (W1, W6): p_scenario's script drives step_soldier_weapons at a fixed eye (feet at the origin,
// facing +Z, the shooter pose's eye height p_eye_height) with no world, projectiles or targets. As in run_range,
// idle ticks run the deploy first, then p_scenario.ticks script ticks and p_scenario.settle_ticks idle ticks.
// Every ShotEvent and timed phase event is kept with the per-tick states, so the gates read exact times.
struct FireTimingResult {
	std::vector<ShotEvent> shots;
	std::vector<TimedWeaponEvent> timed;
	std::vector<Tick> presses; // ticks whose command pressed fire (a rising edge)
	std::vector<SoldierWeaponsState> states; // after every tick, the deploy included
	Tick first_script_tick = 0; // the script's first command
	uint32_t refused_shots = 0;
	uint32_t timed_dropped = 0;
};
core::Result<FireTimingResult> run_fire_timing(const WeaponTable &p_table, const RangeScenario &p_scenario, float p_eye_height = 1.6f);

// FNV-1a 64 over every ShotEvent field (identical shot lists for identical seeds, §1 W7).
uint64_t shot_list_hash(std::span<const ShotEvent> p_shots);
uint64_t impact_list_hash(std::span<const ImpactEvent> p_impacts);

} // namespace bf3::sim::weapons
