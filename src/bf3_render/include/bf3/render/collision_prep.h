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
// (0 = all cores).
std::vector<PreparedCollisionMesh> prepare_collision_meshes(const core::level::CollisionSoup &p_soup, unsigned p_threads = 0);

} // namespace bf3::render
