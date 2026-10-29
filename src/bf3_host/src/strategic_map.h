#pragma once

#include "bf3/core/result.h"
#include "bf3/host/bot_fill.h"

#include <span>

namespace bf3::core::level {
struct ModeObjectiveDef;
}

namespace bf3::host {

core::Status build_strategic_map(BotLevel &r_level, std::span<const core::level::ModeObjectiveDef> p_objectives);

}
