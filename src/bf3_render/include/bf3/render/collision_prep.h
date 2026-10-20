#pragma once

#include "bf3/core/level/collision_soup.h"

#include <array>
#include <cstdint>
#include <vector>

namespace bf3::render {

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

std::array<float, 3> collision_surface_colour(uint8_t p_surface);

struct PreparedCollisionMesh {
	int32_t tile_x = 0;
	int32_t tile_z = 0;
	uint8_t layer = 0;
	size_t triangles = 0;
	std::vector<float> positions;
	std::vector<float> colors;
	std::vector<float> uvs;
	std::array<float, 3> min{};
	std::array<float, 3> max{};
};

std::vector<PreparedCollisionMesh> prepare_collision_meshes(const core::level::CollisionSoup &p_soup, unsigned p_threads = 0);

struct CollisionPrepOptions {
	unsigned threads = 0;
	bool terrain = true;
	std::array<float, 2> terrain_centre{};
	float terrain_radius = 512.0f;
	size_t max_terrain_triangles = size_t(1) << 21;
};
inline constexpr uint8_t kTerrainLayer = 4;
inline constexpr float kMaxTerrainWindowRadius = 4096.0f;

inline constexpr size_t kTerrainWindowTileBudget = size_t(1) << 16;

struct TerrainWindowStats {
	std::array<float, 2> centre{};
	float radius = 0.0f;
	size_t tiles_considered = 0;
	size_t tiles = 0;
	size_t triangles = 0;
	size_t tiles_failed = 0;
	bool capped = false;
};

std::vector<std::array<int32_t, 2>> terrain_window_candidates(const CollisionPrepOptions &p_options);

std::vector<PreparedCollisionMesh> prepare_collision_meshes(const core::level::CollisionSoup &p_soup, const CollisionPrepOptions &p_options,
		TerrainWindowStats *r_stats = nullptr);

std::vector<PreparedCollisionMesh> prepare_terrain_window(const core::level::CollisionSoup &p_soup, const CollisionPrepOptions &p_options,
		TerrainWindowStats *r_stats = nullptr);

}
