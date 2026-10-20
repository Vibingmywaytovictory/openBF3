#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/result.h"

#include <string_view>
#include <vector>

namespace bf3::core::gameplay {

Result<std::vector<MaterialPairRef>> read_grid_pairs(const EbxResolver &p_db, std::string_view p_grid, const MaterialGrid &p_material_grid);

}
