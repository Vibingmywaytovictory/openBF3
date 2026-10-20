#pragma once

#include "bf3/net/protocol.h"
#include "bf3/sim/sim_types.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace bf3::host {

struct RestSample {
	uint64_t time_ns = 0;
	sim::Tick tick = 0;
	bool input_idle = false;
	float horizontal_speed = 0.0f;
	sim::Vec3 eye{};
	float visual_offset_m = 0.0f;
	uint64_t reconciliations = 0;
	sim::Tick last_reconcile_tick = 0;
};

struct RestCheckOptions {
	float rest_speed = 1e-4f;
	double max_stop_s = 1.0;
	double settle_s = 0.5;
	double window_s = 10.0;
	float max_eye_jitter_m = 1e-4f;

	uint32_t edge_ticks = uint32_t(net::kCommandsPerPacket) + 6;
};

struct RestCycleResult {
	bool ok = false;
	std::array<char, 96> failure{};
	sim::Tick stop_tick = 0;
	uint64_t stop_ns = 0;
	uint64_t rest_ns = 0;
	uint64_t window_start_ns = 0;
	uint64_t window_end_ns = 0;
	uint64_t period_end_ns = 0;
	bool pushed = false;
	uint32_t edge_corrections = 0;
	uint32_t window_reconciliations = 0;
	float eye_jitter_m = 0.0f;
	float max_visual_offset_m = 0.0f;
};

std::vector<RestCycleResult> judge_rest(std::span<const RestSample> p_samples, const RestCheckOptions &p_options = {});

}
