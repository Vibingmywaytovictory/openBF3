#pragma once

#include <cstdint>

namespace bf3::sim {

struct NavQueryStats {
	uint64_t nearest = 0;
	uint64_t paths_started = 0;
	uint64_t paths_done = 0;
	uint64_t paths_partial = 0;
	uint64_t paths_failed = 0;
	uint64_t path_iterations = 0;
	uint64_t corridor_moves = 0;
	uint64_t random_points = 0;
	double ms_nearest = 0.0;
	double ms_path = 0.0;
	double ms_corridor = 0.0;
};

}

namespace bf3::sim::bots {

struct WalkerStats {
	uint64_t ticks = 0;
	uint64_t thinks = 0;
	uint64_t goals_started = 0;
	uint64_t goals_reached = 0;
	uint64_t goals_abandoned = 0;
	uint64_t path_requests = 0;
	uint64_t path_partial = 0;
	uint64_t path_failed = 0;
	uint64_t repaths = 0;
	uint64_t stuck_events = 0;
	uint64_t off_mesh_ticks = 0;
	uint64_t sprint_ticks = 0;
	uint64_t crouch_ticks = 0;
	double metres = 0.0;

	uint64_t starts_outside_main = 0;

	void add(const WalkerStats &p_other);
};

struct PathServiceStats {
	uint64_t runs = 0;
	uint64_t iterations = 0;
	uint64_t ticks_at_budget = 0;
	uint64_t requests = 0;
	uint64_t answers = 0;
	uint32_t pending_max = 0;
	uint32_t iterations_last = 0;
	double ms_run = 0.0;
	double ms_run_max = 0.0;
};

}
