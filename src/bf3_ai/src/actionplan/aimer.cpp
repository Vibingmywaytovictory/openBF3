#include "bf3/ai/actionplan/aimer.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace bf3::ai {

namespace {

constexpr float kEpsilon = 1.1920929e-07f;

float length3(float p_x, float p_y, float p_z) {
	return std::sqrt(p_x * p_x + p_y * p_y + p_z * p_z);
}

void push_root(double p_root, double p_a, double p_b, double p_c, double p_d, std::vector<float> &r_roots) {
	double t = p_root;
	for (int i = 0; i < 2; ++i) {
		const double f = ((p_a * t + p_b) * t + p_c) * t + p_d;
		const double df = (3.0 * p_a * t + 2.0 * p_b) * t + p_c;
		if (df == 0.0) {
			break;
		}
		t -= f / df;
	}
	if (std::isfinite(t)) {
		r_roots.push_back(float(t));
	}
}

}

void cubic_roots(float p_c0, float p_c1, float p_c2, float p_c3, std::vector<float> &r_roots) {
	r_roots.clear();
	const double a = p_c3, b = p_c2, c = p_c1, d = p_c0;
	const double scale = std::max({ std::fabs(a), std::fabs(b), std::fabs(c), std::fabs(d) });
	if (!(scale > 0.0) || !std::isfinite(scale)) {
		return;
	}
	const double tiny = scale * 1e-9;
	if (std::fabs(a) <= tiny) {
		if (std::fabs(b) <= tiny) {
			if (std::fabs(c) > tiny) {
				push_root(-d / c, 0.0, 0.0, c, d, r_roots);
			}
			return;
		}
		const double disc = c * c - 4.0 * b * d;
		if (disc < 0.0) {
			return;
		}
		const double s = std::sqrt(disc);
		push_root((-c + s) / (2.0 * b), 0.0, b, c, d, r_roots);
		if (s > 0.0) {
			push_root((-c - s) / (2.0 * b), 0.0, b, c, d, r_roots);
		}
		return;
	}
	const double nb = b / a, nc = c / a, nd = d / a;
	const double shift = -nb / 3.0;
	const double p = nc - nb * nb / 3.0;
	const double q = 2.0 * nb * nb * nb / 27.0 - nb * nc / 3.0 + nd;
	const double disc = q * q / 4.0 + p * p * p / 27.0;
	if (disc > 0.0) {
		const double s = std::sqrt(disc);
		push_root(std::cbrt(-q / 2.0 + s) + std::cbrt(-q / 2.0 - s) + shift, a, b, c, d, r_roots);
		return;
	}
	if (p >= 0.0) {
		push_root(shift, a, b, c, d, r_roots);
		return;
	}
	const double m = 2.0 * std::sqrt(-p / 3.0);
	const double phi = std::acos(std::clamp(3.0 * q / (p * m), -1.0, 1.0));
	for (int k = 0; k < 3; ++k) {
		push_root(m * std::cos((phi - 2.0 * std::numbers::pi * k) / 3.0) + shift, a, b, c, d, r_roots);
	}
}

bool Aimer::set_target_data(
		const Vec3 &p_relative_pos, const Vec3 &p_target_velocity, const Vec3 &p_shooter_velocity, double p_time) {
	relative_pos_ = p_relative_pos;
	relative_dir_ = p_relative_pos;
	const float length_sq =
			relative_dir_.x * relative_dir_.x + relative_dir_.y * relative_dir_.y + relative_dir_.z * relative_dir_.z;
	if (std::fabs(length_sq - 1.0f) >= kEpsilon) {
		if (std::fabs(length_sq) < kEpsilon) {
			relative_dir_ = Vec3{};
		} else {
			const float inv = 1.0f / std::sqrt(length_sq);
			relative_dir_ = Vec3{ relative_dir_.x * inv, relative_dir_.y * inv, relative_dir_.z * inv };
		}
	}
	target_velocity_ = p_target_velocity;
	shooter_velocity_ = p_shooter_velocity;
	time_ = p_time;
	return true;
}

float Aimer::impact_time(float p_distance, float p_speed) const {
	const float dt = time_step;
	float time = 0.0f;
	float v = p_speed;
	while (p_distance > 0.0f) {
		const float drag_term = p_speed > 200.0f ? v * v : v;
		const float next = std::max(v - drag_term * drag * dt, 0.0f);
		const float step = (v + next) * 0.5f * dt;
		if (step <= 0.0f || 1000.0f * step < p_distance) {
			return 1000.0f;
		}
		time += p_distance < step ? (p_distance / step) * dt : dt;
		p_distance -= step;
		v = next;
	}
	return time;
}

