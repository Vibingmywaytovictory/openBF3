#pragma once

#include "bf3/sim/bots/bot_stats.h"

#include <cstdint>

namespace bf3::host {

struct BotFillStats {
	uint32_t bots = 0;
	uint32_t thinks_last_tick = 0;
	uint32_t path_iterations_last_tick = 0;
	uint32_t paths_pending = 0;
	double last_brains_ms = 0.0;
	double last_paths_ms = 0.0;
	sim::bots::WalkerStats walkers;
	sim::bots::PathServiceStats paths;
	sim::NavQueryStats query;
};

}
