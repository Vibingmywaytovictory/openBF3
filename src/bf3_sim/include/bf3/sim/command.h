#pragma once

#include "bf3/sim/sim_types.h"

#include <cstdint>

namespace bf3::sim {

namespace buttons {
inline constexpr uint32_t kJump = 1u << 0;
inline constexpr uint32_t kSprint = 1u << 1;
inline constexpr uint32_t kFire = 1u << 2;
inline constexpr uint32_t kZoom = 1u << 3;
inline constexpr uint32_t kReload = 1u << 4;
inline constexpr uint32_t kUse = 1u << 5;
inline constexpr uint32_t kEdgeMask = kJump;
inline constexpr uint32_t kWireMask = (1u << 24) - 1;
}

namespace command_flags {

inline constexpr uint8_t kRepeated = 1u << 0;

inline constexpr uint8_t kLocalSource = 1u << 1;
}

enum class ViewQuality : uint8_t { None = 0, Interpolated = 1, Extrapolated = 2, Held = 3 };

struct Command {
	Tick tick = 0;
	uint32_t sequence = 0;
	uint32_t buttons = 0;
	int8_t move_forward = 0;
	int8_t move_right = 0;
	uint16_t yaw = 0;
	int16_t pitch = 0;
	Pose pose = Pose::Stand;
	uint8_t flags = 0;

	uint16_t view_back_q8 = 0;
	uint8_t view_quality = 0;
	uint8_t view_seq8 = 0;

	bool operator==(const Command &) const = default;
};

inline constexpr float kTwoPi = 6.28318530717958647692f;
inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr int32_t kYawSteps = 65536;
inline constexpr int32_t kPitchStepsPerHalfTurn = 16384;
inline constexpr int16_t kPitchLimit = 8191;
inline constexpr int8_t kAxisMax = 127;

namespace detail {

constexpr int32_t round_half_up(float p_x) {
	const int32_t t = int32_t(p_x);
	const float f = p_x - float(t);
	return f >= 0.5f ? t + 1 : (f < -0.5f ? t - 1 : t);
}
}

constexpr uint16_t quantize_yaw(float p_radians) {
	const float turns = p_radians / kTwoPi;
	const float frac = turns - float(int32_t(turns)) + 1.0f;
	return uint16_t(uint32_t(detail::round_half_up(frac * float(kYawSteps))) & 0xFFFFu);
}
constexpr float yaw_radians(uint16_t p_yaw) {
	return float(p_yaw) * (kTwoPi / float(kYawSteps));
}

constexpr int16_t quantize_pitch(float p_radians) {
	int32_t s = detail::round_half_up(p_radians * (float(kPitchStepsPerHalfTurn) / kPi));
	s = s > kPitchLimit ? kPitchLimit : (s < -kPitchLimit ? -kPitchLimit : s);
	return int16_t(s);
}
constexpr float pitch_radians(int16_t p_pitch) {
	return float(p_pitch) * (kPi / float(kPitchStepsPerHalfTurn));
}

constexpr int8_t quantize_axis(float p_value) {
	const float c = p_value > 1.0f ? 1.0f : (p_value < -1.0f ? -1.0f : p_value);
	return int8_t(detail::round_half_up(c * float(kAxisMax)));
}
constexpr float axis_value(int8_t p_axis) {
	return float(p_axis < -kAxisMax ? -kAxisMax : p_axis) / float(kAxisMax);
}

constexpr Command repeated_command(const Command &p_last, Tick p_tick) {
	Command c = p_last;
	c.tick = p_tick;
	c.buttons &= ~buttons::kEdgeMask;
	c.flags = uint8_t(c.flags | command_flags::kRepeated);
	return c;
}

constexpr Command idle_command(Tick p_tick, uint16_t p_yaw, int16_t p_pitch, Pose p_pose) {
	Command c;
	c.tick = p_tick;
	c.yaw = p_yaw;
	c.pitch = p_pitch;
	c.pose = p_pose;
	c.flags = command_flags::kLocalSource;
	return c;
}

}
