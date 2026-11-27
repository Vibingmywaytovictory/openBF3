#include "soldier_water.h"

#include "bf3/sim/collision_world.h"
#include "bf3/sim/static_world.h"

#include <cmath>

namespace bf3::sim {

WaterDepth water_depth(const WaterSurface *p_water, const Vec3 &p_feet) {
	WaterDepth out;
	if (p_water != nullptr) {
		out.surface = p_water->height(p_feet[0], p_feet[2]);
	}
	out.depth = out.surface ? *out.surface - p_feet[1] : -INFINITY;
	return out;
}

// SOURCE: notes R-B5 B (the shallow-bank rule, WaterDepthLimit less 0.05 m once swimming, the clear ray down).
bool soldier_in_water(const SwimParams &p_swim, const WaterDepth &p_water, const SwimTest &p_test,
		const CollisionWorld &p_world, const Vec3 &p_feet) {
	if (!p_swim.present || !p_water.surface) {
		return false;
	}
	if (p_water.depth < p_swim.depth_limit && p_test.supported) {
		if (std::fabs(p_test.forward) > kSwimThrottleThreshold) {
			const float nx = p_test.ground_normal[0];
			const float nz = p_test.ground_normal[2];
			const float len = std::sqrt(nx * nx + nz * nz);
			if (len > 0.0f) {
				const float dot = (nx * std::sin(p_test.yaw) + nz * std::cos(p_test.yaw)) / len;
				if (dot * (p_test.forward > 0.0f ? 1.0f : -1.0f) < kSwimBankDot) {
					return false;
				}
			}
		} else if (p_test.forward == 0.0f && p_test.right == 0.0f) {
			return p_test.swimming;
		}
	}
	const float limit = p_swim.depth_limit - (p_test.swimming ? kSwimHysteresis : 0.0f);
	if (!(p_water.depth > limit)) {
		return false;
	}
	return !p_world.ray_any({ p_feet[0], *p_water.surface, p_feet[2] }, { 0.0f, -limit, 0.0f }, query_mask::kMovement);
}

// SOURCE: notes R-B3 F (pose-cycle requests acted on only while depth < Crouch Height - 0.3 m).
// INFERRED: the two input flags R-B3 F names are the crouch and prone requests, as the note infers. Settle: R-B3 F.
bool water_blocks_low_pose(const MotorParams &p_params, const WaterDepth &p_water) {
	return p_params.swim.present && p_water.depth >= p_params.poses[size_t(Pose::Crouch)].height - kWaterCrouchMargin;
}

// SOURCE: notes R-B3 F (target Crouch from Crouch Height - 0.3 m deep, Prone from Prone Height - 0.1 m: stand).
bool water_forces_stand(const MotorParams &p_params, const WaterDepth &p_water, Pose p_target) {
	if (!p_params.swim.present) {
		return false;
	}
	if (p_target == Pose::Crouch) {
		return p_water.depth >= p_params.poses[size_t(Pose::Crouch)].height - kWaterCrouchMargin;
	}
	if (p_target == Pose::Prone) {
		return p_water.depth >= p_params.poses[size_t(Pose::Prone)].height - kWaterProneMargin;
	}
	return false;
}

// SOURCE: notes R-B5 D 4 (vy = (surface - (feet + BodyUnderWater)) / dt, 0 without a water level).
float swim_vertical_speed(const SwimParams &p_swim, const WaterDepth &p_water, float p_feet_y, float p_dt) {
	if (!p_water.surface || !(p_dt > 0.0f)) {
		return 0.0f;
	}
	return (*p_water.surface - (p_feet_y + p_swim.body_under_water)) / p_dt;
}

}
