#pragma once
// The static level world in our own Jolt (plan D5, §2.4 step 3, §3.12 / D17 "64 m tiled Jolt shapes"; gap load
// §2.6; docs/design/m1.md §5). Godot-free; Jolt stays inside bf3_sim (this header names no Jolt type).
//
// Source: the collision soup (bf3_core level/collision_soup.h) of a level x mode: world-space triangles with a
// material value, a surface class, a rigid-body kind (collision, raycast, character) and flags, chunked into
// 64 m tiles. Convex Havok leaves arrive tessellated and the terrain as 2 m cells, so every piece is a triangle.
//
// Shapes: per (tile, layer) one JPH::MeshShape over that tile's triangles of that layer (layer = body kind;
// terrain cells are in the Collision layer), built with per-triangle user data = an index into the tile's
// SurfaceTag table. No welding, no clipping at tile borders (the soup's rule), active edges computed by Jolt.
// The shapes are immutable once built and are shared, ref-counted, by every CollisionWorld of the process: the
// server world and the client prediction world of a listen host hold one copy of the static shapes (plan D5).
//
// Cache (bf3_core Cache, docs §5.5): two kinds.
//   "jolt-tile"  one tile's shapes and tag table, Shape::SaveBinaryState streams. Key: sources none; params =
//                SHA-256 of the tile's exact input (every triangle's float bits, tag, layer, in soup order) +
//                the build parameters; converter kStaticWorldConverterVersion; jolt_version = jolt_build_id().
//                Content keyed, so modes and levels that share a tile share its entry.
//   "jolt-world" the tile list of one level x mode: (x, z, tile key) per tile + totals. Key: params = the
//                level x mode's source digest (bf3_core level/source_digest.h: manifest SHA1s of every bundle
//                the mode mounts) + bf3_core kCollisionSoupVersion + the soup options + the build parameters;
//                same versions. (The tile keys hash the soup's output, so they need no soup version.)
// Warm load: source digest -> "jolt-world" -> every "jolt-tile" -> sRestoreFromBinaryState; no soup, no Havok
// decode. Cold (or any tile missing): build the soup, cook the tiles on all lanes, put tiles then the world.

#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/digest.h"
#include "bf3/core/level/water_plane.h" // S1 water rule
#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {
class Cache;
}
namespace bf3::core::level {
struct CollisionSoup;
class LoadedLevel;
class World;
struct SoupProgress;
} // namespace bf3::core::level

namespace bf3::sim {

// One static Jolt object layer per Havok rigid-body kind (bf3_core level::CollisionBody, same numbers).
enum class StaticLayer : uint8_t {
	Collision = 0, // RBTypeCollision (and every static model group) + terrain: blocks soldiers, vehicles, rays
	Raycast = 1, // RBTypeRaycast: detailed shells for rays (bullets, sight); soldiers pass through
	Character = 2, // RBTypeCharacter: blocks soldiers only (invisible character collision)
};
inline constexpr size_t kStaticLayerCount = 3;
// The layer of a soup triangle: StaticLayer(body). The one rule the static world's tile cook and the S1 nav input
// (nav.h) both use, so the navmesh is baked from exactly the triangles the motor collides with (docs/design/s1.md
// §5.1). Defined by S1 in static_world.cpp; nullopt for a body past kStaticLayerCount (the cook's error case).
std::optional<StaticLayer> static_layer_of(uint8_t p_soup_body);

// Which static layers a query sees (bit = 1 << StaticLayer).
namespace query_mask {
inline constexpr uint32_t kCollision = 1u << 0;
inline constexpr uint32_t kRaycast = 1u << 1;
inline constexpr uint32_t kCharacter = 1u << 2;
inline constexpr uint32_t kAll = kCollision | kRaycast | kCharacter;
// What the soldier motor collides with: the character-collision set (plan §3.6 "exactly the set the character
// collides with"; R6).
inline constexpr uint32_t kMovement = kCollision | kCharacter;
// Bullets and line of sight [inferred, M2a decides]: Collision + Raycast.
inline constexpr uint32_t kProjectile = kCollision | kRaycast;
} // namespace query_mask

// What a static triangle is made of (the soup triangle's fields minus its corners).
struct SurfaceTag {
	uint32_t material = 0; // MaterialFlagsAndIndices (low 24 bits: MaterialGridData pair; bit 27 see-through)
	uint8_t surface = 0; // bf3_core level::surface:: class (footsteps, impacts)
	uint8_t body = 0; // bf3_core level::CollisionBody
	uint8_t flags = 0; // bf3_core level::kSoupSeeThrough | kSoupInvisible | kSoupTerrain
	uint8_t reserved = 0;

