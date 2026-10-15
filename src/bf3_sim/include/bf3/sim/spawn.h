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
	SoldierState state; // after settle_ticks idle ticks
	float floor_y = 0; // the floor found below the point
	float lift = 0; // placed.y - point.y
	float shift = 0; // horizontal distance placed -> point
	float gap = 0; // feet - floor after settling (padding included)
	bool overlapping = false; // the settled stand shape overlaps geometry (sinking)
};

// Every spawn point of the traversal, in its order, yaw from the point's forward axis (atan2(f.x, f.z)).
std::vector<SpawnPoint> to_spawn_points(const core::level::LevelSpawns &p_spawns);

// p_motor must be bound to p_world. Runs on the caller's thread.
SettleResult settle_spawn(const SoldierMotor &p_motor, const CollisionWorld &p_world, const SpawnPoint &p_point, uint32_t p_life, Tick p_tick,
		const SettleOptions &p_options = {});

// Which point a soldier of p_team spawns at: points of the team or neutral; skip points with another live soldier
// within p_protection_radius (SpawnProtectionRadius, 10 m in TDM); prefer the higher priority, then a seeded
// random pick. TryToSpawnOutOfSight and FriendZone weights are later (M2b). Returns an index into p_points, or
// SIZE_MAX when there is none.
size_t choose_spawn(const std::vector<SpawnPoint> &p_points, int p_team, const std::vector<Vec3> &p_live_soldiers, float p_protection_radius,
		uint64_t p_seed);

} // namespace bf3::sim
