#pragma once
// The E2 gate "no rubber-banding at rest" as one pure function (plan §6 M1 exit; docs/design/m1.md §1 E2, §14),
// so host.session (T4) and godot.m1 (T6) judge the same recording the same way. Godot-free.
//
// The test drives walk-stop cycles (walk, then rest with no input) and records one RestSample per presented frame
// (godot.m1) or per PresentationFrame publish (host.session). judge_rest splits the recording into rest periods
// (maximal runs of samples with an idle command) and judges each:
//   stop tick   the first idle sample's tick (for the idle phase after the spawn: its first sample)
//   rest        the first sample of the period with horizontal_speed <= rest_speed; it must come within
//               max_stop_s of the stop (else "no rest": creep, not rubber-banding, is reported as such)
//   window      starts settle_s after the rest and lasts window_s
//   push        a reconciliation in the period whose snapshot tick is <= stop tick + edge_ticks is the input-edge
//               case of docs §7.5 (a lost or late stop command made the server repeat a moving one); it moves the
//               window start to that sample's time + settle_s. Only such a correction may move the window
//   fail        any other reconciliation in the period (a correction at rest), a window that does not fit inside
//               the period, a reconciliation inside the window, eye jitter (max - min per axis over the window)
//               >= max_eye_jitter_m, or a visual offset other than exactly 0 inside the window

#include "bf3/net/protocol.h"
#include "bf3/sim/sim_types.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace bf3::host {

struct RestSample {
	uint64_t time_ns = 0; // net::monotonic_ns of the frame (or publish)
	sim::Tick tick = 0; // PresentationFrame::tick
	bool input_idle = false; // PresentationFrame::own_command has no move and no buttons
	float horizontal_speed = 0.0f; // |own_curr.velocity| in x and z
	sim::Vec3 eye{}; // the presented eye position: blended feet + visual_offset + blended eye height
	float visual_offset_m = 0.0f; // |PresentationFrame::visual_offset|
	uint64_t reconciliations = 0; // ClientStats::reconciliations (cumulative; an increase is a reconciliation)
	sim::Tick last_reconcile_tick = 0; // ClientStats::last_reconcile_tick (snapshot tick of the newest one)
};

struct RestCheckOptions {
	float rest_speed = 1e-4f; // m/s
	double max_stop_s = 1.0; // stop -> rest (a 6.5 m/s sprint stops in 0.43 s at the default 15 m/s^2)
	double settle_s = 0.5;
	double window_s = 10.0;
	float max_eye_jitter_m = 1e-4f; // 0.1 mm
	// kCommandsPerPacket + ClientSessionConfig::max_buffer_ticks (checked in m1.contract)
	uint32_t edge_ticks = uint32_t(net::kCommandsPerPacket) + 6;
};

struct RestCycleResult {
	bool ok = false;
	std::array<char, 96> failure{}; // NUL-terminated reason when !ok
	sim::Tick stop_tick = 0;
	uint64_t stop_ns = 0;
	uint64_t rest_ns = 0;
	uint64_t window_start_ns = 0;
	uint64_t window_end_ns = 0;
	uint64_t period_end_ns = 0; // the last idle sample of the period
	bool pushed = false; // an input-edge correction moved the window
	uint32_t edge_corrections = 0;
	uint32_t window_reconciliations = 0;
	float eye_jitter_m = 0.0f;
	float max_visual_offset_m = 0.0f;
};

// One result per rest period, in order. A recording with no rest period gives an empty vector (the caller fails
// that). Pure; tests feed it synthetic recordings.
std::vector<RestCycleResult> judge_rest(std::span<const RestSample> p_samples, const RestCheckOptions &p_options = {});

} // namespace bf3::host