	bool operator==(const SurfaceTag &) const = default;
};

// A triangle for the synthetic worlds of the tests (and tools): world-space corners, wound like the soup.
struct StaticTriangle {
	std::array<Vec3, 3> v{};
	SurfaceTag tag;
	StaticLayer layer = StaticLayer::Collision;
};

struct StaticTileInfo {
	int32_t x = 0; // floor(centroid / 64 m), the soup's tile key
	int32_t z = 0;
	std::array<uint32_t, kStaticLayerCount> triangles{}; // per layer (0 = no shape for that layer)
	uint32_t tags = 0; // distinct SurfaceTags of the tile
	Vec3 min{}; // bounds over the tile's corners
	Vec3 max{};
	core::Sha256Digest content{}; // SHA-256 of the tile's input (the "jolt-tile" key's params part)
};

// ---- TC: the terrain as heightfields (docs/design/terrain-collision.md §5; declared here, defined by the TC
// implementation in static_world_terrain.cpp; nothing calls them before it lands) -----------------------------
// Each Built soup terrain becomes one JPH::HeightFieldShape over its whole grid (kStaticTerrainBits per sample,
// kStaticTerrainBlock samples a block), in the Collision layer, as one static body with user data
// terrain * 4 + kStaticTerrainBodyLayer (tile bodies use tile * 4 + layer, layer < 3). Jolt drops a triangle when one
// of its three samples is a hole; the soup drops the cell when one of its four is. Where the two differ (a cell whose
// only hole is corner b or c: 4 cells on retail [verified 2026-10-16]) the cook holes that cell's sample a in the
// field and puts the cells' triangles the field then lacks into a small patch MeshShape (a second body, same user
// data), so the static world's terrain is exactly the soup's cells. Hits on either carry static_terrain_tag().
inline constexpr uint32_t kStaticTerrainBits = 16; // measured: <= 1.1 mm error on 23 of 24 terrains, 11.1 mm on MP_013
inline constexpr uint32_t kStaticTerrainBlock = 4;
inline constexpr uint32_t kStaticTerrainBodyLayer = 3;
inline constexpr size_t kMaxStaticTerrains = 8; // core::level::kMaxSoupTerrains
inline constexpr const char *kStaticTerrainCacheKind = "jolt-terrain";
// The tag of every terrain triangle: material 0, surface dirt, body collision, kSoupTerrain (the soup's cells).
inline constexpr SurfaceTag static_terrain_tag() { return SurfaceTag{ 0, 6, 0, 4, 0 }; }

// A terrain grid as the static world takes it (a non-owning view; the soup's SoupTerrain::grid and frame, or a
// synthetic test grid). Samples X fastest; holes[i] != 0 marks a hole. The frame (axes = rows right, up, forward,
// with their scale; translation) must be a rotation times a positive uniform scale: anything else fails the build.
struct StaticTerrainView {
	uint32_t object = 0; // World::objects index (reported)
	size_t width = 0; // samples along X
	size_t height = 0; // samples along Z
	std::array<float, 2> origin_xz{};
	std::array<float, 2> spacing{};
	std::span<const float> heights;
	std::span<const uint8_t> holes;
	std::array<Vec3, 3> axes{ { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } } };
	Vec3 translation{};
};

struct StaticTerrainInfo {
	uint32_t object = 0;
	uint32_t width = 0; // samples
	uint32_t height = 0;
	uint32_t holes = 0; // hole samples of the grid
	uint32_t holed = 0; // samples the cook holed for Jolt's per-triangle rule (see above)
	uint32_t patch_triangles = 0; // triangles of the patch MeshShape
	float max_error = 0.0f; // largest |field height - grid height| over the non-hole samples (metres), measured at the cook
	Vec3 min{}; // world bounds
	Vec3 max{};
	core::Sha256Digest content{}; // SHA-256 of the view (sizes, origin, spacing, frame, every height's bits, holes)
};

