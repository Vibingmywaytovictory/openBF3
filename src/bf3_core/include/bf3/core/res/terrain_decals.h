#pragma once

#include "bf3/core/res/res_types.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bf3::core::res {

inline constexpr size_t kMaxTerrainDecalRecords = 1u << 16;
inline constexpr size_t kMaxTerrainDecalVertices = 1u << 22;
inline constexpr size_t kMaxTerrainDecalIndices = 1u << 24;

inline constexpr size_t kTerrainDecalPlanarVertexBytes = 24;
inline constexpr size_t kTerrainDecalSurfaceVertexBytes = 28;
inline constexpr size_t kTerrainDecalWaterVertexBytes = 36;

struct TerrainDecalRecord {
	std::string shader;
	uint8_t flag = 0;
	uint32_t first_index = 0;
	uint32_t triangle_count = 0;
	Vec3 bounds_min{};
	Vec3 bounds_max{};
	Vec3 patch_min{};
	Vec3 patch_max{};
	float inverse_order = 0.0f;
	uint32_t first_vertex = 0;
	uint32_t end_vertex = 0;

	int32_t draw_order() const;
};

struct TerrainDecalVertex {
	Vec3 position{};
	Vec2 uv{};
	Vec4 mask{};
	Vec3 normal{ 0.0f, 1.0f, 0.0f };
	Vec3 tangent{ 1.0f, 0.0f, 0.0f };
};

struct TerrainDecalSet {
	std::vector<TerrainDecalRecord> records;
	std::vector<TerrainDecalVertex> vertices;
	std::vector<uint32_t> indices;
};

struct TerrainDecals {
	float head_a = 0.0f;
	float head_b = 0.0f;
	uint32_t head_c = 0;
	uint8_t head_d = 0;
	TerrainDecalSet planar;
	TerrainDecalSet surface;
	TerrainDecalSet water;

	static ResError parse(Bytes p_res, TerrainDecals &r_out);
};

}
