#pragma once
// Plain counters of the S1 nav queries, path service and walker brains (spike S1; docs/design/s1.md §6.4, §7, §8).
// A light header on purpose: GameServerStats carries these through bf3/host/bot_fill_stats.h, and game_server.h must
// not pull nav.h, nav_query.h or walker_brain.h into every M1 includer (bf3_godot, the tests). Godot-free; only
// <cstdint>. Every struct is trivially copyable (s1.contract checks).

#include <cstdint>

namespace bf3::sim {

// Cumulative counters of one NavQuery (nav_query.h; the bench reports them per second and per call).
struct NavQueryStats {
	uint64_t nearest = 0;
	uint64_t paths_started = 0;
	uint64_t paths_done = 0; // complete paths
	uint64_t paths_partial = 0; // DT_PARTIAL_RESULT: the goal was not reached (other component, node pool)
	uint64_t paths_failed = 0;
	uint64_t path_iterations = 0; // updateSlicedFindPath iterations
	uint64_t corridor_moves = 0; // NavCorridor::move_position
	uint64_t random_points = 0;
	double ms_nearest = 0.0;
	double ms_path = 0.0; // init + update + finalize of the sliced searches
	double ms_corridor = 0.0;
};

} // namespace bf3::sim

namespace bf3::sim::bots {

// Summed over a brain's life (walker_brain.h).
struct WalkerStats {
	uint64_t ticks = 0; // commands written
	uint64_t thinks = 0;
	uint64_t goals_started = 0;
	uint64_t goals_reached = 0;
	uint64_t goals_abandoned = 0; // max_repaths reached
	uint64_t path_requests = 0;
	uint64_t path_partial = 0;
	uint64_t path_failed = 0;
	uint64_t repaths = 0;
	uint64_t stuck_events = 0;
	uint64_t off_mesh_ticks = 0;
	uint64_t sprint_ticks = 0;
	uint64_t crouch_ticks = 0;
	double metres = 0.0; // horizontal distance walked (from the states)
	// Path requests whose start poly lay outside the component of the bot's goals (docs §7): the brain then draws its
	// goals from its own component. Non-zero means a bot spawned or fell outside the main component; the bench
	// reports it beside the spawn filter's counts so G1's validity stays independent of G2 (review of revision 2).
	uint64_t starts_outside_main = 0;

	void add(const WalkerStats &p_other);
};

// PathService counters (walker_brain.h). The bench derives the cost of one sliced A* iteration (ms_run /
// iterations, an upper bound: init and finalize are inside) and how often the per-tick budget was the limit
// (ticks_at_budget), so docs §14's "lower iterations_per_tick" step is chosen from data (review of revision 1).
struct PathServiceStats {
	uint64_t runs = 0; // run() calls (one per tick)
	uint64_t iterations = 0; // A* iterations spent, total
	uint64_t ticks_at_budget = 0; // runs that spent the whole iterations_per_tick with requests still queued
	uint64_t requests = 0;
	uint64_t answers = 0; // Done + Partial + Failed handed out
	uint32_t pending_max = 0; // longest queue seen
	uint32_t iterations_last = 0; // of the last run
	double ms_run = 0.0; // inside run(), total
	double ms_run_max = 0.0; // the longest single run()
};

} // namespace bf3::sim::bots
