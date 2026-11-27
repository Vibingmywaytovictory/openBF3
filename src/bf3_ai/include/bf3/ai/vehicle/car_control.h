#pragma once

#include "bf3/ai/knowledge/ai_object.h"

#include <optional>
#include <span>

namespace bf3::ai {

inline constexpr float kCarArrivedMode = 666.0f;
inline constexpr float kCarNoSlowDown = 9999.0f;
inline constexpr double kCarAngleTestSeconds = 0.5;

struct CarControls {
	float turn = 0.0f;
	float throttle = 0.0f;

	bool operator==(const CarControls &) const = default;
};

struct CarCommand {
	float mode = 1.0f;
	float angle = 0.0f;
	float speed_sign = 1.0f;
	float speed_limit = 0.0f;
};

struct CarSpace {
	float front = 0.0f;
	float back = 0.0f;
	float left = 0.0f;
	float right = 0.0f;
};

struct CarSituation {
	float radius = 0.0f;
	float max_speed = 0.0f;
	Vec2 forward{ 0.0f, 1.0f };
	Vec2 normal{ 1.0f, 0.0f };
	Vec2 desired{ 0.0f, 1.0f };
	float goal_distance = 0.0f;
	float forward_speed = 0.0f;
	CarSpace space;
	float slow_down = kCarNoSlowDown;
	bool slow_down_stop = false;
};

float car_signed_angle(const Vec2 &p_from, const Vec2 &p_normal, const Vec2 &p_to);
void car_reverse_angle(float &r_angle);
float car_trace_length(float p_radius);

CarControls car_control_towards_direction(
		const Vec2 &p_velocity, const Vec3 &p_rotational_speed, float p_speed, const CarCommand &p_command);
float car_reset_throttle(float p_forward_speed);
float car_turn_factor(float p_speed_magnitude, std::optional<float> p_largest_angle);
float car_approach_factor(float p_remaining, float p_smallest_radius);

CarCommand car_action_status(int &r_status, const CarSituation &p_situation);

class CarAngleDiff {
public:
	float largest(
			double p_now, const Vec3 &p_position, const Vec2 &p_facing, float p_speed, std::span<const Vec3> p_points);

private:
	double last_test_ = -1.0e9;
	float largest_ = 0.0f;
};

class CarProgress {
public:
	void clear(double p_now);
	bool check(double p_now, const Vec3 &p_position, float p_throttle, float p_turn);

private:
	Vec3 anchor_{};
	double no_throttle_ = 0.0;
	double no_progress_ = 0.0;
};

}
