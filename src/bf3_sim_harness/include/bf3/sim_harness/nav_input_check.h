#pragma once

#include "bf3/core/result.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/nav.h"
#include "bf3/sim/static_world.h"

#include <cstddef>
#include <cstdint>

namespace bf3::sim {

struct NavInputCheck {
	size_t nav_triangles = 0;
	size_t world_triangles = 0;
	size_t matched = 0;
	size_t nav_only = 0;
	size_t nav_only_degenerate = 0;
	size_t world_only = 0;
	float max_corner_error = 0.0f;

	size_t terrain_matched = 0;
	float terrain_tolerance = 0.0f;
};
core::Result<NavInputCheck> nav_check_input_against_world(const StaticWorldShapes &p_world, const NavTileInput &p_input,
		const NavParams &p_params, uint32_t p_layer_mask = query_mask::kMovement, float p_tolerance = 0.001f);

}
