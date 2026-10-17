#pragma once
// Command: one tick of one soldier's input, the only thing a client tells the authoritative server about its
// soldier (plan §2.4 step 4, §3.5 "Tick", D11 "bots emit the same Command as humans"; topics/05 §7.7). Bots
// and scripted walkers write the same struct on the server.
//
// The values are quantised where they are made (the client's input sampling), never on the wire only: the
// client predicts with exactly the numbers the server receives, so both run the same arithmetic on replay
// (plan R4, docs/design/m1.md §6.11). The bit layout on the wire is bf3_net protocol.h's.
//
// Angles: yaw turns about +Y; yaw 0 looks down +Z (Frostbite forward), yaw a quarter turn looks down +X, so the
// view direction is (sin yaw cos pitch, sin pitch, cos yaw cos pitch). Pitch > 0 looks up.

#include "bf3/sim/sim_types.h"

#include <cstdint>

namespace bf3::sim {

namespace buttons {
inline constexpr uint32_t kJump = 1u << 0; // edge: set on the one command of the tick the key went down
inline constexpr uint32_t kSprint = 1u << 1; // held
inline constexpr uint32_t kFire = 1u << 2; // reserved (M2a)
inline constexpr uint32_t kZoom = 1u << 3; // reserved (M2a)
inline constexpr uint32_t kReload = 1u << 4; // reserved (M2a)
inline constexpr uint32_t kUse = 1u << 5; // reserved
inline constexpr uint32_t kEdgeMask = kJump; // edge buttons a repeated command must not repeat
inline constexpr uint32_t kWireMask = (1u << 24) - 1; // 24 bits on the wire (topics/05 §7.7)
} // namespace buttons

namespace command_flags {
// Set by the server only: the client's command for this tick was missing, so the previous one was repeated
// with its edge buttons cleared (topics/05 §4.1 "the server repeats the last command when one is late").
inline constexpr uint8_t kRepeated = 1u << 0;
// Set by the server only: the command came from a server-side source (bot, scripted walker), not a peer.
inline constexpr uint8_t kLocalSource = 1u << 1;
} // namespace command_flags

// M2a (docs/design/m2a.md §6.2): how the drawn frame showed the others when the command was sampled (2 bits on the
// wire).
enum class ViewQuality : uint8_t { None = 0, Interpolated = 1, Extrapolated = 2, Held = 3 };

struct Command {
	Tick tick = 0; // the server tick this command drives (the client aims it 1-2 ticks ahead, §7.5)
	uint32_t sequence = 0; // +1 per command of one client session: acks and dedupe
	uint32_t buttons = 0; // buttons::
	int8_t move_forward = 0; // stick, -127..127 (keyboard = +-127); -128 is never produced
	int8_t move_right = 0;
	uint16_t yaw = 0; // view yaw, 2^16 steps per turn
	int16_t pitch = 0; // view pitch, kPitchStepsPerHalfTurn steps per half turn, clamped to +-kPitchLimit
	Pose pose = Pose::Stand; // the pose the player asks for (toggles resolved by the client's input mapping)
	uint8_t flags = 0; // command_flags:: (server side only; never sent by a client)
	// M2a view sample (docs/design/m2a.md §6.2): what the player saw when this command's input was sampled, so the
	// server tests the command's shots against the targets as that client drew them (lag compensation, §6.3).
	// 26 bits on the wire (bf3_net protocol.h v2). Bots and server-side sources leave them 0 (quality None: they
	// fire at the present, plan D7).
	uint16_t view_back_q8 = 0; // (tick << 8) - render_time_q8, server ticks x 256, saturated at 65,535 (4.27 s)
	uint8_t view_quality = 0; // ViewQuality, from present_others' counters at that frame
	uint8_t view_seq8 = 0; // low 8 bits of the newest Snapshot seq (this client's seq space) received before the
			// drawn frame was published

	bool operator==(const Command &) const = default;
};

// ---- quantisation (the contract both sides use; bit counts in bf3_net protocol.h) ----------------------------

inline constexpr float kTwoPi = 6.28318530717958647692f;
inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr int32_t kYawSteps = 65536;
inline constexpr int32_t kPitchStepsPerHalfTurn = 16384; // 14-bit pitch range over -90..+90 degrees
inline constexpr int16_t kPitchLimit = 8191; // +-89.99 degrees
inline constexpr int8_t kAxisMax = 127;

namespace detail {
// floor(x + 0.5) without <cmath> (constexpr; |x| stays far below 2^31 here).
constexpr int32_t round_half_up(float p_x) {
	const int32_t t = int32_t(p_x);
	const float f = p_x - float(t);
	return f >= 0.5f ? t + 1 : (f < -0.5f ? t - 1 : t);
}
} // namespace detail

// Radians (any value) -> yaw steps, wrapped to one turn.
constexpr uint16_t quantize_yaw(float p_radians) {
	const float turns = p_radians / kTwoPi;
	const float frac = turns - float(int32_t(turns)) + 1.0f; // (0, 2)
	return uint16_t(uint32_t(detail::round_half_up(frac * float(kYawSteps))) & 0xFFFFu);
}
constexpr float yaw_radians(uint16_t p_yaw) {
	return float(p_yaw) * (kTwoPi / float(kYawSteps));
}
// Radians -> pitch steps, clamped to +-kPitchLimit.
constexpr int16_t quantize_pitch(float p_radians) {
	int32_t s = detail::round_half_up(p_radians * (float(kPitchStepsPerHalfTurn) / kPi));
	s = s > kPitchLimit ? kPitchLimit : (s < -kPitchLimit ? -kPitchLimit : s);
	return int16_t(s);
}
constexpr float pitch_radians(int16_t p_pitch) {
	return float(p_pitch) * (kPi / float(kPitchStepsPerHalfTurn));
}
// Stick axis -1..1 -> -127..127.
constexpr int8_t quantize_axis(float p_value) {
	const float c = p_value > 1.0f ? 1.0f : (p_value < -1.0f ? -1.0f : p_value);
	return int8_t(detail::round_half_up(c * float(kAxisMax)));
}
constexpr float axis_value(int8_t p_axis) {
	return float(p_axis < -kAxisMax ? -kAxisMax : p_axis) / float(kAxisMax);
}

// The command a server runs when the client's command for p_tick is missing: the last one, edges cleared.
constexpr Command repeated_command(const Command &p_last, Tick p_tick) {
	Command c = p_last;
	c.tick = p_tick;
	c.buttons &= ~buttons::kEdgeMask;
	c.flags = uint8_t(c.flags | command_flags::kRepeated);
	return c;
}

// No input: the view and pose kept (spawn settle, SpawnSweep dummies, the free camera's neutral command).
constexpr Command idle_command(Tick p_tick, uint16_t p_yaw, int16_t p_pitch, Pose p_pose) {
	Command c;
	c.tick = p_tick;
	c.yaw = p_yaw;
	c.pitch = p_pitch;
	c.pose = p_pose;
	c.flags = command_flags::kLocalSource;
	return c;
}

} // namespace bf3::sim
