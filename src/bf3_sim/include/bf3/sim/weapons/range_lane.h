#pragma once

#include "bf3/sim/collision_world.h"
#include "bf3/sim/spawn.h"
#include "bf3/sim/weapons/ballistics.h"

#include <cstdint>
#include <optional>
#include <span>

namespace bf3::sim::weapons {

inline constexpr float kRangeLaneFloorTolerance = 0.5f;

struct RangeLane {
	Vec3 feet{};
	Vec3 eye{};
	uint16_t yaw = 0;
	float length = 0;
	uint32_t spawn_index = 0;
};

DVec3 range_heading(uint16_t p_yaw);

Vec3 range_floor_under(const CollisionWorld &p_world, const Vec3 &p_point, float p_up, float p_down, bool &r_found);

std::optional<RangeLane> find_range_lane(const CollisionWorld &p_world, std::span<const SpawnPoint> p_spawns,
		float p_eye_height, float p_min_length, uint32_t p_headings = 64);

}
