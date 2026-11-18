#pragma once

#include "bf3/core/level/level_transform.h"
#include "bf3/core/res/terrain.h"
#include "bf3/core/res/terrain_decals.h"
#include "bf3/core/result.h"
#include "bf3/render/mesh_prep.h"
#include "bf3/render/shader_translate.h"
#include "bf3/render/terrain_look.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::core {
class LevelMount;
}

namespace bf3::render {

class ShaderLookup;

inline constexpr float kTerrainDecalViewPull = 0.0005f;
inline constexpr int32_t kTerrainDecalFirstPriority = -100;
inline constexpr int32_t kTerrainDecalLastPriority = -1;
inline constexpr size_t kMaxTerrainDecalTriangles = size_t(1) << 22;

struct SceneDecalShader {
	std::string shader;
	std::vector<SceneTerrainTexture> textures;
	std::shared_ptr<const std::vector<uint8_t>> program;
	std::string refused;
};

std::string terrain_decals_name(std::string_view p_asset);

std::vector<SceneDecalShader> plan_terrain_decals(const core::LevelMount &p_mount, const ShaderLookup &p_shaders,
		std::string_view p_asset, const std::function<int32_t(const std::string &)> &p_texture,
		std::vector<std::pair<std::string, std::string>> &r_failures);

struct DrapeVertex {
	float x = 0.0f;
	float z = 0.0f;
	std::array<float, 2> uv{};
	std::array<float, 4> mask{};
};

struct DrapedVertex {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	std::array<float, 2> uv{};
	std::array<float, 4> mask{};
};

struct DrapeCells {
	size_t x0 = 0;
	size_t z0 = 0;
	size_t x1 = SIZE_MAX;
	size_t z1 = SIZE_MAX;
};

DrapeCells drape_cells(const core::res::HeightGrid &p_grid, const std::optional<std::array<float, 4>> &p_crop_xz);

double drape_triangle(const core::res::HeightGrid &p_grid, const std::array<DrapeVertex, 3> &p_triangle,
		std::vector<DrapedVertex> &r_triangles, const DrapeCells &p_cells = {});

struct TerrainDecalStats {
	uint32_t records = 0;
	uint32_t drawn_records = 0;
	uint32_t shaders = 0;
	uint32_t drawn_shaders = 0;
	uint32_t surfaces = 0;
	uint64_t data_triangles = 0;
	uint64_t drawn_triangles = 0;
	double data_area = 0.0;
	double drawn_area = 0.0;
	uint32_t refused_records = 0;
	double refused_area = 0.0;
	std::vector<std::pair<std::string, std::string>> undrawn;
};

core::Result<std::string> terrain_decal_shader_code(
		const PixelTranslation &p_translation, const TerrainMaterial &p_material);

core::Result<TerrainMaterial> terrain_decal_material(
		const SceneDecalShader &p_shader, const TerrainLook &p_look, int32_t p_priority);

TerrainDecalStats add_terrain_decals(const core::res::TerrainDecals &p_decals, const core::res::HeightGrid &p_grid,
		const core::level::LevelTransform &p_transform, const std::optional<std::array<float, 4>> &p_crop_xz,
		std::span<const SceneDecalShader> p_shaders, TerrainLook &r_look, PreparedMesh &r_mesh);

}
