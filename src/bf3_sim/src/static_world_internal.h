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

#include <Jolt/Math/Quat.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/Shape/SubShapeID.h>

#include <array>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace bf3::core {
struct CachedItem;
}
namespace bf3::core::level {
struct CollisionSoup;
struct SoupTile;
} // namespace bf3::core::level

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
	// TC (docs/design/terrain-collision.md §5.2): one per Built soup terrain, soup order. The field (a
	// HeightFieldShape, null when no sample has collision) sits at its body transform (position, rotation: the
	// terrain frame without its uniform scale, which the field's scale holds); the patch (a MeshShape with one tag,
	// static_terrain_tag(), null when empty) holds world-space vertices and sits at the identity.
	struct Terrain {
		StaticTerrainInfo info;
		JPH::RefConst<JPH::Shape> field;
		JPH::RefConst<JPH::Shape> patch;
		Vec3 position{};
		std::array<float, 4> rotation{ 0.0f, 0.0f, 0.0f, 1.0f }; // quaternion x, y, z, w
	};
	std::vector<Terrain> terrains;
	StaticWorldStats stats;
	Vec3 min{};
	Vec3 max{};
};

// Builds StaticWorldShapes from finished parts (the friend of StaticWorldShapes).
struct StaticWorldBuilder {
	// Sorts nothing: p_impl->tiles must already be in (x, z) order. Fills min, max and the stats' totals (tiles,
	// shapes, triangles, tags, shape_bytes; TC: terrains, terrain_bytes, and the terrains' bounds) from the tiles and
	// terrains; seconds_total is the sum of the soup, cook and restore times unless the caller set it already (a
	// measured wall time).
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

// TC: cook_static_tiles on the build lanes and cook_static_terrains on one more lane beside them; the Impl carries
// the terrains, their stats (terrains_hit / cooked, seconds_terrain, cache bytes). r_stored: both all in the cache.
core::Result<std::unique_ptr<StaticWorldShapes::Impl>> cook_static_tiles_and_terrains(core::Cache *p_cache,
		const std::vector<std::array<int32_t, 2>> &p_keys, const StaticTileSource &p_source, std::span<const StaticTerrainView> p_terrains,
		const StaticWorldBuildOptions &p_options, std::shared_ptr<void> p_runtime, bool *r_stored);
// TC: the views of a soup's Built terrains (soup order); an error when a Built terrain has no record or past
// kMaxStaticTerrains.
core::Result<std::vector<StaticTerrainView>> soup_terrain_views(const core::level::CollisionSoup &p_soup);

// The loose-triangle build through a cache (sim.world.unit's synthetic cold / warm cases; tools). TC: p_terrains are
// cooked beside the tiles (through their "jolt-terrain" entries when p_cache is open).
core::Result<std::shared_ptr<const StaticWorldShapes>> build_static_world_cached(core::Cache *p_cache, std::span<const StaticTriangle> p_triangles,
		const StaticWorldBuildOptions &p_options = {}, std::span<const StaticTerrainView> p_terrains = {});

// ---- TC: terrain fields (static_world_terrain.cpp) --------------------------------------------------------------

// The field, patch, info and body transform of one terrain view. p_content is its static_terrain_content when the
// caller has it (else computed). Every visited sample and cell is charged to a WorkBudget of kMaxGridSide^2 x 2 units
// per terrain. Fails on a bad view (sizes, non-finite spacing or origin, a frame that is not a rotation times a
// positive uniform scale, a height span past 1e6 m) or a Jolt error. Needs the Jolt runtime held by the caller.
core::Result<StaticWorldShapes::Impl::Terrain> cook_static_terrain(const StaticTerrainView &p_view, const StaticWorldBuildOptions &p_options,
		const core::Sha256Digest *p_content = nullptr);
// Every view on the calling lane, in order, each through its "jolt-terrain" entry when p_cache is open (a hit decodes
// and must describe the same content; a miss cooks and puts). r_stored (optional) is set to whether every terrain is
// in the cache afterwards; r_bytes (optional) gets the blob bytes read or written; r_hit (optional) the hits.
core::Result<std::vector<StaticWorldShapes::Impl::Terrain>> cook_static_terrains(core::Cache *p_cache, std::span<const StaticTerrainView> p_views,
		const StaticWorldBuildOptions &p_options, bool *r_stored = nullptr, uint64_t *r_bytes = nullptr, size_t *r_hit = nullptr);
// "OBF3JTER" of one terrain and its decoder (layout and checks in static_world_cache.cpp).
std::vector<uint8_t> encode_static_terrain(const StaticWorldShapes::Impl::Terrain &p_terrain);
core::Result<StaticWorldShapes::Impl::Terrain> decode_static_terrain(std::span<const uint8_t> p_blob);

// TC (docs/design/terrain-collision.md §5.3): the static bodies every world over p_world makes, in creation order:
// one per present (tile, layer) in tile then layer order (shape at static_tile_origin, identity rotation, layer =
// the layer, user data tile * 4 + layer), then per terrain its field and its patch when present (layer Collision,
// user data terrain * 4 + kStaticTerrainBodyLayer). CollisionWorld and VehicleWorld both create exactly these.
struct StaticBodySpec {
	const JPH::Shape *shape = nullptr;
	JPH::Vec3 position = JPH::Vec3::sZero();
	JPH::Quat rotation = JPH::Quat::sIdentity();
	uint32_t layer = 0; // object layer = StaticLayer
	uint64_t user = 0;
};
std::vector<StaticBodySpec> static_body_specs(const StaticWorldShapes::Impl &p_world);
// The world-space face normal (not turned) of sub-shape p_sub of static body p_shape whose user data is p_user (a
// terrain body: the field rotated by its body rotation, or the patch); +Y when the body is not one of p_world's.
JPH::Vec3 static_face_normal(const StaticWorldShapes::Impl &p_world, uint64_t p_user, const JPH::Shape *p_shape, const JPH::SubShapeID &p_sub);

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
	// TC ("OBF3JWLD" v2, kStaticWorldIndexVersion): per Built terrain, soup order, its object and content digest.
	struct Terrain {
		uint32_t object = 0;
		core::Sha256Digest content{};
	};
	std::vector<Terrain> terrains;
};
std::vector<uint8_t> encode_static_world_index(const StaticWorldIndex &p_index);
core::Result<StaticWorldIndex> decode_static_world_index(std::span<const uint8_t> p_blob);

// SHA-256 over a tile list (sorted and made unique first; i32 x, i32 z little endian each): the nav-world key's
// "region=" part (S1's terrain_cover_digest, renamed by TC with the same bytes, so region keys are unchanged).
core::Sha256Digest tile_list_digest(std::span<const std::array<int32_t, 2>> p_tiles);
// SHA-256 over water planes in order (f32 y, u32 point count, f32 x, f32 z per point, little endian bit patterns):
// the nav-world key's "water=" part (TC: NavBakeOptions::water; S1's cover_water_digest, same bytes).
core::Sha256Digest water_planes_digest(std::span<const core::level::WaterPlane> p_water);

} // namespace bf3::sim
