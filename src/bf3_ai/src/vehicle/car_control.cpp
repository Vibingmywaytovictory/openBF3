// SOURCE: OpenBF2 Code/BF2/AI/AIDLL/AI/AIPlanInterpreter/CarControl.cpp, EntryCarMoveTo.cpp and AIBot/BotNavigation.cpp
// (checkProgress) @c47317b
#include "bf3/ai/vehicle/car_control.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace bf3::ai {

namespace {

constexpr float kSpaceTweak = 1.3f;
constexpr float kMinSpeed = 2.0f;
constexpr float kStartAngle = 0.2f;
constexpr float kTurnSpeedScale = 0.8f;
constexpr float kMaxResetThrottle = 0.7f;
constexpr float kAngleTimeStep = 2.0f;
constexpr float kProgressRadius = 4.0f;
constexpr double kProgressSeconds = 20.0;
constexpr double kStillSeconds = 0.5;

float sign_of(float p_value) {
	return p_value > 0.0f ? 1.0f : (0.0f > p_value ? -1.0f : 0.0f);
}

float dot2(const Vec2 &p_a, const Vec2 &p_b) {
	return p_a.x * p_b.x + p_a.y * p_b.y;
}

Vec2 normalised(Vec2 p_v) {
	const float length_sq = dot2(p_v, p_v);
	if (std::fabs(length_sq - 1.0f) < 1.1920929e-07f) {
		return p_v;
	}
	if (std::fabs(length_sq) < 1.4210855e-14f) {
		return {};
	}
	const float inverse = 1.0f / std::sqrt(length_sq);
	return { p_v.x * inverse, p_v.y * inverse };
}

float unsigned_angle(const Vec2 &p_from, const Vec2 &p_to) {
	return std::atan2(std::fabs(p_from.x * p_to.y - p_from.y * p_to.x), dot2(p_from, p_to));
}

}

float car_signed_angle(const Vec2 &p_from, const Vec2 &p_normal, const Vec2 &p_to) {
	const float angle = unsigned_angle(p_from, p_to);
	return dot2(p_normal, p_to) < 0.0f ? -angle : angle;
}

void car_reverse_angle(float &r_angle) {
	const float magnitude = std::fabs(r_angle);
	if (!(magnitude > std::numbers::pi_v<float> * 0.5f)) {
		return;
	}
	r_angle = sign_of(r_angle) * (std::numbers::pi_v<float> - magnitude);
}

float car_trace_length(float p_radius) {
	return p_radius + 10.0f;
}

CarControls car_control_towards_direction(
		const Vec2 &p_velocity, const Vec3 &p_rotational_speed, float p_speed, const CarCommand &p_command) {
	CarControls out;
	const float velocity = std::sqrt(dot2(p_velocity, p_velocity));
	float angle = p_command.angle;
	if (p_command.mode != 0.0f && p_command.speed_sign != p_command.mode && velocity > 1.0f) {
		angle = -angle;
	}
	if (p_command.mode == 0.0f) {
		out.turn = std::min(std::fabs(angle * 3.0f), 1.0f) * -sign_of(angle);
		return out;
	}
	out.turn = std::clamp(angle * 3.0f, -1.0f, 1.0f);
	float speed = p_speed * p_command.mode;
	// INFERRED: the angular velocity is in the hull frame (z roll, x pitch). Settle: BF2's frame for the vehicle's rotational
	// speed.
	const float tilt = std::max(std::fabs(p_rotational_speed[2]), std::fabs(p_rotational_speed[0]));
	if (tilt > 0.15f) {
		speed = (1.0f - std::min((tilt - 0.15f) / 0.7f, 1.0f) * 0.8f) * speed;
	}
	if (std::fabs(speed) > p_command.speed_limit) {
		speed = sign_of(speed) * p_command.speed_limit;
	}
	const float response = std::min((speed - velocity * p_command.speed_sign) / 3.0f, 1.0f) + speed / 15.0f;
	out.throttle = std::clamp(response, -1.0f, 1.0f);
	if (out.throttle != 0.0f && !(1.5f <= velocity)) {
		out.throttle = sign_of(out.throttle);
	}
	return out;
}

