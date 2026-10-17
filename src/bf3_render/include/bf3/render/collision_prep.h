#pragma once
// Collision soup (bf3_core level/collision_soup.h) -> vertex arrays for the Godot collision overlay
// (Bf3CollisionOverlay, README "Collision overlay"). Godot-free; runs on the loader thread, and the bridge only
// copies the arrays into Packed*Arrays and creates one RenderingServer mesh per (tile, layer) on the main thread.
//
// Conventions:
// * Frostbite and Godot are both Y up, metres: positions are copied 1:1.
// * Winding: soup triangles wind counter-clockwise seen from their outside (convex pieces; mesh triangles as
//   stored); Godot's front faces are clockwise, so each triangle is emitted as (v0, v2, v1). The overlay draws
//   both sides (cull disabled) and darkens back faces, so an inside-out face stays visible.
// * Unindexed: three vertices per triangle, so every face keeps its own flat colour and its own barycentric UVs
//   ((1,0), (0,1), (0,0) at the emitted corners) for the shader's wireframe overlay.
// * Colour: the surface class's colour (collision_surface_colour), shaded by the face's facing to a fixed light
//   (0.45 + 0.55 |n.l|) and varied +-6 % per placement so neighbouring objects separate; character bodies,
//   invisible blockers and the terrain get their own colours, see-through faces a lighter tint. RGBA floats,
//   alpha 1.

#include "bf3/core/level/collision_soup.h"

#include <array>
#include <cstdint>
#include <vector>

namespace bf3::render {

// The overlay's layers, toggled separately: terrain (kSoupTerrain) is layer 4; a triangle of an invisible
// blocker (kSoupInvisible: the mesh name says invisible / collisionmesh) is layer 3 whatever its body, else its
// body (collision 0, raycast 1, character 2).
inline constexpr size_t kCollisionLayers = 5;
inline uint8_t collision_layer(const core::level::SoupTriangle &p_triangle) {
	if ((p_triangle.flags & core::level::kSoupTerrain) != 0) {
		return 4;
	}
	if ((p_triangle.flags & core::level::kSoupInvisible) != 0) {
		return 3;
	}
	return uint8_t(p_triangle.body < 3 ? p_triangle.body : 0);
}

// Display colour of a surface class (linear-ish RGB, 0..1); grey for an unused id.
std::array<float, 3> collision_surface_colour(uint8_t p_surface);

struct PreparedCollisionMesh {
	int32_t tile_x = 0; // SoupTile::x / z
	int32_t tile_z = 0;
	uint8_t layer = 0; // collision_layer: one mesh per layer present in the tile
	size_t triangles = 0;
	std::vector<float> positions; // xyz, 3 vertices per triangle
	std::vector<float> colors; // rgba per vertex
	std::vector<float> uvs; // uv per vertex (barycentric corner)
	std::array<float, 3> min{}; // bounds of the positions
	std::array<float, 3> max{};
};

// One mesh per (tile, layer) with triangles, in tile order then layer order. Uses up to p_threads lanes
// (0 = all cores). TC: the soup's tiles hold the Havok triangles only, so this gives no terrain (layer 4); the
// overload below adds the terrain window.
std::vector<PreparedCollisionMesh> prepare_collision_meshes(const core::level::CollisionSoup &p_soup, unsigned p_threads = 0);

// TC (docs/design/terrain-collision.md §8): after TC the soup's tiles hold no terrain, and a whole terrain is up to
// 33.5 M triangles (3.6 GB as overlay vertices), so the overlay's terrain layer is a window: the terrain triangles of
// the 64 m tiles whose square lies within terrain_radius of terrain_centre (world XZ), at most max_terrain_triangles,
// made from the soup's terrain records (core::level::append_soup_terrain_tile) as layer-4 meshes in tile order after
// the Havok slots. The overlay re-prepares the window on its loader thread when the camera leaves the inner half.
struct CollisionPrepOptions {
	unsigned threads = 0; // 0 = all cores
	bool terrain = true;
	std::array<float, 2> terrain_centre{};
	float terrain_radius = 512.0f; // metres; about 200 tiles, 0.4 M triangles
	size_t max_terrain_triangles = size_t(1) << 21;
};
inline constexpr uint8_t kTerrainLayer = 4; // collision_layer of a kSoupTerrain triangle
inline constexpr float kMaxTerrainWindowRadius = 4096.0f; // terrain_radius is clamped to 0..this
// Cells one window tile may visit (append_soup_terrain_tile's budget): a retail 64 m tile visits about 1,100 at 2 m
// spacing; a tile past this (a hostile frame or spacing) is skipped and counted in tiles_failed.
inline constexpr size_t kTerrainWindowTileBudget = size_t(1) << 16;
// What a window holds (the status text and the check print it).
struct TerrainWindowStats {
	std::array<float, 2> centre{};
	float radius = 0.0f; // after the clamp
	size_t tiles_considered = 0; // tiles within the radius
	size_t tiles = 0; // with terrain triangles, kept (one layer-4 mesh each)
	size_t triangles = 0;
	size_t tiles_failed = 0; // budget or record errors
	bool capped = false; // max_terrain_triangles stopped the window short of the radius (nearest tiles kept)
};
// The tiles within terrain_radius of terrain_centre (the tile square's XZ distance), nearest first, ties in tile
// order; empty for a non-finite or far-out centre.
std::vector<std::array<int32_t, 2>> terrain_window_candidates(const CollisionPrepOptions &p_options);
// The Havok slots, then the terrain window (layer-4 meshes in tile order).
std::vector<PreparedCollisionMesh> prepare_collision_meshes(const core::level::CollisionSoup &p_soup, const CollisionPrepOptions &p_options,
		TerrainWindowStats *r_stats = nullptr);
// Only the terrain window (the camera moved): one layer-4 mesh per candidate tile with terrain triangles, all Built
// terrains of the tile in one mesh (terrain order, then append_soup_terrain_tile's emit order), in tile order.
std::vector<PreparedCollisionMesh> prepare_terrain_window(const core::level::CollisionSoup &p_soup, const CollisionPrepOptions &p_options,
		TerrainWindowStats *r_stats = nullptr);

} // namespace bf3::render