void Aimer::predicted_impact_position(Vec3 p_velocity, float p_time, Vec3 &r_out) const {
	const float initial = length3(p_velocity.x, p_velocity.y, p_velocity.z);
	r_out = Vec3{};
	if (initial == 0.0f) {
		return;
	}
	float dt = time_step;
	for (; p_time > 0.0f; p_time -= dt) {
		if (p_time < dt) {
			dt = p_time;
		}
		float dx = 0.0f, dy = 0.0f, dz = 0.0f;
		if (drag != 0.0f) {
			if (initial <= 200.0f) {
				dx = p_velocity.x * drag;
				dy = p_velocity.y * drag;
				dz = p_velocity.z * drag;
			} else {
				const float v = length3(p_velocity.x, p_velocity.y, p_velocity.z);
				dx = p_velocity.x * v * drag;
				dy = p_velocity.y * v * drag;
				dz = v * p_velocity.z * drag;
			}
			dx *= dt;
			dy *= dt;
			dz *= dt;
		}
		const float nx = (p_velocity.x - dx) + dt * 0.0f;
		const float ny = (p_velocity.y - dy) + gravity * dt;
		const float nz = (p_velocity.z - dz) + dt * 0.0f;
		r_out.x = (p_velocity.x + nx) * 0.5f * dt + r_out.x;
		r_out.y = (p_velocity.y + ny) * 0.5f * dt + r_out.y;
		r_out.z = (p_velocity.z + nz) * 0.5f * dt + r_out.z;
		p_velocity = Vec3{ nx, ny, nz };
	}
}

bool Aimer::static_firing_direction(Vec3 &r_direction, Vec3 &r_target) const {
	r_target = relative_pos_;
	const float horizontal = std::sqrt(relative_pos_.x * relative_pos_.x + relative_pos_.z * relative_pos_.z);
	const float g = -gravity;
	const float speed_sq = speed * speed;
	const float b = -horizontal;
	const float a = (horizontal * horizontal * g) / (speed_sq + speed_sq);
	const float disc = b * b - a * 4.0f * (a + relative_pos_.y);
	if (disc < 0.0f) {
		return false;
	}
	const float root = std::sqrt(disc);
	const float first = std::atan((root - b) / (a + a));
	const float angle = std::max(std::atan((-b - root) / (a + a)), first);
	const float c = std::cos(angle);
	const float s = std::sin(angle);
	r_direction = Vec3{ relative_pos_.x * (c / horizontal), s, (c / horizontal) * relative_pos_.z };
	return true;
}

bool Aimer::turn_point_for_grenade(const Vec3 &p_direction, Vec3 &r_point, float &r_time) const {
	const float up = p_direction.y;
	if (up > 0.0f) {
		const float t = (up * speed) / -gravity;
		r_time = t;
		r_point = Vec3{ p_direction.x * t * speed, up * speed * 0.5f * t, p_direction.z * t * speed };
	}
	return up > 0.0f;
}

