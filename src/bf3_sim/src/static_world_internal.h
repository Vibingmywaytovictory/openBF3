#pragma once
// bf3_sim internal (Jolt stays PRIVATE to bf3_sim): StaticWorldShapes::Impl and the tile cook shared by the soup
// path, the loose-triangle path and (T1 phase 2) the cache path (docs/design/m1.md §5.1, §5.3, §5.5).
//
// Body frame: the shapes of tile (x, z) hold their vertices relative to static_tile_origin(x, z), the tile's centre
// at y = 0, and every CollisionWorld places that tile's bodies there with the identity rotation (floats are most
// accurate near the origin; the rotation being identity makes a shape's local normals world normals).
// Triangle user data = an index into the tile's tag table (MeshShapeSettings::mPerTriangleUserData).

#include "bf3/sim/static_world.h"

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <array>
#include <memory>
#include <span>
#include <vector>

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
	// shapes, triangles, tags, shape_bytes) from the tiles.
	static std::shared_ptr<const StaticWorldShapes> finish(std::unique_ptr<StaticWorldShapes::Impl> p_impl);
};

// Cooks one tile from its triangles in soup order: the tag table, the content digest (StaticTileInfo::content,
// over every input triangle, including a Raycast layer that p_options leaves out), the bounds and one MeshShape per
// present layer. Needs the Jolt runtime held by the caller. Fails on a bad layer, non-finite corners or a Jolt
// build error.
core::Result<StaticWorldShapes::Impl::Tile> cook_static_tile(int32_t p_x, int32_t p_z, std::span<const StaticTriangle> p_triangles,
		const StaticWorldBuildOptions &p_options);

} // namespace bf3::sim
