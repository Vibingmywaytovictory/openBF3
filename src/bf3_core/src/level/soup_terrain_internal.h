#pragma once
// bf3_core internal (TC, docs/design/terrain-collision.md §5.1): what build_collision_soup needs from the terrain record
// code in soup_terrain.cpp. The public cell functions are in collision_soup.h.

#include "bf3/core/res/terrain.h"

#include <cstddef>

namespace bf3::core::level {

// The cell counts of a grid as hash_soup_terrain gives them (cells whose centre has a sane 64 m tile, and of those the
// ones with a hole corner), without making a triangle: rows on up to p_threads lanes. The grid's heights and holes must
// hold width x height entries (the caller checked).
void soup_terrain_census(const res::HeightGrid &p_grid, unsigned p_threads, size_t &r_cells, size_t &r_open);

} // namespace bf3::core::level
