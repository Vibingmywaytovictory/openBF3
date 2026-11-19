#pragma once

#include "bf3/core/res/terrain_decals.h"
#include "bf3/render/look_environment.h"
#include "bf3/render/mesh_prep.h"
#include "bf3/render/shader_database.h"
#include "bf3/render/terrain_look.h"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::core {
class LevelMount;
}

namespace bf3::render {

inline constexpr std::array<std::string_view, 6> kWaterGlobals{ "outdoorLightHemisphereDir", "outdoorLightTopColor",
	"outdoorLightBottomColor", "outdoorLightDir", "outdoorLightKeySpecularColorAndHemisphereVisibility",
	"outdoorLightSkyEnvmap8BitTexInvScale" };

struct SceneWaterShader {
	std::string shader;
	std::vector<ShaderSolution> solutions;
	std::vector<SceneTerrainTexture> textures;
};

std::vector<SceneWaterShader> plan_terrain_water(const core::LevelMount &p_mount, const ShaderLookup &p_shaders,
		std::string_view p_asset, const std::function<int32_t(const std::string &)> &p_texture,
		std::vector<std::pair<std::string, std::string>> &r_failures);

struct TerrainWaterStats {
	uint32_t records = 0;
	uint32_t drawn_records = 0;
	uint32_t shaders = 0;
	uint32_t drawn_shaders = 0;
	uint32_t surfaces = 0;
	uint64_t triangles = 0;
	std::vector<std::pair<std::string, std::string>> undrawn;
};

core::Result<TerrainMaterial> terrain_water_material(const SceneWaterShader &p_shader, const LookEnvState &p_look,
		const SceneTerrainTexture &p_envmap, const std::array<float, 4> &p_height_uv);

TerrainWaterStats add_terrain_water(const core::res::TerrainDecals &p_decals,
		std::span<const SceneWaterShader> p_shaders, const LookEnvState &p_look, const SceneTerrainTexture &p_envmap,
		TerrainLook &r_look, PreparedMesh &r_mesh);

}
