#pragma once
// Plain types shared by the simulation, the protocol and the presentation (docs/design/m1.md §4, §6, §7).
// No Jolt and no Godot here: bf3_net, bf3_host and bf3_godot include this.

#include <array>
#include <cstddef>
#include <cstdint>

namespace bf3::sim {

// Fixed simulation rate (plan D4: 60 Hz simulation, 30 Hz snapshots). Ticks are integers and time is
// tick x kTickSeconds; every rate in the motor is written per second (topics/05 §4.1), so a 30 Hz build only
// changes these two constants.
inline constexpr uint32_t kSimHz = 60;
inline constexpr float kTickSeconds = 1.0f / float(kSimHz);
using Tick = uint32_t;

// Whole ticks for a duration, rounded to the nearest tick: the pose transitions of plan §7.2 are exact
// (0.3 s = 18, 0.5 s = 30, 0.7 s = 42, 1.05 s = 63 ticks at 60 Hz).
constexpr uint32_t seconds_to_ticks(float p_seconds) {
	return p_seconds <= 0.0f ? 0u : uint32_t(p_seconds * float(kSimHz) + 0.5f);
}

// Frostbite space as bf3_core gives it: metres, Y up, forward = +Z. Godot uses the same axes; only the camera
// looks down -Z (render_bridge.h), so a Godot camera with view yaw y has basis rotate_y(y + pi).
using Vec3 = std::array<float, 3>;

// Soldier poses, numbered like bf3::core::gameplay::SoldierPose (Stand 0, Crouch 1, Prone 2); soldier_motor.cpp
// static_asserts the match.
enum class Pose : uint8_t { Stand = 0, Crouch = 1, Prone = 2 };
inline constexpr size_t kPoseCount = 3;

} // namespace bf3::sim
