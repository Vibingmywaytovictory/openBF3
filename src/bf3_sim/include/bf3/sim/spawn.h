#pragma once
// Spawning soldiers on the level's spawn points (plan §3.1 "expect a spawn settle", §3.7 spawns; topics/02
// §3.12, §4.5; docs/design/m1.md §10). Godot-free.
//
// Points: bf3_core level/spawn_points.h reads them from the traversal (Metro TDM: 102 AlternateSpawnEntityData
// in levels/mp_subway/teamdeathmatch/layer0_teamdeathmatch_logic, all TeamNeutral, Enabled, Priority 1, used by
// the two CharacterSpawnReferenceObjectData of gameplay/level_setups/complete_setup/full_teamdeathmatch, Team1 and
// Team2, SpawnProtectionRadius 10, TryToSpawnOutOfSight). to_spawn_points turns them into SpawnPoints.
//
// Settle: BF3 spawn points sit closer to walls than our hull and a little above or below the floor (handoff 04
// §2.2). settle_spawn finds where a standing soldier fits, then lets the motor drop it onto the ground; the
// result must stand on walkable ground without sinking (no overlap) or hovering (feet within padding + 1 cm of the
// floor), within lift / shift limits.

#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bf3::core::level {
struct LevelSpawns;
}

namespace bf3::sim {

class CollisionWorld;
class SoldierMotor;

struct SpawnPoint {
	Vec3 position{}; // the point's transform translation (Frostbite metres)
	uint16_t yaw = 0; // from its forward axis (Command quantisation)
	int team = 0; // 0 neutral, 1, 2 (TeamId)
	float priority = 1.0f;
	uint32_t index = 0; // stable index: object order of the traversal (bf3dump level lists the same order)
};

struct SettleOptions {
	float probe_up = 1.0f; // look for the floor from this far above the point
	float probe_down = 2.0f; // ... down to this far below it
	float max_lift = 0.6f; // how far up a blocked point may be moved
	float ring_step = 0.1f; // horizontal search rings when the stand shape does not fit (m)
	uint32_t rings = 5; // up to ring_step * rings sideways
	uint32_t ring_samples = 8;
	uint32_t settle_ticks = 60; // idle ticks the motor runs after placing
	float hover_tolerance = 0.01f; // allowed gap feet -> floor beyond the character padding
};

struct SettleResult {
	bool ok = false;
	std::string failure; // why not, when !ok (no floor, no room within the rings, still airborne, ...)
	Vec3 placed{}; // where the soldier was put before the idle ticks
	// After settle_ticks idle ticks. place_spawn / judge_settle leave it as the steps made it (tick = p_tick +
	// settle_ticks for the headless pieces); settle_spawn re-stamps it with stamp_spawn(p_tick), so it is the
	// state of a soldier that spawns at p_tick with full spawn protection.
	SoldierState state;
	float floor_y = 0; // the floor a ray found below the placed x, z
	float lift = 0; // placed.y - point.y
	float shift = 0; // horizontal distance placed -> point
	// After settling: how far the stand shape's bottom sphere could still drop straight down before touching
	// anything (0 at rest, also on a slope or an edge where the feet are above the surface under the centre).
	float gap = 0;
	bool overlapping = false; // the settled stand shape overlaps geometry (sinking)
};

// Every spawn point of the traversal, in its order, yaw from the point's forward axis (atan2(f.x, f.z)).
std::vector<SpawnPoint> to_spawn_points(const core::level::LevelSpawns &p_spawns);

// settle_spawn in three pieces, so the server's SpawnSweep runs the idle ticks over live server ticks (not 6,120
// motor steps inside one tick) and still gives the same rows as the headless call, bit for bit:
//   place_spawn   floor ray (the rings when it misses), lifts, rings with lifts -> SettleResult with placed /
//                 floor_y / lift / shift filled and state = spawn_state(placed); failure set when no floor or no
//                 room
//   (the caller)  settle_ticks steps of the idle command (sim::idle_command) on state
//   judge_settle  grounded, no overlap, |gap| <= padding + hover_tolerance, lift / shift limits -> ok, gap,
//                 overlapping, failure
// The geometric fields do not depend on p_tick or p_life (the motor never reads them), so a sweep started at
// any server tick yields the headless rows.
SettleResult place_spawn(const SoldierMotor &p_motor, const CollisionWorld &p_world, const SpawnPoint &p_point, uint32_t p_life, Tick p_tick,
		const SettleOptions &p_options = {});
void judge_settle(const SoldierMotor &p_motor, const CollisionWorld &p_world, const SpawnPoint &p_point, const SettleOptions &p_options,
		SettleResult &r_result);
// Makes a settled state the spawn state of tick p_tick: tick = p_tick, protect_ticks = immortal_ticks with
// kSpawnProtected (the idle ticks do not use up ImmortalTimeAfterSpawn), events = kSpawned, air_ticks,
// jump_penalty_ticks, impact_speed and damage 0. Position, velocity, ground normal, pose and the grounded /
// steep flags are kept. The server sends this state in Spawned and steps it from p_tick + 1 (docs §8.2).
void stamp_spawn(const SoldierMotor &p_motor, Tick p_tick, SoldierState &r_state);
// place_spawn, settle_ticks idle steps (ticks p_tick + 1 ...; the motor's tick precondition), judge_settle, then
// stamp_spawn(p_tick). The server calls it in the rules step of server tick p_tick (docs §8.1 step 5).
// p_motor must be bound to p_world. Caller's thread.
SettleResult settle_spawn(const SoldierMotor &p_motor, const CollisionWorld &p_world, const SpawnPoint &p_point, uint32_t p_life, Tick p_tick,
		const SettleOptions &p_options = {});

// Which point a soldier of p_team spawns at: points of the team or neutral; skip points with another live soldier
// within p_protection_radius (SpawnProtectionRadius, 10 m in TDM); prefer the higher priority, then a seeded
// random pick. TryToSpawnOutOfSight and FriendZone weights are later (M2b). Returns an index into p_points, or
// SIZE_MAX when there is none.
size_t choose_spawn(const std::vector<SpawnPoint> &p_points, int p_team, const std::vector<Vec3> &p_live_soldiers, float p_protection_radius,
		uint64_t p_seed);

// ---- test courses on a real level (sim.motor, host.session, --m1-check: docs §14) ----------------------------
// Deterministic searches, so the headless tests and the Godot check pick the same spots on the same world.

// A heading (Command yaw) from p_feet along which a standing soldier has p_length metres of clear, level path: a
// stand-capsule sweep of p_length, lifted 2 mm off the feet, finds nothing (no wall, and no step, curb, mat or seam
// higher than 2 mm under the capsule's footprint: such lips bump a walker and cost speed), and floor samples every
// 10 cm along it (a 5 cm sphere lowered onto the floor, so the millimetre cracks between floor meshes do not count)
// stay within p_max_rise of p_feet.y with normals within 2 degrees of up (no holes, stairs or ramps: the speed
// checks must not see the ground plane's sin^2 shortening). At the start the prone shape and the prone footprint
// box (CollisionBoxMin/MaxExpand, 1.3 m behind to 1.1 m ahead, oriented by the heading) and the crouch shape must
// also be free, and the stand shape needs the jump height + 5 cm of room straight up, so the E1b jump and the E1c
// pose courses run from p_feet on that heading and are refused only for timing bugs, never for a nearby wall or
// ceiling. Tries p_headings yaws evenly spaced from p_first_yaw (64 by default: most Metro points stand by a wall,
// where the footprint rules out most headings); nullopt when none qualifies (the test then moves to the next spawn
// point).
std::optional<uint16_t> find_clear_heading(const SoldierMotor &p_motor, const CollisionWorld &p_world, const Vec3 &p_feet, float p_length,
		float p_max_rise = 0.05f, uint32_t p_headings = 64, uint16_t p_first_yaw = 0);

// A floor spot for the E1d drops: scans the static world's bounds on a p_grid metre grid (x, then z, ascending)
// and returns the first floor point whose normal is within 2 degrees of up over a 1 m patch and from which a
// stand capsule swept p_clearance metres straight up hits nothing. nullopt when none.
std::optional<Vec3> find_drop_spot(const SoldierMotor &p_motor, const CollisionWorld &p_world, float p_clearance = 50.0f, float p_grid = 5.0f);

} // namespace bf3::sim
