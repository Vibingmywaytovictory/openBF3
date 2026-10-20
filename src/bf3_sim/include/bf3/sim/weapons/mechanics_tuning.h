#pragma once

#include "bf3/core/result.h"
#include "bf3/sim/weapons/mechanics.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace bf3::sim::weapons {

inline constexpr size_t kMaxMechanicsTuningBytes = 64 * 1024;
inline constexpr size_t kMaxMechanicsTuningLines = 1024;

inline constexpr std::string_view kMechanicsTuningPath = "config/weapon_mechanics.cfg";

core::Result<Mechanics> parse_mechanics_tuning(std::string_view p_text);
std::string format_mechanics_tuning(const Mechanics &p_mechanics);

}
