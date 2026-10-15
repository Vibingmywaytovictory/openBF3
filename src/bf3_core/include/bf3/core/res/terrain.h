#pragma once
// Terrain (0x6BB6D7D2) and TerrainStreamingTree (0x22FE8AC8) res: the heightfield only. Port of the
// heightfield parts of IW4L resource_bf3 terrain.rs (Apache-2.0); the mask, colour and other raster
// trees are stepped over by their length. Operation order follows the Rust code, so the heights are
// bit-identical to its HeightGrid.
//
// Layout: the Terrain res body is the NUL-terminated name of its streaming tree res. The tree: a
// header (unblurred samples per node side, flags, detail factors, node count), then raster records
// (u8 kind, u32 length, payload) up to kind 0xFF, then the streaming quadtree (per node: lod0 chunk
// size and id, an optional lod1 chunk, a flag, then optionally its four children). The heightfield
// raster is its own quadtree: per node 6 floats of bounds, disabled / pruned / can-stream /
// persistent flags, a persistent node's embedded tile (samples_per_side^2 u16, X fastest, then its
// min-max stack and occluder grid), then optionally four children in the order (0,0) (1,0) (1,1)
// (0,1); x runs along world +X and y along world +Z. A streamed node's tile starts its lod0 chunk.
//
// Bounds: quadtree depth 16, samples_per_side 2..4096, and a height grid of at most kMaxGridSide
// samples a side; every read is bounds-checked.

#include "bf3/core/res/res_types.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace bf3::core::res {

inline constexpr uint32_t kResTypeTerrain = 0x6BB6D7D2u;
inline constexpr uint32_t kResTypeTerrainStreamingTree = 0x22FE8AC8u;
// Largest height grid side height_grid builds (IW4L collision/terrain.rs MAX_SIDE).
inline constexpr size_t kMaxGridSide = 4097;

// The Terrain res body: the name of its streaming tree res.
ResError terrain_streaming_tree_name(Bytes p_terrain_res, std::string &r_name);

struct TerrainNodeId {
	uint8_t level = 0;
	uint32_t x = 0; // along world +X
	uint32_t y = 0; // along world +Z
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
	std::optional<std::vector<uint16_t>> heights; // embedded tile
};

struct HeightfieldTree {
	uint32_t samples_per_side = 0; // border included
	float world_size_y = 0.0f; // height = sample * world_size_y / 65535
	float physics_meters_per_sample = 0.0f;
	uint32_t min_max_stack_depth = 0;
	uint32_t occluder_grid_stack_depth = 0;
	uint32_t border_width = 0;
	std::vector<HeightfieldNode> nodes; // depth-first, nodes[0] = root

	uint32_t interior_cells() const { return samples_per_side - 2 * border_width - 1; }
	uint8_t max_level() const;
	float height_metres(uint16_t p_sample) const;
};

struct TerrainStreamingNode {
	TerrainNodeId id;
	ChunkId lod0_chunk;
	uint32_t lod0_chunk_size = 0;
};

// Heights on a regular grid in Frostbite space.
struct HeightGrid {
	std::array<float, 2> origin_xz{}; // world (x, z) of sample (0, 0)
	std::array<float, 2> sample_spacing{}; // metres along X and Z
	size_t width = 0; // samples along X
	size_t height = 0; // samples along Z
	std::vector<float> heights; // metres, X fastest
	std::vector<uint8_t> holes; // 1 where the terrain is disabled or has no data

	float at(size_t p_x, size_t p_z) const { return heights[p_x + p_z * width]; }
	bool hole(size_t p_x, size_t p_z) const { return holes[p_x + p_z * width] != 0; }
};

class TerrainStreamingTree {
public:
	std::optional<HeightfieldTree> heightfield;
	std::vector<TerrainStreamingNode> streaming_nodes; // depth-first, [0] = root

	static ResError parse(Bytes p_res, TerrainStreamingTree &r_tree);

	// Heights of the whole terrain on the grid of quadtree level p_depth
	// (2^depth * interior_cells + 1 samples a side, at most kMaxGridSide). Tiles come from the res or,
	// for streamed nodes, from p_fetch (the decompressed lod0 chunk; nullopt = unavailable). A node
	// without data keeps its ancestor's heights bilinearly upsampled; disabled nodes are holes except
	// for the edges they share with nodes that have data; one deeper than p_depth holes only the
	// samples strictly inside it.
	ResError height_grid(uint8_t p_depth, const std::function<std::optional<std::vector<uint8_t>>(const ChunkId &)> &p_fetch, HeightGrid &r_grid) const;
};

} // namespace bf3::core::res
