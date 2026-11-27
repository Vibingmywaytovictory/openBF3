#pragma once

#include "bf3/core/math.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace bf3::sim {

// STAND-IN: the sim ticks at 60 Hz, where notes R-A0 give BF3's 30 Hz tick; the sway and fire rules step their
// 30 Hz update every second tick. Settle: R-A0's 30 Hz tick for the whole sim.
inline constexpr uint32_t kSimHz = 60;
inline constexpr float kTickSeconds = 1.0f / float(kSimHz);
using Tick = uint32_t;

constexpr uint32_t seconds_to_ticks(float p_seconds) {
	return p_seconds <= 0.0f ? 0u : uint32_t(p_seconds * float(kSimHz) + 0.5f);
}

using Vec3 = core::Vec3;

enum class Pose : uint8_t { Stand = 0, Crouch = 1, Prone = 2 };
inline constexpr size_t kPoseCount = 3;

}
