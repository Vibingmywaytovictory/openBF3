#pragma once
// The level grid's MaterialPairs list in order (M2a weapons; docs/design/m2a-weapons.md §4.3, §6.6).
//
// A static-world triangle names its material by an index, not a GUID: the soup's MaterialFlagsAndIndices value
// (sim::SurfaceTag::material) keeps flags in the top byte and, in the low 24 bits (level::kMaterialPairMask), the
// index of a pair in MaterialGridData.MaterialPairs. MaterialGrid (material_grid.h) keeps the pairs by instance
// GUID only, so a bullet hit cannot reach the pair's PhysicsPropertyIndex (damage, penetration) or its
// PhysicsMaterialIndex (impact effects, impact sounds, decals: the other index space, plan §3.2 "Effects and
// impacts") without this table. Verified on MP_Subway (2026-10-15, bf3dump dump of the grid and
// materials/materialcontainer): 32,400 grid pairs, all resolved; the low 24 bits of a container pair's own
// FlagsAndIndex equal its position in the grid list (the first five are 0..4).
//
// This is a new file beside material_grid.h so the M0 reader stays unchanged; the M2a weapons implementation
// wave defines it (src/gameplay/grid_pairs.cpp). The VFX and audio tracks read impacts through the same table.

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/result.h"

#include <string_view>
#include <vector>

namespace bf3::core::gameplay {

// Every entry of p_grid's MaterialGridData.MaterialPairs, in order, completed from p_material_grid's container
// (level copy, then its fallbacks). An entry that does not resolve has resolved = false. Fails when the grid
// partition or its MaterialPairs array is missing, or when the array is longer than 2^24 entries (the index
// field's range; retail: 32,400).
Result<std::vector<MaterialPairRef>> read_grid_pairs(const EbxResolver &p_db, std::string_view p_grid, const MaterialGrid &p_material_grid);

} // namespace bf3::core::gameplay
