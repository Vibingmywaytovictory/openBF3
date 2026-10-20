#pragma once

#include "bf3/core/res/terrain.h"

#include <cstddef>

namespace bf3::core::level {

void soup_terrain_census(const res::HeightGrid &p_grid, unsigned p_threads, size_t &r_cells, size_t &r_open);

}
