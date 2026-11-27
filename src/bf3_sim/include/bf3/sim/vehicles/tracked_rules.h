#pragma once

#include <array>
#include <span>

namespace bf3::sim::vehicles {

// SOURCE: notes R5-G1-5 (800, 0.25, 0.33, 0.01, 0.5; runtime setting defaults steer 1, forward 1, slip fade 7 and 1).
inline constexpr float kTrackSteerForce = 800.0f;
inline constexpr float kTankSteer = 1.0f;
inline constexpr float kTankForward = 1.0f;
inline constexpr float kTrackForwardQuarter = 0.25f;
inline constexpr float kTrackForwardShare = 0.33f;
inline constexpr float kTankSteerFixMultiplier = 7.0f;
inline constexpr float kTankSteerFixOffset = 1.0f;
inline constexpr float kTrackSteerWholeChange = 0.01f;
inline constexpr float kTrackSpinShare = 0.5f;

struct TrackSteerRule {
	float turn_speed_limit = 0;
	float forward_speed_limit = 0;
	std::span<const std::array<float, 2>> steer_inertia;
};

bool track_steer_wheel(int p_steering_type, int p_driving_type);

float track_forward_term(float p_engine_torque, float p_gear_ratio);

float track_turn_term(float p_yaw, float p_gear_ratio, float p_throttle, bool p_negative_x_side, bool p_inverted);

float track_slip_fade(float p_slip);

float track_steer_inertia(const TrackSteerRule &p_rule, float p_speed);

float track_steer_smooth(float p_previous, float p_target, float p_inertia);

float track_speed_attenuation(float p_limit, float p_angular_speed);

float boost_drive_factor(float p_acceleration_scale, float p_strength);
}