float car_reset_throttle(float p_forward_speed) {
	const float brake = -std::clamp(p_forward_speed, -kMaxResetThrottle, kMaxResetThrottle);
	if (!(std::fabs(brake) > 0.05f)) {
		return 0.0f;
	}
	return sign_of(brake) * std::max(std::fabs(brake), 0.4f);
}

float car_turn_factor(float p_speed_magnitude, std::optional<float> p_largest_angle) {
	if (!(std::fabs(p_speed_magnitude) > 1.0f) || !p_largest_angle) {
		return 1.0f;
	}
	const float excess = std::max(std::fabs(*p_largest_angle) - kStartAngle, 0.0f);
	return 1.0f - std::min(excess * 1.8f, 1.0f) * kTurnSpeedScale;
}

float car_approach_factor(float p_remaining, float p_smallest_radius) {
	const float gap = std::max(p_remaining - p_smallest_radius - 0.5f, 0.0f);
	if (!(gap * gap < 225.0f)) {
		return 1.0f;
	}
	return std::max(gap * gap / 225.0f, 0.2f);
}

// STAND-IN: no obstacle list (CommonControls' collision, avoid-side and slow-down checks are not ported), so the
// collided-object statuses fall back to 0. Settle: port CommonControls.
CarCommand car_action_status(int &r_status, const CarSituation &p_situation) {
	CarCommand out;
	const CarSituation &s = p_situation;
	const float radius = s.radius;
	const float near_distance = radius * 10.0f;
	const float dot = dot2(s.desired, s.forward);
	out.angle = car_signed_angle(s.forward, s.normal, s.desired);
	const bool angle_negative = 0.0f > out.angle;
	out.speed_sign = s.forward_speed >= 0.0f ? 1.0f : -1.0f;
	out.mode = 1.0f;
	int status = r_status;
	const bool reversing = status > 7;
	const float trace_length = car_trace_length(radius);
	const float space_radius = radius * kSpaceTweak;
	const CarSpace &space = s.space;
	const bool wide_space = space.left + space.right > radius * 2.7f && space.front + space.back > radius * 2.7f;
	const bool very_wide_space = space.left + space.right > radius * 3.0f && space.front + space.back > radius * 3.0f;
	const float back_ratio = (space.back - space_radius) / (trace_length - space_radius);
	const float front_ratio = (space.front - space_radius) / (trace_length - space_radius);
	bool stuck = false;
	if (space.front == 0.0f && space.back == 0.0f) {
		stuck = true;
		status = !(dot > 0.0f) ? 8 : 0;
	}
	if (!reversing) {
		out.speed_limit = 0.95f > front_ratio ? 5.0f - (1.0f - front_ratio) * (5.0f - kMinSpeed)
											  : s.max_speed - (1.0f - front_ratio) * (s.max_speed - kMinSpeed);
	} else {
		out.speed_limit = 5.0f - (1.0f - back_ratio) * (5.0f - kMinSpeed);
	}
	switch (status) {
		case 0:
			if (stuck) {
				out.mode = 1.0f;
				out.speed_limit = s.max_speed * 0.5f;
				break;
			}
			if (!(0.0f > dot) && !(space_radius > space.front)) {
				out.mode = 1.0f;
				break;
			}
			if (near_distance >= s.goal_distance && -0.6f > dot && space.back > space_radius) {
				r_status = 8;
				break;
			}
			if (0.0f > dot && !wide_space) {
				r_status = 12;
				break;
			}
			r_status = angle_negative ? 9 : 11;
			break;
		case 2:
			if (angle_negative) {
				r_status = dot > 0.0f ? 3 : 9;
				break;
			}
			if (dot > 0.9f) {
				r_status = 0;
				break;
			}
			if (space_radius > space.front) {
				r_status = 11;
				break;
			}
			out.mode = 1.0f;
			out.angle = std::fabs(out.angle);
			break;
		case 3:
			if (!angle_negative) {
				r_status = dot > 0.0f ? 2 : 11;
				break;
			}
			if (dot > 0.9f) {
				r_status = 0;
				break;
			}
			if (space_radius > space.front) {
				r_status = 9;
				break;
			}
			out.mode = 1.0f;
			out.angle = -std::fabs(out.angle);
			break;
		case 4:
		case 5:
			r_status = 6;
			break;
		case 6:
			r_status = 0.0f > out.angle ? 3 : 2;
			break;
		case 8:
			if (stuck) {
				out.mode = -1.0f;
				out.angle = -out.angle;
				car_reverse_angle(out.angle);
				break;
			}
			if (dot > -0.5f || s.goal_distance > near_distance + 5.0f || space_radius > space.back) {
				r_status = 0;
				out.mode = 1.0f;
				break;
			}
			out.mode = -1.0f;
			car_reverse_angle(out.angle);
			break;
		case 9:
			if (!angle_negative) {
				r_status = dot > 0.0f ? 2 : 11;
				break;
			}
			if (dot > 0.9f && space.front > space_radius) {
				r_status = 0;
			} else if (space_radius > space.back) {
				r_status = 3;
			} else {
				out.mode = -1.0f;
				out.angle = std::fabs(out.angle);
			}
			break;
		case 11:
			if (angle_negative) {
				r_status = dot > 0.0f ? 3 : 9;
				break;
			}
			if (dot > 0.9f && space.front > space_radius) {
				r_status = 0;
			} else if (space_radius > space.back) {
				r_status = 2;
			} else {
				out.mode = -1.0f;
				out.angle = -std::fabs(out.angle);
			}
			break;
		case 12:
			if (dot > 0.0f) {
				r_status = 0;
			} else if (near_distance >= s.goal_distance && -0.6f > dot && space.back > space_radius) {
				r_status = 8;
			} else if (very_wide_space) {
				r_status = angle_negative ? 9 : 11;
			} else {
				out.mode = -1.0f;
				car_reverse_angle(out.angle);
			}
			break;
		case 13:
			r_status = 6;
			out.mode = -1.0f;
			out.angle = 2.0f;
			break;
		case 14:
			r_status = 6;
			out.mode = -1.0f;
			out.angle = -2.0f;
			break;
		default:
			break;
	}
	if (out.speed_limit > s.slow_down) {
		out.speed_limit = s.slow_down;
	}
	if (s.slow_down_stop) {
		out.angle = -2.0f;
	}
	return out;
}

