#pragma once

#include "bf3/sim/sim_types.h"

namespace bf3::host {

inline constexpr double kNsPerTick = 1e9 / double(sim::kSimHz);
inline constexpr double kMsPerTick = 1000.0 / double(sim::kSimHz);

}
