#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace bf3::core::gameplay {

struct MaterialSounds {
	std::vector<std::optional<float>> by_material;
	size_t rows = 0;
	size_t rows_with_sound = 0;

	std::optional<float> sound_id(uint8_t p_physics_material) const;
};

Result<MaterialSounds> read_material_sounds(const EbxResolver &p_db, std::string_view p_grid);

}