struct StaticWorldStats {
	size_t tiles = 0;
	size_t shapes = 0; // MeshShapes (tiles x layers present)
	std::array<size_t, kStaticLayerCount> triangles{};
	size_t tags = 0;
	uint64_t shape_bytes = 0; // Jolt's own Shape::GetStats sizes, summed (resident estimate)
	uint64_t cache_bytes = 0; // blob bytes read or written
	size_t tiles_hit = 0; // tiles restored from the cache
	size_t tiles_cooked = 0; // tiles built from triangles this run
	bool world_hit = false; // the "jolt-world" entry was found (warm path, no soup built)
	double seconds_soup = 0.0; // build_collision_soup (cold path only)
	double seconds_cook = 0.0; // MeshShape builds (all lanes, wall time)
	double seconds_restore = 0.0; // tile restores (all lanes, wall time)
	double seconds_total = 0.0;
	// TC: the terrain fields (0 until the TC implementation).
	size_t terrains = 0;
	uint64_t terrain_bytes = 0; // the fields' and patches' GetStats sizes (also in shape_bytes)
	size_t terrains_hit = 0; // restored from "jolt-terrain" entries
	size_t terrains_cooked = 0;
	double seconds_terrain = 0.0; // content digests + field cooks (wall time)
};

// The immutable shapes of one level's static world. Shared as std::shared_ptr<const StaticWorldShapes> by every
// CollisionWorld built over it, on any thread (nothing in it changes after the build).
class StaticWorldShapes {
public:
	~StaticWorldShapes();
	StaticWorldShapes(const StaticWorldShapes &) = delete;
	StaticWorldShapes &operator=(const StaticWorldShapes &) = delete;

	size_t tile_count() const;
	const StaticTileInfo &tile(size_t p_index) const; // sorted by (x, z), like the soup
	// The tag of triangle user-data value p_tag in tile p_tile (CollisionWorld hits carry both).
	const SurfaceTag &tag(size_t p_tile, uint32_t p_tag) const;
	const StaticWorldStats &stats() const;
	// TC: the terrains (soup terrain order, Built only); 0 until the TC implementation.
	size_t terrain_count() const;
	const StaticTerrainInfo &terrain(size_t p_index) const;
	// Bounds of every tile.
	Vec3 min() const;
	Vec3 max() const;

	struct Impl; // bf3_sim internal (src/static_world_internal.h): the JPH::Shape references per tile and layer
	const Impl &impl() const { return *impl_; }

private:
	friend struct StaticWorldBuilder; // static_world.cpp
	explicit StaticWorldShapes(std::unique_ptr<Impl> p_impl);
	std::unique_ptr<Impl> impl_;
};

struct StaticWorldBuildOptions {
	unsigned threads = 0; // cook lanes; 0 = all cores
	// MeshShapeSettings::mActiveEdgeCosThresholdAngle as degrees (Jolt default 5 degrees).
	float active_edge_degrees = 5.0f;
	bool include_raycast = true; // build the Raycast layer (movement never needs it)
	std::atomic<size_t> *tiles_done = nullptr; // progress for the loading screen (optional)
};

// Format and converter versions of the cache entries (bump the converter when the same soup builds different
// shapes: winding, layer split, active edges, tag table).
inline constexpr uint32_t kStaticWorldFormatVersion = 1;
inline constexpr uint32_t kStaticWorldConverterVersion = 1;
inline constexpr const char *kStaticTileCacheKind = "jolt-tile";
inline constexpr const char *kStaticWorldCacheKind = "jolt-world";
// "5.6.0;det=1;dp=0": the Jolt version and the build switches that change a saved shape stream
// (CROSS_PLATFORM_DETERMINISTIC, double precision). The jolt_version of every cache key above.
std::string jolt_build_id();

// Shapes from a soup (every built tile; no cache).
core::Result<std::shared_ptr<const StaticWorldShapes>> build_static_world(const core::level::CollisionSoup &p_soup,
		const StaticWorldBuildOptions &p_options = {});
// Shapes from loose triangles, tiled like the soup (synthetic worlds for tests and tools).
core::Result<std::shared_ptr<const StaticWorldShapes>> build_static_world(std::span<const StaticTriangle> p_triangles,
		const StaticWorldBuildOptions &p_options = {});
