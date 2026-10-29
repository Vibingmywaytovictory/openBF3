#include "bf3/ai/path/map_properties.h"

#include <algorithm>
#include <cmath>

namespace bf3::ai {

bool MapProperties::is_valid_ground_mesh(const Vec3 &p_normal, MaterialId) const {
	return max_slope_cos_ <= p_normal.y;
}

void MapProperties::set_max_slope(float p_radians) {
	max_slope_cos_ = std::cos(p_radians);
}

float MapProperties::max_slope() const {
	return std::acos(std::clamp(max_slope_cos_, -1.0f, 1.0f));
}

}
