#pragma once
// The vehicle spawners a traversed level x mode places (plan §3.7 "Spawns", §3.4; research/topics/04-vehicles.md
// §2.8; docs/design/s2.md §4.3). Godot-free; reads EBX fields of the World's placed objects, like spawn_points.h.
//
//   spawners: every VehicleSpawnReferenceObjectData placement (ObjectKind::VehicleSpawnReference), in object order,
//             with its world transform (the placement's, every enclosing reference composed), the Blueprint
//             partition, Team, Enabled (effective value: a PropertyConnection that feeds one wins,
//             Scopes::property; the raw field is often false and fed by the mode's logic), SpawnDelay,
//             InitialSpawnDelay, AutoSpawn / InitialAutoSpawn, SpawnAreaRadius, RespawnRange and the
//             RotationYaw / Pitch / Roll fields (degrees, applied by the spawner on top of the transform [inferred]).
// mode_def.h's VehicleSpawnDef lists the same spawners per mode without transforms; this reader is the one S2 and
// M4 place vehicles with. Checked on MP_007 RushLarge0 (bf3dump mode MP_007 RL, bf3dump dump of the rush layers,
// 2026-10-15): 6 vehicles/humveearmored/humveearmored spawners in levels/mp_007/rush/layer{2,5,8}_base_{1,2,3}.
// Charges the World's WorkBudget like the other helpers (census.h).

#include "bf3/core/level/level_transform.h"
#include "bf3/core/level/world.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bf3::core::level {

struct VehicleSpawnerDef {
	uint32_t object = 0; // index into World::objects
	LevelTransform transform;
	std::string blueprint; // VehicleBlueprint partition (lowercase), e.g. vehicles/humveearmored/humveearmored
	int team = 0; // TeamId: 0 TeamNeutral, 1 Team1, 2 Team2, ...
	bool enabled = true; // effective value
	bool auto_spawn = true;
	bool initial_auto_spawn = true;
	float spawn_delay = 0; // s
	float initial_spawn_delay = 0; // s
	float spawn_area_radius = 0; // m
	float respawn_range = 0; // m
	float rotation_yaw = 0, rotation_pitch = 0, rotation_roll = 0; // degrees
};

Result<std::vector<VehicleSpawnerDef>> read_vehicle_spawners(const World &p_world);

} // namespace bf3::core::level