float CarAngleDiff::largest(
		double p_now, const Vec3 &p_position, const Vec2 &p_facing, float p_speed, std::span<const Vec3> p_points) {
	if (!(p_now - last_test_ >= kCarAngleTestSeconds) || p_points.empty()) {
		return largest_;
	}
	last_test_ = p_now;
	const auto length = [](const Vec3 &p_a, const Vec3 &p_b) {
		const float dx = p_a[0] - p_b[0], dy = p_a[1] - p_b[1], dz = p_a[2] - p_b[2];
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	};
	float covered = length(p_position, p_points.front());
	const float range = std::max(p_speed * kAngleTimeStep, 3.0f);
	float out = 0.0f;
	for (size_t i = 0; covered < range && i < p_points.size(); ++i) {
		const Vec3 &from = i == 0 ? p_position : p_points[i - 1];
		const Vec3 &to = p_points[i];
		covered += length(from, to);
		const Vec2 leg = normalised({ to[0] - from[0], to[2] - from[2] });
		out = std::max(out, unsigned_angle(p_facing, leg));
	}
	largest_ = out;
	return out;
}

void CarProgress::clear(double p_now) {
	no_throttle_ = p_now;
	no_progress_ = p_now;
	anchor_ = {};
}

bool CarProgress::check(double p_now, const Vec3 &p_position, float p_throttle, float p_turn) {
	const float move = std::max(std::max(p_throttle, 0.0f), p_turn);
	if (std::fabs(move) <= 0.1f) {
		if (kStillSeconds < p_now - no_throttle_) {
			no_progress_ = p_now;
			return true;
		}
	} else {
		no_throttle_ = p_now;
	}
	const float dx = anchor_[0] - p_position[0], dy = anchor_[1] - p_position[1], dz = anchor_[2] - p_position[2];
	if (dx * dx + dy * dy + dz * dz <= kProgressRadius * kProgressRadius) {
		return p_now - no_progress_ < kProgressSeconds;
	}
	anchor_ = p_position;
	no_progress_ = p_now;
	return true;
}

}
