#pragma once

#include "bf3/core/math.h"
#include "bf3/core/result.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace bf3::core::level {

class LoadedLevel;
class World;

inline constexpr size_t kMaxWaterMeshes = 64;
inline constexpr size_t kMaxWaterTriangles = size_t(1) << 20;

struct WaterMesh {
	std::string asset;
	std::vector<std::array<Vec3, 3>> triangles;
};

Result<std::vector<WaterMesh>> read_water_meshes(const LoadedLevel &p_level, const World &p_world);

}
