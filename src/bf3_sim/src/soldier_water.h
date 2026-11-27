#pragma once

#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/water.h"

#include <optional>

namespace bf3::sim {

class CollisionWorld;

// SOURCE: notes R-B5 constants (0.2 throttle and -0.5 bank dot of the shallow rule, 0.05 m hysteresis) and R-B3 F
// (stand-up margins 0.3 m crouch, 0.1 m prone), the five constants below.
inline constexpr float kSwimThrottleThreshold = 0.2f;
inline constexpr float kSwimBankDot = -0.5f;
inline constexpr float kSwimHysteresis = 0.05f;
inline constexpr float kWaterCrouchMargin = 0.3f;
inline constexpr float kWaterProneMargin = 0.1f;

struct WaterDepth {
	std::optional<float> surface;
	float depth = 0.0f;
};

WaterDepth water_depth(const WaterSurface *p_water, const Vec3 &p_feet);

struct SwimTest {
	bool supported = false;
	bool swimming = false;
	float forward = 0.0f;
	float right = 0.0f;
	Vec3 ground_normal{ 0.0f, 1.0f, 0.0f };
	float yaw = 0.0f;
};

bool soldier_in_water(const SwimParams &p_swim, const WaterDepth &p_water, const SwimTest &p_test,
		const CollisionWorld &p_world, const Vec3 &p_feet);

bool water_blocks_low_pose(const MotorParams &p_params, const WaterDepth &p_water);

bool water_forces_stand(const MotorParams &p_params, const WaterDepth &p_water, Pose p_target);

float swim_vertical_speed(const SwimParams &p_swim, const WaterDepth &p_water, float p_feet_y, float p_dt);

}
