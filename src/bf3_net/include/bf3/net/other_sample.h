#pragma once
// The Hermite sampler of one other soldier and the client's track rules, shared by the client's presentation and the
// server's lag compensation (docs/design/m2a.md §6.1, §6.3, §6.4; owner lc). Godot-free, pure functions.
//
// One function on both ends: lc writes sample_other from ClientSession's present_others body (client_session.cpp),
// and cl replaces that body with a call to it (named replacement R1, §15). The selection rule is M1's, unchanged:
// the bracketing pair is the newest sample at or before the render time and the next one; before the oldest
// sample, hold it; past the newest, extrapolate along its velocity for at most p_max_extrapolate_ticks (250 ms),
// then hold. Positions are Hermite with the sample velocities, velocity lerped, yaw along the shortest arc, pitch
// and eye height lerped. The discrete fields (pose, target_pose, transition, mode, flags, generation) all switch
// together at the newer sample (u >= 1). tick_a / tick_b / kind say which samples were used: the server's pose
// cache key and the E12 trace rows use them.
//
// One list on both ends: rebuild_track applies the client's track rules (client_session.cpp add_others / publish)
// to one target's candidate records, so the server (candidates from LagHistory under the shot's received set) and
// the client (candidates from its ReceivedRecords under the same set) sample the same list:
//   * sorted by tick, a duplicate tick kept once (add_others inserts reordered samples in tick order and skips a
//     duplicate);
//   * only the newest kTrackSamples ticks (pop_front after the insert keeps the 8 newest whatever the arrival order);
//   * only the generation of the newest record in the set (add_others resets the track on a generation change);
//   * empty when the newest record is more than kTrackTimeoutTicks older than p_view_tick (the client drops a track
//     after kTrackTimeoutNs = 1 s without a sample).
// Lag compensation passes the render time as double(time_q8) / 256, exact in a double, on both ends.

#include "bf3/net/protocol.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/weapons/sim_time.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace bf3::net {

inline constexpr size_t kTrackSamples = 8; // client_session.cpp kTrackSamples
inline constexpr uint32_t kTrackTimeoutTicks = 60; // 1 s at 60 Hz (client_session.cpp kTrackTimeoutNs)

// One received record of one soldier at a server tick (= host::OtherSample, moved here; cl switches the alias).
struct TimedOther {
	uint32_t server_tick = 0;
	OtherSoldier soldier;
};
static_assert(std::is_trivially_copyable_v<TimedOther>);

enum class SampleKind : uint8_t { Interpolate = 0, Extrapolate = 1, Hold = 2 };

// One soldier sampled at a render time: what present_others draws and what the lag compensation poses.
struct OtherPose {
	uint8_t slot = 0;
	uint8_t generation = 0;
	uint8_t pose = 0; // sim::Pose
	uint8_t target_pose = 0; // sim::Pose
	uint8_t transition = 0; // 0..255 (OtherSoldier::transition)
	uint8_t mode = 0; // sim::MoveMode
	uint8_t flags = 0; // low 8 bits of soldier_flags::
	bool extrapolated = false; // the render time was past the newest sample
	bool held = false; // before the oldest sample, or past the extrapolation limit
	sim::Vec3 position{}; // feet, world
	sim::Vec3 velocity{};
	float yaw = 0; // radians
	float pitch = 0;
	float eye_height = 0; // blended over the pose transition
	uint32_t tick_a = 0; // the samples used (tick_a == tick_b when one sample was used)
	uint32_t tick_b = 0;
	uint8_t kind = 0; // SampleKind
};
static_assert(std::is_trivially_copyable_v<OtherPose>);

// p_samples: one soldier's samples, oldest first, distinct ticks (the client's track after its rules). An empty span
// gives a default OtherPose with kind Hold.
OtherPose sample_other(std::span<const TimedOther> p_samples, double p_render_ticks, std::span<const float, sim::kPoseCount> p_pose_eye_heights,
		double p_max_extrapolate_ticks);

// The time rule of §6.4, both ends: the target time of a projectile segment starting at p_segment_start for a shot
// whose command carried p_view_back_q8, in q8 server ticks (ticks x 256), rounded to 1/256 tick. A lag beyond
// p_max_rewind_ticks is clamped (the caller counts it: LagCompStats::clamped).
int64_t rewind_time_q8(sim::weapons::SimTime p_segment_start, int32_t p_view_back_q8, uint32_t p_max_rewind_ticks);

// The client's track rules over one target's candidate records (any order, duplicates allowed); writes the list
// oldest first into r_list and returns its length (<= kTrackSamples).
size_t rebuild_track(std::span<const TimedOther> p_candidates, uint32_t p_view_tick, std::span<TimedOther, kTrackSamples> r_list);

} // namespace bf3::net
