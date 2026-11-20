#pragma once

#include "bf3/core/res/terrain.h"
#include "bf3/core/result.h"
#include "bf3/render/shader_translate.h"
#include "bf3/render/terrain_detail.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
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

struct SceneTerrainTexture {
	uint32_t slot = 0;
	int32_t texture = -1;
	bool srgb = false;
};

struct SceneTerrainCombo {
	std::string shader;
	std::vector<uint8_t> layers;
	std::vector<SceneTerrainTexture> textures;
	std::shared_ptr<const std::vector<uint8_t>> program;
};

std::optional<std::vector<uint8_t>> terrain_combo_layers(std::string_view p_shader, std::string_view p_asset);

SceneTerrainTexture plan_terrain_texture(const core::LevelMount &p_mount, uint32_t p_slot, const std::string &p_texture,
		const std::function<int32_t(const std::string &)> &p_register, std::map<std::string, bool> &r_srgb,
		std::vector<std::pair<std::string, std::string>> &r_failures);

std::vector<SceneTerrainCombo> plan_terrain_combos(const core::LevelMount &p_mount, const ShaderLookup &p_shaders,
		std::string_view p_asset, const std::function<int32_t(const std::string &)> &p_texture,
		std::vector<std::pair<std::string, std::string>> &r_failures);

inline constexpr std::string_view kTerrainSurfaceShader = "/defaultterrainsurfaceshader";

inline constexpr std::array<std::array<std::pair<uint8_t, uint8_t>, 4>, 2> kTerrainAtlasLanes{ {
		{ { { 0, 1 }, { 0, 2 }, { 2, 0 }, { 0, 0 } } },
		{ { { 1, 1 }, { 1, 2 }, { 2, 1 }, { 1, 0 } } },
} };

struct TerrainLane {
	std::optional<float> literal;
	uint8_t atlas = 0;
	uint8_t lane = 0;
	float scale = 1.0f;
	float bias = 0.0f;

	bool operator==(const TerrainLane &) const = default;
};

inline constexpr float kTerrainUntracedSmoothness = 0.5f;
inline constexpr TerrainLane kTerrainUntracedSpecular{ std::nullopt, 1, 0 };

struct TerrainSurfaceLanes {
	TerrainLane smoothness;
	TerrainLane specular;
	std::string refusal;

	bool operator==(const TerrainSurfaceLanes &) const = default;
};

core::Result<TerrainSurfaceLanes> terrain_surface_lanes(const ShaderLookup &p_shaders, std::string_view p_asset);

std::string terrain_lane_code(const TerrainLane &p_lane, std::string_view p_cover);

core::Result<TerrainLane> terrain_output_lane(const dxbc::Program &p_program, uint32_t p_output, uint8_t p_lane);

enum class TerrainImageFormat : uint8_t {
	R8,
	Rgba8,
	R32f,
};

struct TerrainImage {
	TerrainImageFormat format = TerrainImageFormat::R8;
	uint32_t width = 0;
	uint32_t height = 0;
	bool mipmaps = false;
	std::vector<uint8_t> data;
};

inline constexpr int32_t kTerrainMaskAtlas = 0;
inline constexpr int32_t kTerrainColourImage = 1;
inline constexpr int32_t kTerrainHeightImage = 2;
inline constexpr int32_t kTerrainDynamicMask = 3;

struct TerrainTextureBinding {
	uint32_t slot = 0;
	int32_t texture = -1;
	int32_t image = -1;
	bool srgb = false;
};

struct TerrainMaterial {
	uint32_t combo = 0;
	std::string code;
	uint32_t constant_slot = 0;
	std::optional<uint32_t> view_slot;
	std::vector<std::array<float, 4>> constants;
	std::vector<TerrainTextureBinding> textures;
	std::vector<std::pair<std::string, std::array<float, 4>>> uniforms;
	int32_t render_priority = 0;
	TerrainSurfaceLanes surface;
};

enum class TerrainChoice : uint8_t {
	Exact,
	Superset,
	Opaque,
	Miss,
};

struct TerrainChoiceResult {
	int32_t combo = -1;
	TerrainChoice kind = TerrainChoice::Miss;
};

TerrainChoiceResult choose_terrain_combo(std::span<const SceneTerrainCombo> p_combos,
		const std::vector<uint8_t> &p_present, const std::vector<uint8_t> &p_opaque);

struct TerrainLookStats {
	uint32_t layers = 0;
	uint32_t patches = 0;
	uint32_t exact = 0;
	uint32_t superset = 0;
	uint32_t opaque = 0;
	uint32_t misses = 0;
	uint64_t image_bytes = 0;
	uint64_t instructions = 0;
	float mask_texel = 0.0f;
	float data_mask_texel = 0.0f;
	float colour_texel = 0.0f;
	float data_colour_texel = 0.0f;
	std::vector<std::vector<uint8_t>> missed;
};

struct TerrainLook {
	std::vector<TerrainImage> images;
	std::vector<TerrainMaterial> materials;
	std::array<float, 2> patch_origin{};
	float patch = 0.0f;
	uint32_t patch_columns = 0;
	uint32_t patch_rows = 0;
	std::vector<uint16_t> patch_material;
	std::array<float, 4> height_uv{};
	std::array<float, 4> height_pixel{};
	TerrainLookStats stats;
	TerrainDetailLayout detail;
	TerrainSurfaceLanes surface;

	uint32_t material_at(float p_x, float p_z) const;
};

struct TerrainLookInput {
	const core::res::TerrainStreamingTree *tree = nullptr;
	const core::res::HeightGrid *heights = nullptr;
	std::array<float, 4> region{};
	std::span<const SceneTerrainCombo> combos;
	core::res::TerrainChunkFetch fetch;
	std::optional<TerrainSurfaceLanes> surface;
};

inline constexpr uint32_t kTerrainMaxMaskSide = 1024;
inline constexpr uint8_t kTerrainNeutralColour = 128;
inline constexpr size_t kTerrainMissedSets = 8;
inline constexpr uint32_t kTerrainPatchSplits = 2;
inline constexpr uint32_t kTerrainMaxColourSide = 1024;

core::Result<TerrainLook> build_terrain_look(const TerrainLookInput &p_input);

core::Result<std::string> terrain_shader_code(const PixelTranslation &p_translation, const TerrainMaterial &p_material);

}
