#pragma once

#include "bf3/net/protocol.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/weapons/sim_time.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace bf3::net {

inline constexpr size_t kTrackSamples = 8;
inline constexpr uint32_t kTrackTimeoutTicks = 60;

struct TimedOther {
	uint32_t server_tick = 0;
	OtherSoldier soldier;
};
static_assert(std::is_trivially_copyable_v<TimedOther>);

enum class SampleKind : uint8_t { Interpolate = 0, Extrapolate = 1, Hold = 2 };

struct OtherPose {
	uint8_t slot = 0;
	uint8_t generation = 0;
	uint8_t pose = 0;
	uint8_t target_pose = 0;
	uint8_t transition = 0;
	uint8_t mode = 0;
	uint8_t flags = 0;
	bool extrapolated = false;
	bool held = false;
	sim::Vec3 position{};
	sim::Vec3 velocity{};
	float yaw = 0;
	float pitch = 0;
	float eye_height = 0;
	uint32_t tick_a = 0;
	uint32_t tick_b = 0;
	uint8_t kind = 0;
};
static_assert(std::is_trivially_copyable_v<OtherPose>);

OtherPose sample_other(std::span<const TimedOther> p_samples, double p_render_ticks, std::span<const float, sim::kPoseCount> p_pose_eye_heights,
		double p_max_extrapolate_ticks);

int64_t rewind_time_q8(sim::weapons::SimTime p_segment_start, int32_t p_view_back_q8, uint32_t p_max_rewind_ticks);

size_t rebuild_track(std::span<const TimedOther> p_candidates, uint32_t p_view_tick, std::span<TimedOther, kTrackSamples> r_list);

struct SampleSelection {
	uint32_t index_a = 0;
	uint32_t index_b = 0;
	uint32_t tick_a = 0;
	uint32_t tick_b = 0;
	SampleKind kind = SampleKind::Hold;
	bool held = false;
	bool extrapolated = false;
	bool valid = false;
};
SampleSelection select_sample(std::span<const TimedOther> p_samples, double p_render_ticks, double p_max_extrapolate_ticks);

int64_t sim_time_q8(sim::weapons::SimTime p_time);

constexpr int32_t view_lag_q8(int32_t p_view_back_q8) {
	return p_view_back_q8 - 256;
}

bool same_bits(const OtherPose &p_a, const OtherPose &p_b);
bool same_bits(const TimedOther &p_a, const TimedOther &p_b);

}
