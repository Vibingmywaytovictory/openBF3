#pragma once

#include "bf3/core/res/res_types.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace bf3::core::res {

inline constexpr uint32_t kResTypeTerrain = 0x6BB6D7D2u;
inline constexpr uint32_t kResTypeTerrainStreamingTree = 0x22FE8AC8u;

inline constexpr size_t kMaxGridSide = 4097;

ResError terrain_streaming_tree_name(Bytes p_terrain_res, std::string &r_name);

struct TerrainNodeId {
	uint8_t level = 0;
	uint32_t x = 0;
	uint32_t y = 0;
	friend bool operator==(const TerrainNodeId &, const TerrainNodeId &) = default;
};

struct HeightfieldNode {
	TerrainNodeId id;
	Vec3 bounds_min{};
	Vec3 bounds_max{};
	bool disabled = false;
	bool pruned = false;
	bool can_stream = false;
	bool persistent = false;
	std::optional<std::vector<uint16_t>> heights;
};

struct HeightfieldTree {
	uint32_t samples_per_side = 0;
	float world_size_y = 0.0f;
	float physics_meters_per_sample = 0.0f;
	uint32_t min_max_stack_depth = 0;
	uint32_t occluder_grid_stack_depth = 0;
	uint32_t border_width = 0;
	std::vector<HeightfieldNode> nodes;

	uint32_t interior_cells() const { return samples_per_side - 2 * border_width - 1; }
	uint8_t max_level() const;
	float height_metres(uint16_t p_sample) const;
};

struct TerrainStreamingNode {
	TerrainNodeId id;
	ChunkId lod0_chunk;
	uint32_t lod0_chunk_size = 0;
	ChunkId children_chunk;
	uint32_t children_chunk_size = 0;
};

struct HeightGrid {
	std::array<float, 2> origin_xz{};
	std::array<float, 2> sample_spacing{};
	size_t width = 0;
	size_t height = 0;
	std::vector<float> heights;
	std::vector<uint8_t> holes;

	float at(size_t p_x, size_t p_z) const { return heights[p_x + p_z * width]; }
	bool hole(size_t p_x, size_t p_z) const { return holes[p_x + p_z * width] != 0; }
};

inline constexpr uint32_t kTerrainMaskBorder = 2;
inline constexpr uint32_t kTerrainColourBorder = 4;
inline constexpr uint8_t kTerrainFullMask = 255;

struct TerrainMaskRecord {
	uint8_t layer = 0;
	std::array<float, 4> rect{};
	bool refined = false;
	bool full = false;
	std::vector<uint8_t> coverage;
};

struct TerrainMaskNode {
	TerrainNodeId id;
	std::vector<TerrainMaskRecord> records;
	uint16_t tiles = 0;
	std::optional<std::vector<uint8_t>> persistent;
};

struct TerrainMaskTree {
	uint32_t samples_per_side = 0;
	std::array<float, 4> bounds{};
	std::vector<TerrainMaskNode> nodes;

	uint32_t tile_bytes() const { return samples_per_side * samples_per_side; }
	uint8_t layer_count() const;
	uint8_t max_level() const;
};

struct TerrainColourNode {
	TerrainNodeId id;
	bool can_stream = false;
	std::optional<std::vector<uint8_t>> persistent;
};

struct TerrainColourTree {
	uint32_t texels_per_side = 0;
	std::array<float, 4> bounds{};
	std::vector<TerrainColourNode> nodes;

	uint32_t tile_bytes() const { return texels_per_side * texels_per_side / 2; }
	uint8_t max_level() const;
};

struct TerrainRegion {
	std::array<float, 2> origin{};
	float texel = 1.0f;
	uint32_t width = 0;
	uint32_t height = 0;
};

struct TerrainChunkLayout {
	ChunkId chunk;
	uint64_t base = 0;
	uint64_t heights_bytes = 0;
	uint64_t mask_offset = 0;
	uint32_t mask_tiles = 0;
	std::optional<uint64_t> colour_offset;
	uint64_t total = 0;
};

using TerrainChunkFetch = std::function<std::optional<std::vector<uint8_t>>(const ChunkId &)>;

class TerrainStreamingTree {
public:
	std::optional<HeightfieldTree> heightfield;
	std::optional<TerrainMaskTree> mask;
	std::optional<TerrainColourTree> colour;
	std::vector<TerrainStreamingNode> streaming_nodes;

	static ResError parse(Bytes p_res, TerrainStreamingTree &r_tree);

	ResError height_grid(uint8_t p_depth, const TerrainChunkFetch &p_fetch, HeightGrid &r_grid) const;

	ResError chunk_layout(const TerrainNodeId &p_id, TerrainChunkLayout &r_layout) const;

	ResError mask_grids(const TerrainRegion &p_region, uint8_t p_depth, const TerrainChunkFetch &p_fetch,
			std::vector<std::vector<uint8_t>> &r_layers) const;

	ResError colour_grid(const TerrainRegion &p_region, uint8_t p_depth, const TerrainChunkFetch &p_fetch,
			std::vector<uint8_t> &r_rgba) const;
};

std::array<uint8_t, 64> decode_bc1_block(const uint8_t *p_block);
}
