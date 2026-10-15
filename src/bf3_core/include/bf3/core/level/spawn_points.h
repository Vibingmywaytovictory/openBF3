#pragma once
// The soldier spawn data a traversed level x mode places (plan §3.7 "Spawns"; topics/02 §3.12; docs/design/m1.md
// §10). Godot-free; reads EBX fields of the World's placed objects.
//
//   points:   every AlternateSpawnEntityData placement (ObjectKind::AlternateSpawn), in object order, with its
//             world transform and its Team / Enabled / Priority fields (effective values: a PropertyConnection
//             that feeds one wins, Scopes::property).
//   spawners: every CharacterSpawnReferenceObjectData placement (ObjectKind::CharacterSpawnReference) with
//             Team, Enabled, SpawnProtectionRadius, SpawnProtectionFriendlyKilledCount / Time,
//             SpawnProtectionCheckAllTeams, TryToSpawnOutOfSight, LocationTextSid.
// Checked on Metro TDM (bf3dump level / dump, 2026-10-15): 102 points in
// levels/mp_subway/teamdeathmatch/layer0_teamdeathmatch_logic, all TeamNeutral, Enabled, Priority 1, y 59.8..73.4;
// two spawners in gameplay/level_setups/complete_setup/full_teamdeathmatch (Team1, Team2; radius 10, 3 friendly
// kills in 30 s, out of sight, LocationTextSid ID_H_MP_DM_RANDOMSPAWN). Which spawner uses which points is wired
// by the mode's logic (S4); in TDM both teams draw from the neutral pool.
// Charges the World's WorkBudget like the other helpers (census.h).

#include "bf3/core/level/level_transform.h"
#include "bf3/core/level/world.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bf3::core::level {

struct SpawnPointDef {
	uint32_t object = 0; // index into World::objects
	LevelTransform transform;
	int team = 0; // TeamId: 0 TeamNeutral, 1 Team1, 2 Team2, ...
	bool enabled = true;
	float priority = 1.0f;
};

struct CharacterSpawnerDef {
	uint32_t object = 0;
	int team = 0;
	bool enabled = true;
	float protection_radius = 0; // SpawnProtectionRadius
	int64_t friendly_killed_count = 0; // SpawnProtectionFriendlyKilledCount
	float friendly_killed_time = 0; // SpawnProtectionFriendlyKilledTime
	bool check_all_teams = false;
	bool out_of_sight = false; // TryToSpawnOutOfSight
	std::string location_text_sid;
};

struct LevelSpawns {
	std::vector<SpawnPointDef> points;
	std::vector<CharacterSpawnerDef> spawners;
};

Result<LevelSpawns> read_level_spawns(const World &p_world);

} // namespace bf3::core::level