bool Aimer::firing_direction(
		double p_time, float p_accuracy, bool p_high_arc, Vec3 &r_direction, Vec3 &r_target, double *r_impact_time) {
	const bool no_speed = speed == 0.0f;
	const float rel_y = no_speed ? relative_pos_.y + 20.0f : relative_pos_.y;
	const float distance = length3(relative_pos_.x, rel_y, relative_pos_.z);
	const float closing = (speed -
								  (target_velocity_.y * relative_dir_.y + target_velocity_.x * relative_dir_.x +
										  target_velocity_.z * relative_dir_.z)) +
			relative_dir_.x * shooter_velocity_.x + relative_dir_.y * shooter_velocity_.y +
			relative_dir_.z * shooter_velocity_.z;
	if (closing < 1.0f) {
		return false;
	}
	const float first_estimate = distance / closing;
	if (first_estimate < 0.0f) {
		return false;
	}
	double impact_at = p_time + first_estimate;
	predicted_target_position(impact_at, r_target);
	float horizontal = std::sqrt(r_target.z * r_target.z + r_target.x * r_target.x);
	if (horizontal == 0.0f) {
		return false;
	}
	float height = no_speed ? r_target.y + 20.0f : r_target.y;

	float angle = 0.0f;
	if (!p_high_arc || angle_ >= 0.7853982f) {
		angle = !no_speed && angle_ != kNoAngle ? angle_ : std::atan2(height, horizontal);
	} else {
		angle = 1.1780972f;
	}
	{
		const float sign = angle_step_ > 0.0f ? 1.0f : (angle_step_ >= 0.0f ? 0.0f : -1.0f);
		angle_step_ = std::max(std::fabs(angle_step_), 0.0001f) * sign;
	}
	if (p_high_arc && !no_speed) {
		angle = (angle + 1.5079645f) * 0.5f;
		if (angle_step_ > 0.0f) {
			angle_step_ = -angle_step_;
		}
	}

	bool converged = false;
	int first_direction = 0;
	const unsigned int iterations = static_cast<unsigned int>(p_accuracy * 8.0f + 2.0f);
	unsigned int iteration = 0;
	unsigned int converged_count = 0;
	while (converged_count < iterations) {
		float hx = r_target.x, hy = 0.0f, hz = r_target.z;
		const float h_sq = hx * hx + 0.0f + hz * hz;
		if (std::fabs(h_sq - 1.0f) >= kEpsilon) {
			if (std::fabs(h_sq) < kEpsilon) {
				hx = 0.0f;
				hz = 0.0f;
			} else {
				const float inv = 1.0f / std::sqrt(h_sq);
				hy = inv * 0.0f;
				hx = r_target.x * inv;
				hz = inv * r_target.z;
			}
		}
		const auto horizontal_speed_at = [&](float p_angle) {
			return shooter_velocity_.z * hz + shooter_velocity_.x * hx + shooter_velocity_.y * hy +
					std::cos(p_angle) * speed;
		};
		float horizontal_speed = horizontal_speed_at(angle);
		float vertical_speed = std::sin(angle) * speed + shooter_velocity_.y;
		if (horizontal_speed <= 0.1f) {
			return false;
		}
		float t = drag != 0.0f ? impact_time(horizontal, horizontal_speed) : horizontal / horizontal_speed;
		if (t > 60.0f) {
			angle = std::atan2(height, horizontal);
			angle_ = angle;
			horizontal_speed = horizontal_speed_at(angle);
			vertical_speed = std::sin(angle) * speed + shooter_velocity_.y;
			predicted_target_position(p_time + first_estimate, r_target);
			const float h = std::sqrt(r_target.z * r_target.z + r_target.x * r_target.x);
			t = drag != 0.0f ? impact_time(h, horizontal_speed) : h / horizontal_speed;
		}
		if (t > 100.0f) {
			return false;
		}
		Vec3 impact{};
		predicted_impact_position(Vec3{ horizontal_speed, vertical_speed, 0.0f }, t, impact);
		vertical_extreme_ = t * 0.5f;
		impact_at = p_time + t;
		predicted_target_position(impact_at, r_target);
		horizontal = std::sqrt(r_target.z * r_target.z + r_target.x * r_target.x);
		height = no_speed ? r_target.y + 20.0f : r_target.y;
		if (grenade && !grenade_exact) {
			height -= 1.3f;
		}
		if (!std::isfinite(horizontal) || !std::isfinite(height)) {
			return false;
		}
		if (no_speed) {
			converged = true;
			break;
		}
		if (iteration == iterations - 1) {
			break;
		}
		if (height <= impact.y) {
			if (angle <= -kMaxElevation) {
				return false;
			}
			if (first_direction == 0) {
				first_direction = -1;
			} else if (first_direction > 0) {
				converged = true;
			}
			if (converged) {
				angle_step_ *= 0.5f;
			}
			const float next = angle - angle_step_;
			angle = -kMaxElevation <= next ? next : -kMaxElevation;
		} else {
			if (kMaxElevation <= angle) {
				return false;
			}
			if (first_direction == 0) {
				first_direction = 1;
			} else if (first_direction < 0) {
				converged = true;
			}
			if (converged) {
				angle_step_ *= 0.5f;
			}
			angle = std::min(angle + angle_step_, kMaxElevation);
		}
		++iteration;
		if (iteration > 0x400) {
			break;
		}
		if (converged) {
			++converged_count;
		}
	}
	angle_ = angle;
	if (!converged) {
		const float step = angle_step_;
		angle_step_ = std::min(step + step, 0.017f);
		if (std::fabs(step) >= 0.017f) {
			return false;
		}
	}
	const float c = std::cos(angle) / horizontal;
	r_direction = Vec3{ r_target.x * c, std::sin(angle), c * r_target.z };
	const float len = length3(r_direction.x, r_direction.y, r_direction.z);
	if (len < 0.95f) {
		return false;
	}
	const float inv = 1.0f / len;
	r_direction = Vec3{ r_direction.x * inv, r_direction.y * inv, inv * r_direction.z };
	if (r_impact_time != nullptr) {
		*r_impact_time = impact_at;
	}
	return true;
}

