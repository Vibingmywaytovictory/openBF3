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

enum class FireScript : uint8_t {
	Hold = 0, // fire held for the whole run (automatic); reloads on its own (AlwaysAutoReload)
	Tap = 1, // one-tick presses as fast as the weapon allows (single, bolt, burst): press when ready, release next tick
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
	double seconds_wall = 0; // wall time of the run (reported, not gated: the machine is shared)
};

// Runs one scenario. p_world is the caller's (one owner thread). Fails when the table has no such def, the soldier
// has no stand / crouch / prone pose, or a target would lie past the lane.
core::Result<RangeResult> run_range(const WeaponTable &p_table, const CollisionWorld &p_world, const core::gameplay::SoldierDef &p_soldier,
		std::span<const core::gameplay::HitCapsule> p_capsules, const RangeLane &p_lane, const RangeScenario &p_scenario);

// FNV-1a 64 over every ShotEvent field (identical shot lists for identical seeds, §1 W7).
uint64_t shot_list_hash(std::span<const ShotEvent> p_shots);
uint64_t impact_list_hash(std::span<const ImpactEvent> p_impacts);

} // namespace bf3::sim::weapons
