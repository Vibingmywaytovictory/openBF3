#pragma once
// bf3_sim internal (Jolt stays PRIVATE to bf3_sim): StaticWorldShapes::Impl and the tile cook shared by the soup
// path, the loose-triangle path and the cache path (docs/design/m1.md §5.1, §5.3, §5.5).
//
// Body frame: the shapes of tile (x, z) hold their vertices relative to static_tile_origin(x, z), the tile's centre
// at y = 0, and every CollisionWorld places that tile's bodies there with the identity rotation (floats are most
// accurate near the origin; the rotation being identity makes a shape's local normals world normals).
// Triangle user data = an index into the tile's tag table (MeshShapeSettings::mPerTriangleUserData).

#include "bf3/sim/static_world.h"

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <array>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace bf3::core {
struct CachedItem;
}
namespace bf3::core::level {
struct SoupTile;
}

namespace bf3::sim {

inline constexpr float kStaticTileSize = 64.0f; // the soup's tile key (bf3_core collision_soup.h)

JPH::Vec3 static_tile_origin(int32_t p_x, int32_t p_z);

struct StaticWorldShapes::Impl {
	// Keeps the Jolt runtime (jolt_runtime.h) alive while any shape exists. Declared first: destroyed last.
	std::shared_ptr<void> runtime;

	struct Tile {
		StaticTileInfo info;
		std::vector<SurfaceTag> tags; // the triangles' user data indexes this (first-appearance order)
		std::array<JPH::RefConst<JPH::Shape>, kStaticLayerCount> shapes; // a MeshShape per present layer
	};
	std::vector<Tile> tiles; // sorted by (x, z)
	StaticWorldStats stats;
	Vec3 min{};
	Vec3 max{};
};

// Builds StaticWorldShapes from finished parts (the friend of StaticWorldShapes).
struct StaticWorldBuilder {
	// Sorts nothing: p_impl->tiles must already be in (x, z) order. Fills min, max and the stats' totals (tiles,
	// shapes, triangles, tags, shape_bytes) from the tiles; seconds_total is the sum of the soup, cook and restore
	// times unless the caller set it already (a measured wall time).
	static std::shared_ptr<const StaticWorldShapes> finish(std::unique_ptr<StaticWorldShapes::Impl> p_impl);
};

// A tile's input before the cook: the tag table, each triangle's tag index and the info (bounds, content digest,
// tag count; triangles[] still 0). Fails on a bad layer or a non-finite corner.
struct StaticTileInput {
	StaticTileInfo info;
	std::vector<SurfaceTag> tags;
	std::vector<uint32_t> triangle_tags; // per input triangle
};
core::Result<StaticTileInput> prepare_static_tile(int32_t p_x, int32_t p_z, std::span<const StaticTriangle> p_triangles);
// The MeshShapes of a prepared tile (one per present layer). Needs the Jolt runtime held by the caller.
core::Result<StaticWorldShapes::Impl::Tile> cook_static_tile(StaticTileInput p_input, std::span<const StaticTriangle> p_triangles,
		const StaticWorldBuildOptions &p_options);
// Both steps: the tag table, the content digest (StaticTileInfo::content, over every input triangle, including a
// Raycast layer that p_options leaves out), the bounds and one MeshShape per present layer. Fails on a bad layer,
// non-finite corners or a Jolt build error.
core::Result<StaticWorldShapes::Impl::Tile> cook_static_tile(int32_t p_x, int32_t p_z, std::span<const StaticTriangle> p_triangles,
		const StaticWorldBuildOptions &p_options);

// A soup tile's triangles as StaticTriangles (layer = body). Fails on a body with no layer.
core::Status soup_tile_triangles(const core::level::SoupTile &p_tile, std::vector<StaticTriangle> &r_out);

// Tile i's triangles (a span valid until the next call on the same lane, which may use r_scratch).
using StaticTileSource = std::function<core::Result<std::span<const StaticTriangle>>(size_t p_tile, std::vector<StaticTriangle> &r_scratch)>;

// Every tile of p_keys on the build lanes. With p_cache (open), each tile goes through its "jolt-tile" entry
// (content-keyed: a hit restores, a miss cooks and puts); without, every tile is cooked. r_stored (optional) is
// set to whether every tile is in the cache afterwards. The first error in tile order wins. The Impl is returned
// unfinished so the caller can add its own times.
core::Result<std::unique_ptr<StaticWorldShapes::Impl>> cook_static_tiles(core::Cache *p_cache, const std::vector<std::array<int32_t, 2>> &p_keys,
		const StaticTileSource &p_source, const StaticWorldBuildOptions &p_options, std::shared_ptr<void> p_runtime, bool *r_stored = nullptr);

// The loose-triangle build through a cache (sim.world.unit's synthetic cold / warm cases; tools).
core::Result<std::shared_ptr<const StaticWorldShapes>> build_static_world_cached(core::Cache *p_cache, std::span<const StaticTriangle> p_triangles,
		const StaticWorldBuildOptions &p_options = {});

// ---- the cache blobs (static_world_cache.cpp) -----------------------------------------------------------------

// "OBF3JTIL" v1 of one tile (layout in static_world_cache.cpp).
std::vector<uint8_t> encode_static_tile(const StaticWorldShapes::Impl::Tile &p_tile);
// Checks every count against the bytes left, validates each Jolt tree (node and triangle-block offsets, vertex
// indices, the tree depth Jolt's walk stack allows, user data inside the tag table) before Jolt sees it, and
// refuses trailing bytes. Needs the Jolt runtime held by the caller.
core::Result<StaticWorldShapes::Impl::Tile> decode_static_tile(std::span<const uint8_t> p_blob);

// One tile of a "jolt-world" entry.
struct StaticWorldEntry {
	int32_t x = 0;
	int32_t z = 0;
	core::Sha256Digest content{};
	std::array<uint32_t, kStaticLayerCount> triangles{};
	uint32_t tags = 0;
};
struct StaticWorldIndex {
	std::vector<StaticWorldEntry> tiles; // strictly increasing (x, z)
	uint64_t soup_sum = 0; // CollisionSoup::sum
	uint64_t soup_triangles = 0; // CollisionSoup::triangles
};
std::vector<uint8_t> encode_static_world_index(const StaticWorldIndex &p_index);
core::Result<StaticWorldIndex> decode_static_world_index(std::span<const uint8_t> p_blob);

} // namespace bf3::sim
