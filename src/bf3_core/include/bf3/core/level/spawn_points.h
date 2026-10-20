#pragma once

#include "bf3/core/level/level_transform.h"
#include "bf3/core/level/world.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bf3::core::level {

struct SpawnPointDef {
	uint32_t object = 0;
	LevelTransform transform;
	int team = 0;
	bool enabled = true;
	float priority = 1.0f;
};

struct CharacterSpawnerDef {
	uint32_t object = 0;
	int team = 0;
	bool enabled = true;
	float protection_radius = 0;
	int64_t friendly_killed_count = 0;
	float friendly_killed_time = 0;
	bool check_all_teams = false;
	bool out_of_sight = false;
	std::string location_text_sid;
};

struct LevelSpawns {
	std::vector<SpawnPointDef> points;
	std::vector<CharacterSpawnerDef> spawners;
};

Result<LevelSpawns> read_level_spawns(const World &p_world);

}
