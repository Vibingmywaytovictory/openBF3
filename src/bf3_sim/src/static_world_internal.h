#pragma once

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
}

namespace bf3::sim {

inline constexpr float kStaticTileSize = 64.0f;

JPH::Vec3 static_tile_origin(int32_t p_x, int32_t p_z);

struct StaticWorldShapes::Impl {

	std::shared_ptr<void> runtime;

	struct Tile {
		StaticTileInfo info;
		std::vector<SurfaceTag> tags;
		std::array<JPH::RefConst<JPH::Shape>, kStaticLayerCount> shapes;
	};
	std::vector<Tile> tiles;

	struct Terrain {
		StaticTerrainInfo info;
		JPH::RefConst<JPH::Shape> field;
		JPH::RefConst<JPH::Shape> patch;
		Vec3 position{};
		std::array<float, 4> rotation{ 0.0f, 0.0f, 0.0f, 1.0f };
	};
	std::vector<Terrain> terrains;
	StaticWorldStats stats;
	Vec3 min{};
	Vec3 max{};
};

struct StaticWorldBuilder {

	static std::shared_ptr<const StaticWorldShapes> finish(std::unique_ptr<StaticWorldShapes::Impl> p_impl);
};

struct StaticTileInput {
	StaticTileInfo info;
	std::vector<SurfaceTag> tags;
	std::vector<uint32_t> triangle_tags;
};
core::Result<StaticTileInput> prepare_static_tile(int32_t p_x, int32_t p_z, std::span<const StaticTriangle> p_triangles);

core::Result<StaticWorldShapes::Impl::Tile> cook_static_tile(StaticTileInput p_input, std::span<const StaticTriangle> p_triangles,
		const StaticWorldBuildOptions &p_options);

core::Result<StaticWorldShapes::Impl::Tile> cook_static_tile(int32_t p_x, int32_t p_z, std::span<const StaticTriangle> p_triangles,
		const StaticWorldBuildOptions &p_options);

core::Status soup_tile_triangles(const core::level::SoupTile &p_tile, std::vector<StaticTriangle> &r_out);

using StaticTileSource = std::function<core::Result<std::span<const StaticTriangle>>(size_t p_tile, std::vector<StaticTriangle> &r_scratch)>;

core::Result<std::unique_ptr<StaticWorldShapes::Impl>> cook_static_tiles(core::Cache *p_cache, const std::vector<std::array<int32_t, 2>> &p_keys,
		const StaticTileSource &p_source, const StaticWorldBuildOptions &p_options, std::shared_ptr<void> p_runtime, bool *r_stored = nullptr);

core::Result<std::unique_ptr<StaticWorldShapes::Impl>> cook_static_tiles_and_terrains(core::Cache *p_cache,
		const std::vector<std::array<int32_t, 2>> &p_keys, const StaticTileSource &p_source, std::span<const StaticTerrainView> p_terrains,
		const StaticWorldBuildOptions &p_options, std::shared_ptr<void> p_runtime, bool *r_stored);

core::Result<std::vector<StaticTerrainView>> soup_terrain_views(const core::level::CollisionSoup &p_soup);

core::Result<std::shared_ptr<const StaticWorldShapes>> build_static_world_cached(core::Cache *p_cache, std::span<const StaticTriangle> p_triangles,
		const StaticWorldBuildOptions &p_options = {}, std::span<const StaticTerrainView> p_terrains = {});

core::Result<StaticWorldShapes::Impl::Terrain> cook_static_terrain(const StaticTerrainView &p_view, const StaticWorldBuildOptions &p_options,
		const core::Sha256Digest *p_content = nullptr);

core::Result<std::vector<StaticWorldShapes::Impl::Terrain>> cook_static_terrains(core::Cache *p_cache, std::span<const StaticTerrainView> p_views,
		const StaticWorldBuildOptions &p_options, bool *r_stored = nullptr, uint64_t *r_bytes = nullptr, size_t *r_hit = nullptr);

std::vector<uint8_t> encode_static_terrain(const StaticWorldShapes::Impl::Terrain &p_terrain);
core::Result<StaticWorldShapes::Impl::Terrain> decode_static_terrain(std::span<const uint8_t> p_blob);

struct StaticBodySpec {
	const JPH::Shape *shape = nullptr;
	JPH::Vec3 position = JPH::Vec3::sZero();
	JPH::Quat rotation = JPH::Quat::sIdentity();
	uint32_t layer = 0;
	uint64_t user = 0;
};
std::vector<StaticBodySpec> static_body_specs(const StaticWorldShapes::Impl &p_world);

JPH::Vec3 static_face_normal(const StaticWorldShapes::Impl &p_world, uint64_t p_user, const JPH::Shape *p_shape, const JPH::SubShapeID &p_sub);

std::vector<uint8_t> encode_static_tile(const StaticWorldShapes::Impl::Tile &p_tile);

core::Result<StaticWorldShapes::Impl::Tile> decode_static_tile(std::span<const uint8_t> p_blob);

struct StaticWorldEntry {
	int32_t x = 0;
	int32_t z = 0;
	core::Sha256Digest content{};
	std::array<uint32_t, kStaticLayerCount> triangles{};
	uint32_t tags = 0;
};
struct StaticWorldIndex {
	std::vector<StaticWorldEntry> tiles;
	uint64_t soup_sum = 0;
	uint64_t soup_triangles = 0;

	struct Terrain {
		uint32_t object = 0;
		core::Sha256Digest content{};
	};
	std::vector<Terrain> terrains;
};
std::vector<uint8_t> encode_static_world_index(const StaticWorldIndex &p_index);
core::Result<StaticWorldIndex> decode_static_world_index(std::span<const uint8_t> p_blob);

core::Sha256Digest tile_list_digest(std::span<const std::array<int32_t, 2>> p_tiles);

core::Sha256Digest water_planes_digest(std::span<const core::level::WaterPlane> p_water);

}
