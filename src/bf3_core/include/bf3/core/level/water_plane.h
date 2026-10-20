#pragma once

#include <array>
#include <vector>

namespace bf3::core::level {

struct WaterPlane {
	float y = 0.0f;
	std::vector<std::array<float, 2>> polygon;
	bool operator==(const WaterPlane &) const = default;
};

bool water_plane_covers(const WaterPlane &p_plane, float p_x, float p_z);

inline constexpr float kCoverWaterDepth = 1.8f;

}
