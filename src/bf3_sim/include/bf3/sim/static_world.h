#pragma once

#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/digest.h"
#include "bf3/core/level/water_plane.h"
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
struct SoupTerrain;
}

namespace bf3::sim {

enum class StaticLayer : uint8_t {
	Collision = 0,
	Raycast = 1,
	Character = 2,
};
inline constexpr size_t kStaticLayerCount = 3;

std::optional<StaticLayer> static_layer_of(uint8_t p_soup_body);

namespace query_mask {
inline constexpr uint32_t kCollision = 1u << 0;
inline constexpr uint32_t kRaycast = 1u << 1;
inline constexpr uint32_t kCharacter = 1u << 2;
inline constexpr uint32_t kAll = kCollision | kRaycast | kCharacter;

inline constexpr uint32_t kMovement = kCollision | kCharacter;

inline constexpr uint32_t kProjectile = kCollision | kRaycast;
}

struct SurfaceTag {
	uint32_t material = 0;
	uint8_t surface = 0;
	uint8_t body = 0;
	uint8_t flags = 0;
	uint8_t reserved = 0;

	bool operator==(const SurfaceTag &) const = default;
};

struct StaticTriangle {
	std::array<Vec3, 3> v{};
	SurfaceTag tag;
	StaticLayer layer = StaticLayer::Collision;
};

struct StaticTileInfo {
	int32_t x = 0;
	int32_t z = 0;
	std::array<uint32_t, kStaticLayerCount> triangles{};
	uint32_t tags = 0;
	Vec3 min{};
	Vec3 max{};
	core::Sha256Digest content{};
};

inline constexpr uint32_t kStaticTerrainBits = 16;
inline constexpr uint32_t kStaticTerrainBlock = 4;
inline constexpr uint32_t kStaticTerrainBodyLayer = 3;
inline constexpr size_t kMaxStaticTerrains = 8;
inline constexpr const char *kStaticTerrainCacheKind = "jolt-terrain";
inline constexpr uint32_t kStaticTerrainFormatVersion = 1;

inline constexpr SurfaceTag static_terrain_tag() { return SurfaceTag{ 0, 6, 0, 4, 0 }; }

struct StaticTerrainView {
	uint32_t object = 0;
	size_t width = 0;
	size_t height = 0;
	std::array<float, 2> origin_xz{};
	std::array<float, 2> spacing{};
	std::span<const float> heights;
	std::span<const uint8_t> holes;
	std::array<Vec3, 3> axes{ { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } } };
	Vec3 translation{};
};

struct StaticTerrainInfo {
	uint32_t object = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t holes = 0;
	uint32_t holed = 0;
	uint32_t patch_triangles = 0;
	float max_error = 0.0f;
	Vec3 min{};
	Vec3 max{};
	core::Sha256Digest content{};
};

core::Result<StaticTerrainView> static_terrain_view(const core::level::SoupTerrain &p_terrain);

core::Sha256Digest static_terrain_content(const StaticTerrainView &p_view);

struct StaticWorldStats {
	size_t tiles = 0;
	size_t shapes = 0;
	std::array<size_t, kStaticLayerCount> triangles{};
	size_t tags = 0;
	uint64_t shape_bytes = 0;
	uint64_t cache_bytes = 0;
	size_t tiles_hit = 0;
	size_t tiles_cooked = 0;
	bool world_hit = false;
	double seconds_soup = 0.0;
	double seconds_cook = 0.0;
	double seconds_restore = 0.0;
	double seconds_total = 0.0;

	size_t terrains = 0;
	uint64_t terrain_bytes = 0;
	size_t terrains_hit = 0;
	size_t terrains_cooked = 0;
	double seconds_terrain = 0.0;
};

class StaticWorldShapes {
public:
	~StaticWorldShapes();
	StaticWorldShapes(const StaticWorldShapes &) = delete;
	StaticWorldShapes &operator=(const StaticWorldShapes &) = delete;

	size_t tile_count() const;
	const StaticTileInfo &tile(size_t p_index) const;

	const SurfaceTag &tag(size_t p_tile, uint32_t p_tag) const;
	const StaticWorldStats &stats() const;

	size_t terrain_count() const;
	const StaticTerrainInfo &terrain(size_t p_index) const;

	Vec3 min() const;
	Vec3 max() const;

	struct Impl;
	const Impl &impl() const { return *impl_; }

private:
	friend struct StaticWorldBuilder;
	explicit StaticWorldShapes(std::unique_ptr<Impl> p_impl);
	std::unique_ptr<Impl> impl_;
};

struct StaticWorldBuildOptions {
	unsigned threads = 0;

	float active_edge_degrees = 5.0f;
	bool include_raycast = true;
	std::atomic<size_t> *tiles_done = nullptr;
};

inline constexpr uint32_t kStaticWorldFormatVersion = 1;
inline constexpr uint32_t kStaticWorldConverterVersion = 1;

inline constexpr uint32_t kStaticWorldIndexVersion = 2;
inline constexpr const char *kStaticTileCacheKind = "jolt-tile";
inline constexpr const char *kStaticWorldCacheKind = "jolt-world";

std::string jolt_build_id();

core::Result<std::shared_ptr<const StaticWorldShapes>> build_static_world(const core::level::CollisionSoup &p_soup,
		const StaticWorldBuildOptions &p_options = {});

core::Result<std::shared_ptr<const StaticWorldShapes>> build_static_world(std::span<const StaticTriangle> p_triangles,
		const StaticWorldBuildOptions &p_options = {});

core::Result<std::shared_ptr<const StaticWorldShapes>> build_static_world_terrain(std::span<const StaticTriangle> p_triangles,
		std::span<const StaticTerrainView> p_terrains, const StaticWorldBuildOptions &p_options = {});

struct StaticWorldLoadOptions {
	StaticWorldBuildOptions build;
	unsigned soup_threads = 0;
	float terrain_spacing = 2.0f;
	core::level::SoupProgress *soup_progress = nullptr;

};

core::Result<std::shared_ptr<const StaticWorldShapes>> load_static_world_cached(core::Cache *p_cache, const core::level::LoadedLevel &p_level,
		const core::level::World &p_world, const StaticWorldLoadOptions &p_options = {});

std::vector<uint8_t> encode_static_tile_blob(const StaticWorldShapes &p_world, size_t p_tile);
core::Result<std::shared_ptr<const StaticWorldShapes>> decode_static_tile_blobs(std::span<const std::span<const uint8_t>> p_tiles);
core::CacheKeyInputs static_tile_key_inputs(const core::Sha256Digest &p_content, const StaticWorldBuildOptions &p_options);

std::vector<uint8_t> encode_static_terrain_blob(const StaticWorldShapes &p_world, size_t p_terrain);
core::Result<std::shared_ptr<const StaticWorldShapes>> decode_static_terrain_blob(std::span<const uint8_t> p_blob);
core::CacheKeyInputs static_terrain_key_inputs(const core::Sha256Digest &p_content, const StaticWorldBuildOptions &p_options);
core::CacheKeyInputs static_world_key_inputs(const core::Sha256Digest &p_source_digest, std::string_view p_level, std::string_view p_mode,
		const StaticWorldLoadOptions &p_options);

}
