#pragma once

#include "bf3/core/level/level_transform.h"
#include "bf3/core/level/world.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bf3::core::level {

inline constexpr size_t kMaxPlacedEffects = 16384;

struct PlacedEffect {
	uint32_t object = 0;
	std::string blueprint;
	LevelTransform transform;
	bool auto_start = false;
};

Result<std::vector<PlacedEffect>> read_placed_effects(const World &p_world);

}
