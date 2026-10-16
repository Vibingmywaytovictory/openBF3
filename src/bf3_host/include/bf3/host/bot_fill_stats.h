#pragma once
// The bot counters GameServerStats carries (spike S1; docs/design/s1.md §6.1, §8). Kept apart from bot_fill.h so
// that game_server.h includes only this light POD header (plus a forward declaration of BotFillConfig) and the M1
// compile surface of game_server.h's includers stays as it was (review of revision 1). Godot-free.

#include "bf3/sim/bots/bot_stats.h"

#include <cstdint>

namespace bf3::host {

// Per tick and cumulative bot counters (GameServerStats::bots). Trivially copyable.
struct BotFillStats {
	uint32_t bots = 0;
	uint32_t thinks_last_tick = 0;
	uint32_t path_iterations_last_tick = 0;
	uint32_t paths_pending = 0;
	double last_brains_ms = 0.0; // PathService::run + every brain's command() of the last tick
	double last_paths_ms = 0.0; // of which PathService::run
	sim::bots::WalkerStats walkers; // summed over the bots, cumulative
	sim::bots::PathServiceStats paths; // the server PathService, cumulative
	sim::NavQueryStats query; // the server NavQuery's cumulative counters
};

} // namespace bf3::host
