#pragma once

#include "bf3/core/res/res_types.h"

#include <array>
#include <string>
#include <vector>

namespace bf3::core::res {

inline constexpr uint32_t kResTypeVisualTerrain = 0x1CA38E06u;
inline constexpr size_t kTerrainVisualHeaderWords = 15;
inline constexpr size_t kTerrainVisualHeaderBytes = 4 * kTerrainVisualHeaderWords;
// INFERRED: header word 3 is a scattering cell's instance cap (every type of tree level 4 or deeper asks for less on
// all 24 MP terrains). Settle: a note on the VisualTerrain header words (R5-VG2 leaves Q-SCATTER-DENSITY open).
inline constexpr size_t kTerrainVisualCellInstancesWord = 3;
// SOURCE: notes R5-VG2 C (the scatter record after the mesh name is 70 bytes).
inline constexpr size_t kTerrainScatterRecordBytes = 70;
inline constexpr uint32_t kMaxTerrainVisualLayers = 64;
inline constexpr uint32_t kMaxTerrainScatterTypesPerLayer = 256;
inline constexpr uint8_t kMaxTerrainScatterTreeLevel = 16;

struct TerrainScatterType {
	uint8_t layer = 0;
	std::string mesh;
	// INFERRED: instances per square metre; notes R5-VG2 C give the field but not its unit.
	// Settle: a note on how the density makes a cell's instance count (R5-VG2 Q-SCATTER-DENSITY).
	float density = 0.0f;
	std::array<float, 2> scale_min{};
	std::array<float, 2> scale_max{};
	uint8_t tree_level = 0;
	// STAND-IN: the record's other fields stay unread, R5-VG2 C's wind amount @49, shadow flag and distance @61 and @62,
	// billboard @67 and instance data mode @68 among them. Settle: read those fields into the scatter.
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
