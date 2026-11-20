#pragma once

#include "bf3/core/res/terrain_decals.h"
#include "bf3/render/look_environment.h"
#include "bf3/render/mesh_prep.h"
#include "bf3/render/terrain_look.h"
#include "bf3/render/water.h"

#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::core {
class EbxResolver;
}

namespace bf3::render {

struct SurfaceDecalShapes {
	std::set<std::string> standalone;
	std::set<std::string> paired;
	size_t shapes = 0;
	size_t without_shader = 0;
};

SurfaceDecalShapes surface_decal_shapes(const core::EbxResolver &p_db, std::string_view p_asset);

struct TerrainSurfaceDecalStats {
	uint32_t records = 0;
	uint32_t drawn_records = 0;
	uint32_t shaders = 0;
	uint32_t drawn_shaders = 0;
	uint32_t surfaces = 0;
	uint64_t triangles = 0;
	std::vector<std::pair<std::string, std::string>> undrawn;
};

TerrainSurfaceDecalStats add_terrain_surface_decals(const core::res::TerrainDecals &p_decals,
		std::span<const SceneWaterShader> p_shaders, const LookEnvState &p_look, TerrainLook &r_look,
		PreparedMesh &r_mesh);

}
