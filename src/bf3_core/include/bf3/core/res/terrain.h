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

class TerrainStreamingTree {
public:
	std::optional<HeightfieldTree> heightfield;
	std::vector<TerrainStreamingNode> streaming_nodes;

	static ResError parse(Bytes p_res, TerrainStreamingTree &r_tree);

	ResError height_grid(uint8_t p_depth, const std::function<std::optional<std::vector<uint8_t>>(const ChunkId &)> &p_fetch, HeightGrid &r_grid) const;
};

}
