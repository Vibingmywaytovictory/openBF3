#pragma once

#include "bf3/core/res/res_types.h"

#include <array>
#include <string>
#include <vector>

namespace bf3::core::res {

inline constexpr uint32_t kResTypeVisualTerrain = 0x1CA38E06u;
inline constexpr size_t kTerrainVisualHeaderWords = 15;
inline constexpr size_t kTerrainVisualHeaderBytes = 4 * kTerrainVisualHeaderWords;
inline constexpr size_t kTerrainVisualCellInstancesWord = 3;
inline constexpr size_t kTerrainScatterRecordBytes = 70;
inline constexpr uint32_t kMaxTerrainVisualLayers = 64;
inline constexpr uint32_t kMaxTerrainScatterTypesPerLayer = 256;
inline constexpr uint8_t kMaxTerrainScatterTreeLevel = 16;

struct TerrainScatterType {
	uint8_t layer = 0;
	std::string mesh;
	float density = 0.0f;
	std::array<float, 2> scale_min{};
	std::array<float, 2> scale_max{};
	uint8_t tree_level = 0;
	std::array<uint8_t, kTerrainScatterRecordBytes> record{};
};

struct TerrainVisual {
	std::array<uint32_t, kTerrainVisualHeaderWords> header{};
	std::string surface_shader;
	std::vector<uint8_t> layer_flags;
	std::vector<TerrainScatterType> types;
	std::string streaming_tree;

	uint32_t cell_instances() const { return header[kTerrainVisualCellInstancesWord]; }

	static ResError parse(Bytes p_res, TerrainVisual &r_visual);
};

std::string terrain_visual_name(std::string_view p_terrain_asset);

}