// TC: the same with terrain grids beside the triangles (synthetic worlds; the soup overload uses the soup's records).
// A separate name, so a braced second argument never makes the two overloads above ambiguous.
core::Result<std::shared_ptr<const StaticWorldShapes>> build_static_world_terrain(std::span<const StaticTriangle> p_triangles,
		std::span<const StaticTerrainView> p_terrains, const StaticWorldBuildOptions &p_options = {});

struct StaticWorldLoadOptions {
	StaticWorldBuildOptions build;
	unsigned soup_threads = 0; // SoupOptions::threads for the cold path
	float terrain_spacing = 2.0f; // SoupOptions::terrain_spacing (a key parameter)
	core::level::SoupProgress *soup_progress = nullptr; // cold path stage display (optional)
	// S1 (docs/design/s1.md §5.2): SoupOptions::terrain_cover for the cold path (the level's combat-area tiles). A
	// non-empty cover is a key parameter of the "jolt-world" entry ("cover=<SHA-256 of the sorted tile list>"); an
	// empty one leaves the key text exactly as M1 wrote it, so M1's entries stay valid.
	std::vector<std::array<int32_t, 2>> terrain_cover;
	// S1 water rule (SoupOptions::cover_water; docs §5.2): with a non-empty cover, a non-empty list adds
	// ";water=<SHA-256 of the planes>" to the key. Ignored without a cover.
	std::vector<core::level::WaterPlane> cover_water;
	// (TC removes terrain_cover and cover_water: the terrain is whole, the water rule is the nav bake's, nav.h.)
};

// The static world of a traversed level x mode through the cache (see the header comment). p_cache may be null
// (always cold, nothing stored). Touches p_world only on the calling thread and only on the cold path (the
// soup's collision placements); everything else runs on the build lanes.
core::Result<std::shared_ptr<const StaticWorldShapes>> load_static_world_cached(core::Cache *p_cache, const core::level::LoadedLevel &p_level,
		const core::level::World &p_world, const StaticWorldLoadOptions &p_options = {});

// ---- blobs (exposed for the tests: round trips, hostile input, key changes) --------------------------------
// "OBF3JTIL" v1: tile x, z, content digest, bounds, tag table, then per layer present: layer id, input and Jolt
// triangle counts, the Shape::SaveBinaryState stream of its MeshShape (no children, no materials; the layout is in
// src/static_world_cache.cpp). Decoding checks every count against the bytes left, walks the whole Jolt tree
// (offsets, depth, vertex indices, the tag indices the shape can name) before Jolt restores it, and refuses
// trailing bytes. "OBF3JWLD" v1: tile count, per tile (x, z, content digest, per-layer
// triangle counts, tag count), the soup's sum and triangle total.
std::vector<uint8_t> encode_static_tile_blob(const StaticWorldShapes &p_world, size_t p_tile);
core::Result<std::shared_ptr<const StaticWorldShapes>> decode_static_tile_blobs(std::span<const std::span<const uint8_t>> p_tiles);
core::CacheKeyInputs static_tile_key_inputs(const core::Sha256Digest &p_content, const StaticWorldBuildOptions &p_options);
// TC: "OBF3JTER" v1: the terrain's StaticTerrainInfo, then the HeightFieldShape's Shape::SaveBinaryState stream,
// then the patch MeshShape's (or none). Decoding checks the field header before Jolt sees it (sample count
// 2..kMaxGridSide rounded up to the block, block 2..8, bits 1..16, and the range-block, sample and active-edge byte
// counts HeightFieldShape::AllocateBuffers derives from them against the bytes left), walks the patch like a tile's
// MeshShape, and refuses trailing bytes. Key: params = the content digest + kStaticTerrainBits / kStaticTerrainBlock +
// the build parameters; content keyed, so the modes of a level share the entry. "OBF3JWLD" becomes v2 (the
// terrain digests after the tiles; kStaticWorldFormatVersion 2).
std::vector<uint8_t> encode_static_terrain_blob(const StaticWorldShapes &p_world, size_t p_terrain);
core::Result<std::shared_ptr<const StaticWorldShapes>> decode_static_terrain_blob(std::span<const uint8_t> p_blob);
core::CacheKeyInputs static_terrain_key_inputs(const core::Sha256Digest &p_content, const StaticWorldBuildOptions &p_options);
core::CacheKeyInputs static_world_key_inputs(const core::Sha256Digest &p_source_digest, std::string_view p_level, std::string_view p_mode,
		const StaticWorldLoadOptions &p_options);

} // namespace bf3::sim
