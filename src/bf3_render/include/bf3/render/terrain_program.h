#pragma once

#include "bf3/core/result.h"
#include "bf3/render/dxbc.h"
#include "bf3/render/shader_translate.h"
#include "bf3/render/terrain_look.h"

#include <array>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace bf3::render {

using TerrainConstantValue = std::function<core::Result<std::array<float, 4>>(const std::string &p_name)>;

core::Status bind_terrain_program(const std::string &p_where, const dxbc::Program &p_program,
		const PixelTranslation &p_translation, std::span<const SceneTerrainTexture> p_textures,
		const std::array<float, 4> &p_height_uv, const std::array<float, 4> &p_height_pixel,
		const TerrainConstantValue &p_value, TerrainMaterial &r_material);

core::Result<std::string> terrain_shader_head(const PixelTranslation &p_translation, const TerrainMaterial &p_material,
		std::string_view p_what, std::string_view p_render_mode);

std::string terrain_shader_locals(const PixelTranslation &p_translation, const TerrainMaterial &p_material);

}
