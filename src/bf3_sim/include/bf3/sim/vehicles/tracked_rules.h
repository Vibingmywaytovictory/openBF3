#pragma once

#include <array>
#include <span>

namespace bf3::sim::vehicles {

inline constexpr float kTrackSteerForce = 800.0f;
inline constexpr float kTankSteer = 1.0f;
inline constexpr float kTankSteerFixMultiplier = 7.0f;
inline constexpr float kTankSteerFixOffset = 1.0f;
inline constexpr float kTrackSteerWholeChange = 0.01f;

struct TrackSteerRule {
	float turn_speed_limit = 0;
	float forward_speed_limit = 0;
	std::span<const std::array<float, 2>> steer_inertia;
};

bool track_steer_wheel(int p_steering_type, int p_driving_type);

float track_steer_target(float p_steer, float p_side_sign, float p_wheel_speed, float p_engine_factor);

float track_steer_inertia(const TrackSteerRule &p_rule, float p_speed);

float track_steer_smooth(float p_previous, float p_target, float p_inertia);

float track_speed_attenuation(float p_limit, float p_vehicle_speed);

}
