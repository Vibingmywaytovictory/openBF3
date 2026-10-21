#include "bf3/sim/weapons/range_lane.h"

#include "weapons_internal.h"

#include <algorithm>
#include <cmath>

namespace bf3::sim::weapons {

namespace {

using namespace detail;

constexpr float kLaneMaxLength = 500.0f;
constexpr float kLaneLowRay = 0.3f;

float clear_length(const CollisionWorld &p_world, const Vec3 &p_from, const DVec3 &p_dir, float p_max) {
	StaticHit h;
	const Vec3 delta = to_f(scale(p_dir, double(p_max)));
	return p_world.ray(p_from, delta, query_mask::kProjectile, &h) ? h.fraction * p_max : p_max;
}

float lane_length(const CollisionWorld &p_world, const Vec3 &p_feet, uint16_t p_yaw, float p_eye_height) {
	const DVec3 dir = range_heading(p_yaw);
	const float eye = clear_length(p_world, { p_feet[0], p_feet[1] + p_eye_height, p_feet[2] }, dir, kLaneMaxLength);
	const float low = clear_length(p_world, { p_feet[0], p_feet[1] + kLaneLowRay, p_feet[2] }, dir, kLaneMaxLength);
	const float len = std::min(eye, low);
	if (len <= 1.0f) {
		return 0.0f;
	}
	const Vec3 far = to_f(add(to_d(p_feet), scale(dir, double(len - 1.0f))));
	bool found = false;
	const Vec3 floor = range_floor_under(p_world, far, 1.0f, 2.0f, found);
	if (!found || std::fabs(floor[1] - p_feet[1]) > kRangeLaneFloorTolerance) {
		return 0.0f;
	}
	return len;
}

}

DVec3 range_heading(uint16_t p_yaw) {
	double s, c;
	poly_sincos(double(yaw_radians(p_yaw)), s, c);
	return { s, 0, c };
}

Vec3 range_floor_under(const CollisionWorld &p_world, const Vec3 &p_point, float p_up, float p_down, bool &r_found) {
	StaticHit h;
	r_found = p_world.ground_below(
			{ p_point[0], p_point[1] + p_up, p_point[2] }, p_up + p_down, query_mask::kMovement, &h);
	return r_found ? h.position : p_point;
}

std::optional<RangeLane> find_range_lane(const CollisionWorld &p_world, std::span<const SpawnPoint> p_spawns,
		float p_eye_height, float p_min_length, uint32_t p_headings) {
	std::optional<RangeLane> best;
	const uint32_t headings = std::clamp<uint32_t>(p_headings, 1, 4096);
	for (size_t i = 0; i < p_spawns.size(); ++i) {
		bool found = false;
		const Vec3 feet = range_floor_under(p_world, p_spawns[i].position, 1.0f, 2.0f, found);
		if (!found) {
			continue;
		}
		for (uint32_t k = 0; k < headings; ++k) {
			const uint16_t yaw = uint16_t((uint64_t(k) * 65536u) / headings);
			const float len = lane_length(p_world, feet, yaw, p_eye_height);
			if (len >= p_min_length && (!best || len > best->length)) {
				RangeLane l;
				l.feet = feet;
				l.eye = { feet[0], feet[1] + p_eye_height, feet[2] };
				l.yaw = yaw;
				l.length = len;
				l.spawn_index = p_spawns[i].index;
				best = l;
			}
		}
	}
	return best;
}

}
