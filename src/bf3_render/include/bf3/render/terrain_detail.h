#pragma once

#include "bf3/core/res/terrain.h"
#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bf3::render {

// STAND-IN: the fine mask and colour window is at most 1024 texels a side around the camera, in place of BF3's terrain
// streaming. Settle: the terrain streaming read.
inline constexpr uint32_t kTerrainDetailSide = 1024;

struct TerrainDetailLayout {
	core::res::TerrainRegion mask;
	uint32_t mask_columns = 1;
	uint32_t mask_tiles = 0;
	uint8_t mask_depth = 0;
	float data_mask_texel = 0.0f;
	std::array<float, 2> mask_grid_origin{};
	std::optional<core::res::TerrainRegion> colour;
	uint8_t colour_depth = 0;
	float data_colour_texel = 0.0f;
	std::array<float, 2> colour_grid_origin{};
	std::array<float, 4> region{};

	uint32_t mask_atlas_width() const { return mask_columns * (mask.width + 2); }
	uint32_t mask_atlas_height() const { return ((mask_tiles + mask_columns - 1) / mask_columns) * (mask.height + 2); }
};

struct TerrainDetailSource {
	core::res::TerrainStreamingTree tree;
	std::map<std::array<uint8_t, 16>, std::vector<uint8_t>> chunks;

	core::res::TerrainChunkFetch fetch() const;
	uint64_t bytes() const;
};

struct TerrainDetail {
	std::array<float, 2> centre{};
	core::res::TerrainRegion mask;
	uint32_t mask_atlas_width = 0;
	uint32_t mask_atlas_height = 0;
	std::vector<uint8_t> mask_atlas;
	core::res::TerrainRegion colour;
	std::vector<uint8_t> colour_rgba;

	std::array<float, 4> mask_window() const;
	std::array<float, 4> mask_size() const;
	std::array<float, 4> colour_window() const;
	std::array<float, 4> colour_size() const;
};

core::Result<TerrainDetail> build_terrain_detail(
		const TerrainDetailSource &p_source, const TerrainDetailLayout &p_layout, float p_x, float p_z);

bool terrain_detail_gains(const TerrainDetailLayout &p_layout);

bool terrain_detail_covers(const core::res::TerrainRegion &p_region, float p_x, float p_z);

float terrain_mask_texel_at(const TerrainDetailLayout &p_layout, const TerrainDetail *p_detail, float p_x, float p_z);
float terrain_colour_texel_at(const TerrainDetailLayout &p_layout, const TerrainDetail *p_detail, float p_x, float p_z);

struct TerrainDetailCode {
	std::string functions;
	std::string body;
};

core::Result<TerrainDetailCode> terrain_detail_code(
		std::span<const uint32_t> p_mask_slots, std::span<const uint32_t> p_colour_slots, std::string p_body);

}