void Aimer::predicted_target_position(double p_time, Vec3 &r_out) const {
	const float dt = grenade ? kGrenadeFuseSeconds : static_cast<float>(p_time - time_);
	r_out = Vec3{ relative_pos_.x + target_velocity_.x * dt, relative_pos_.y + target_velocity_.y * dt,
		dt * target_velocity_.z + relative_pos_.z };
	if (grenade && !grenade_exact) {
		r_out = Vec3{ r_out.x - relative_pos_.x * 0.1f, r_out.y - relative_pos_.y * 0.1f,
			r_out.z - relative_pos_.z * 0.1f };
	}
}

bool Aimer::impact_position(Vec3 &r_out, const Vec3 &p_direction, float p_time) const {
	if (p_time > 100.0f) {
		return false;
	}
	const Vec3 velocity{ p_direction.x * speed + shooter_velocity_.x, p_direction.y * speed + shooter_velocity_.y,
		speed * p_direction.z + shooter_velocity_.z };
	predicted_impact_position(velocity, p_time, r_out);
	return true;
}

bool Aimer::nearest_impact_point(const Vec3 &p_direction, Vec3 &r_out, double &r_time) const {
	const float vx = p_direction.x * speed + shooter_velocity_.x;
	const float vy = p_direction.y * speed + shooter_velocity_.y;
	const float vz = p_direction.z * speed + shooter_velocity_.z;
	const float k = 1.0f / (1.0f - drag);
	const float ay = (vy * drag + gravity) * k;
	const float ax = (vx * drag + 0.0f) * k;
	const float az = k * (vz * drag + 0.0f);
	const float rx = vx - target_velocity_.x;
	const float ry = vy - target_velocity_.y;
	const float rz = vz - target_velocity_.z;
	const float c0 = -(rz * relative_pos_.z + relative_pos_.y * ry + relative_pos_.x * rx);
	const float c1 =
			(rz * rz + rx * rx + ry * ry) - (relative_pos_.x * ax + relative_pos_.y * ay + relative_pos_.z * az);
	std::vector<float> roots;
	cubic_roots(c0 + c0, c1 + c1, (rz * az + rx * ax + ry * ay) * 3.0f, az * az + ax * ax + ay * ay, roots);
	float best = 3.4028235e+38f;
	bool found = false;
	for (const float root : roots) {
		if (0.0f <= root && root <= 100.0f) {
			Vec3 impact{};
			impact_position(impact, p_direction, root);
			const float px = target_velocity_.x * root + relative_pos_.x;
			const float py = target_velocity_.y * root + relative_pos_.y;
			const float pz = root * target_velocity_.z + relative_pos_.z;
			const float dx = px - impact.x, dy = py - impact.y, dz = pz - impact.z;
			const float d2 = dx * dx + dy * dy + dz * dz;
			if (d2 < best) {
				r_time = root;
				r_out = impact;
				found = true;
				best = d2;
			}
		}
	}
	return found;
}

bool aim_vector(Aimer &r_aimer, const Vec3 &p_origin, const Vec3 &p_target, const Vec3 &p_target_velocity,
		const Vec3 &p_own_velocity, float p_accuracy, float p_drag, float p_speed, float p_gravity, double p_time,
		Vec3 &r_direction) {
	r_aimer.drag = p_drag;
	r_aimer.speed = p_speed;
	r_aimer.gravity = p_gravity;
	r_aimer.set_target_data(Vec3{ p_target.x - p_origin.x, p_target.y - p_origin.y, p_target.z - p_origin.z },
			p_target_velocity, p_own_velocity, p_time);
	Vec3 predicted{};
	return r_aimer.firing_direction(p_time, p_accuracy, false, r_direction, predicted, nullptr);
}

}
