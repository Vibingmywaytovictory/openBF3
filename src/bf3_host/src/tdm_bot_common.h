#pragma once

#include "bf3/core/util/finite.h"
#include "bf3/sim/command.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/weapons/weapon_rng.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace bf3::host::tdm_bot {

inline constexpr uint8_t kSlotPrimary = 0, kSlotSidearm = 1, kSlotGrenade = 6, kSlotKnife = 7;

inline uint64_t mix(uint64_t p_x) {
	return sim::weapons::splitmix64(p_x);
}
inline uint64_t next(uint64_t &r_state) {
	r_state = mix(r_state);
	return r_state;
}
inline float unit(uint64_t &r_state) {
	return float(next(r_state) >> 40) / float(1ull << 24);
}
inline float uniform(uint64_t &r_state, float p_min, float p_max) {
	return p_min + (p_max - p_min) * unit(r_state);
}
inline uint32_t uniform_u(uint64_t &r_state, uint32_t p_min, uint32_t p_max) {
	if (p_max <= p_min) {
		return p_min;
	}
	return p_min + uint32_t(next(r_state) % uint64_t(p_max - p_min + 1));
}
inline float gauss(uint64_t &r_state) {
	const float u1 = std::max(unit(r_state), 1e-7f);
	const float u2 = unit(r_state);
	return std::sqrt(-2.0f * std::log(u1)) * std::cos(sim::kTwoPi * u2);
}
inline float wrap(float p_radians) {
	float a = std::fmod(p_radians, sim::kTwoPi);
	if (a > sim::kPi) {
		a -= sim::kTwoPi;
	} else if (a < -sim::kPi) {
		a += sim::kTwoPi;
	}
	return a;
}
inline float yaw_of(float p_dx, float p_dz) {
	return std::atan2(p_dx, p_dz);
}
inline float horizontal(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	return core::vec::horizontal_distance(p_a, p_b);
}
inline float distance(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	const float dx = p_b[0] - p_a[0], dy = p_b[1] - p_a[1], dz = p_b[2] - p_a[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}
inline sim::Vec3 add(const sim::Vec3 &p_a, float p_x, float p_y, float p_z) {
	return { p_a[0] + p_x, p_a[1] + p_y, p_a[2] + p_z };
}
inline sim::Vec3 sub(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	return { p_a[0] - p_b[0], p_a[1] - p_b[1], p_a[2] - p_b[2] };
}
using bf3::core::finite;
inline bool is_alive(const sim::SoldierState &p_state) {
	return (p_state.flags & sim::soldier_flags::kAlive) != 0 && p_state.mode != sim::MoveMode::Dead;
}
inline sim::Vec3 view_dir(float p_yaw, float p_pitch) {
	const float cp = std::cos(p_pitch);
	return { std::sin(p_yaw) * cp, std::sin(p_pitch), std::cos(p_yaw) * cp };
}
inline float ray_point_distance(const sim::Vec3 &p_origin, const sim::Vec3 &p_dir, float p_length, const sim::Vec3 &p_point) {
	const sim::Vec3 d = sub(p_point, p_origin);
	const float t = std::clamp(d[0] * p_dir[0] + d[1] * p_dir[1] + d[2] * p_dir[2], 0.0f, p_length);
	return distance(add(p_origin, p_dir[0] * t, p_dir[1] * t, p_dir[2] * t), p_point);
}
inline uint64_t now_ns() {
	return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

}
