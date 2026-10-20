#pragma once

#include "bf3/core/level/level_transform.h"
#include "bf3/core/level/world.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bf3::core::level {

struct VehicleSpawnerDef {
	uint32_t object = 0;
	LevelTransform transform;
	std::string blueprint;
	int team = 0;
	bool enabled = true;
	bool auto_spawn = true;
	bool initial_auto_spawn = true;
	float spawn_delay = 0;
	float initial_spawn_delay = 0;
	float spawn_area_radius = 0;
	float respawn_range = 0;
	float rotation_yaw = 0, rotation_pitch = 0, rotation_roll = 0;
};

Result<std::vector<VehicleSpawnerDef>> read_vehicle_spawners(const World &p_world);

}
