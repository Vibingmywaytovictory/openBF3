// The Hermite sampler of one other soldier, the time rule and the client's track rules (docs/design/m2a.md §6.1,
// §6.3, §6.4; as built: docs/design/m2a-lc.md). See other_sample.h.
//
// sample_other is ClientSession's present_others body (client_session.cpp, M1) moved here unchanged, expression for
// expression, so the R1 replacement (cl) draws exactly what M1 drew: the same float operations in the same order. Only
// the selection is factored out (select_sample), because the server's pose cache keys on it.
#include "bf3/net/other_sample.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace bf3::net {

namespace {

constexpr double kQ8 = 256.0;

float lerp(float p_a, float p_b, float p_t) {
	return p_a + (p_b - p_a) * p_t;
}

// Cubic Hermite between p0 (tangent m0) and p1 (tangent m1), u in [0, 1] (client_session.cpp, M1).
float hermite(float p_p0, float p_m0, float p_p1, float p_m1, float p_u) {
	const float u2 = p_u * p_u, u3 = u2 * p_u;
	return (2 * u3 - 3 * u2 + 1) * p_p0 + (u3 - 2 * u2 + p_u) * p_m0 + (-2 * u3 + 3 * u2) * p_p1 + (u3 - u2) * p_m1;
}

float eye_of(std::span<const float, sim::kPoseCount> p_eyes, const OtherSoldier &p_o) {
	const size_t from = std::min<size_t>(uint8_t(p_o.pose), sim::kPoseCount - 1);
	const size_t to = std::min<size_t>(uint8_t(p_o.target_pose), sim::kPoseCount - 1);
	return lerp(p_eyes[from], p_eyes[to], float(p_o.transition) / 255.0f);
}

void set_discrete(OtherPose &r_out, const OtherSoldier &p_o) {
	r_out.slot = p_o.slot;
	r_out.generation = p_o.generation;
	r_out.pose = uint8_t(p_o.pose);
	r_out.target_pose = uint8_t(p_o.target_pose);
	r_out.transition = p_o.transition;
	r_out.mode = uint8_t(p_o.mode);
	r_out.flags = p_o.flags;
}

// present_sample (client_session.cpp, M1): one record as drawn.
OtherPose present_sample(std::span<const float, sim::kPoseCount> p_eyes, const OtherSoldier &p_o) {
	OtherPose s;
	set_discrete(s, p_o);
	s.position = p_o.position;
	s.velocity = p_o.velocity;
	s.yaw = sim::yaw_radians(p_o.yaw);
	s.pitch = sim::pitch_radians(p_o.pitch);
	s.eye_height = eye_of(p_eyes, p_o);
	return s;
}

template <typename T>
bool same_float(T p_a, T p_b) {
	return std::memcmp(&p_a, &p_b, sizeof(T)) == 0;
}

bool same_vec(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	return same_float(p_a[0], p_b[0]) && same_float(p_a[1], p_b[1]) && same_float(p_a[2], p_b[2]);
}

bool same_other(const OtherSoldier &p_a, const OtherSoldier &p_b) {
	return p_a.slot == p_b.slot && p_a.generation == p_b.generation && same_vec(p_a.position, p_b.position) && same_vec(p_a.velocity, p_b.velocity) &&
			p_a.yaw == p_b.yaw && p_a.pitch == p_b.pitch && p_a.pose == p_b.pose && p_a.target_pose == p_b.target_pose && p_a.transition == p_b.transition &&
			p_a.mode == p_b.mode && p_a.flags == p_b.flags && p_a.weapon == p_b.weapon && p_a.weapon_phase == p_b.weapon_phase && p_a.firing == p_b.firing &&
			p_a.ads == p_b.ads && p_a.reload_seq == p_b.reload_seq && p_a.burst_seq == p_b.burst_seq;
}

} // namespace

SampleSelection select_sample(std::span<const TimedOther> p_samples, double p_render_ticks, double p_max_extrapolate_ticks) {
	SampleSelection s;
	const uint32_t used = uint32_t(std::min<size_t>(p_samples.size(), 0xFFFFFFFFu));
	if (used == 0) {
		return s;
	}
	s.valid = true;
	// The bracketing pair: the newest sample at or before the render time and the next one.
	uint32_t a = 0;
	while (a + 1 < used && double(p_samples[a + 1].server_tick) <= p_render_ticks) {
		++a;
	}
	const TimedOther &sa = p_samples[a];
	s.index_a = s.index_b = a;
	s.tick_a = s.tick_b = sa.server_tick;
	if (p_render_ticks <= double(sa.server_tick)) {
		s.kind = SampleKind::Hold; // before the oldest sample (held), or exactly at a sample
		s.held = p_render_ticks < double(sa.server_tick);
	} else if (a + 1 < used) {
		s.index_b = a + 1;
		s.tick_b = p_samples[a + 1].server_tick;
		s.kind = SampleKind::Interpolate;
	} else {
		// Past the newest sample: extrapolate along its velocity for at most the limit, then hold.
		const double max_extra = std::max(0.0, p_max_extrapolate_ticks);
		s.extrapolated = true;
		s.held = p_render_ticks - double(sa.server_tick) > max_extra;
		s.kind = s.held ? SampleKind::Hold : SampleKind::Extrapolate;
	}
	return s;
}

OtherPose sample_other(std::span<const TimedOther> p_samples, double p_render_ticks, std::span<const float, sim::kPoseCount> p_pose_eye_heights,
		double p_max_extrapolate_ticks) {
	const SampleSelection sel = select_sample(p_samples, p_render_ticks, p_max_extrapolate_ticks);
	if (!sel.valid) {
		OtherPose empty;
		empty.kind = uint8_t(SampleKind::Hold);
		empty.held = true;
		return empty;
	}
	const double render = p_render_ticks;
	const double max_extra_ticks = std::max(0.0, p_max_extrapolate_ticks);
	const TimedOther &sa = p_samples[sel.index_a];
	OtherPose out;
	if (render <= double(sa.server_tick)) {
		out = present_sample(p_pose_eye_heights, sa.soldier); // before the oldest sample: hold it
	} else if (sel.kind == SampleKind::Interpolate) {
		const TimedOther &sb = p_samples[sel.index_b];
		const double span = double(sb.server_tick - sa.server_tick);
		const float u = float(std::clamp((render - double(sa.server_tick)) / span, 0.0, 1.0));
		const float dt = float(span) * sim::kTickSeconds;
		out = present_sample(p_pose_eye_heights, sa.soldier);
		for (size_t k = 0; k < 3; ++k) {
			out.position[k] = hermite(sa.soldier.position[k], sa.soldier.velocity[k] * dt, sb.soldier.position[k], sb.soldier.velocity[k] * dt, u);
			out.velocity[k] = lerp(sa.soldier.velocity[k], sb.soldier.velocity[k], u);
		}
		const int16_t dyaw = int16_t(uint16_t(sb.soldier.yaw - sa.soldier.yaw)); // shortest arc
		out.yaw = sim::yaw_radians(uint16_t(sa.soldier.yaw + int32_t(std::lround(float(dyaw) * u))));
		out.pitch = lerp(sim::pitch_radians(sa.soldier.pitch), sim::pitch_radians(sb.soldier.pitch), u);
		// The discrete fields switch together at the newer sample (pose, target_pose, transition, mode, flags,
		// generation; m2a.md §6.1).
		set_discrete(out, u >= 1.0f ? sb.soldier : sa.soldier);
		out.eye_height = lerp(eye_of(p_pose_eye_heights, sa.soldier), eye_of(p_pose_eye_heights, sb.soldier), u);
	} else {
		// Past the newest sample: extrapolate along its velocity for at most the limit, then hold.
		const double ahead = std::min(render - double(sa.server_tick), max_extra_ticks);
		const float secs = float(ahead) * sim::kTickSeconds;
		out = present_sample(p_pose_eye_heights, sa.soldier);
		for (size_t k = 0; k < 3; ++k) {
			out.position[k] = sa.soldier.position[k] + sa.soldier.velocity[k] * secs;
		}
	}
	out.extrapolated = sel.extrapolated;
	out.held = sel.held;
	out.tick_a = sel.tick_a;
	out.tick_b = sel.tick_b;
	out.kind = uint8_t(sel.kind);
	return out;
}

int64_t sim_time_q8(sim::weapons::SimTime p_time) {
	// round(p_time x 256 / U) half up, exact in int64 for any time the sim can reach (|t| < 2^54 units).
	const int64_t num = p_time * 256 + sim::weapons::kTimeUnitsPerTick / 2;
	const int64_t q = num / sim::weapons::kTimeUnitsPerTick;
	return (num % sim::weapons::kTimeUnitsPerTick != 0 && num < 0) ? q - 1 : q; // floor division
}

int64_t rewind_time_q8(sim::weapons::SimTime p_segment_start, int32_t p_view_back_q8, uint32_t p_max_rewind_ticks) {
	const int64_t max_lag = int64_t(std::min<uint32_t>(p_max_rewind_ticks, 0x7FFFFFu)) * 256;
	const int64_t lag = std::clamp<int64_t>(view_lag_q8(p_view_back_q8), 0, max_lag);
	return sim_time_q8(p_segment_start) - lag;
}

size_t rebuild_track(std::span<const TimedOther> p_candidates, uint32_t p_view_tick, std::span<TimedOther, kTrackSamples> r_list) {
	if (p_candidates.empty()) {
		return 0;
	}
	// The generation of the newest record in the set (add_others resets the track on a generation change). Ties at
	// the newest tick keep the first (a duplicate tick is one sample).
	size_t newest = 0;
	for (size_t i = 1; i < p_candidates.size(); ++i) {
		if (p_candidates[i].server_tick > p_candidates[newest].server_tick) {
			newest = i;
		}
	}
	const uint32_t newest_tick = p_candidates[newest].server_tick;
	const uint8_t generation = p_candidates[newest].soldier.generation;
	// The track timeout: a newest record more than kTrackTimeoutTicks older than the view's tick means the client had
	// dropped the track (kTrackTimeoutNs without a sample).
	if (p_view_tick > newest_tick && p_view_tick - newest_tick > kTrackTimeoutTicks) {
		return 0;
	}
	// The kTrackSamples newest distinct ticks of that generation, oldest first: an insertion into a bounded sorted list
	// (add_others inserts in tick order, skips a duplicate tick and pops the oldest beyond kTrackSamples).
	size_t n = 0;
	for (const TimedOther &c : p_candidates) {
		if (c.soldier.generation != generation) {
			continue;
		}
		size_t at = 0;
		while (at < n && r_list[at].server_tick < c.server_tick) {
			++at;
		}
		if (at < n && r_list[at].server_tick == c.server_tick) {
			continue; // a duplicate tick
		}
		if (n == kTrackSamples) {
			if (at == 0) {
				continue; // older than all of a full list: it would be popped at once
			}
			// Drop the oldest, shift the ones below the insertion point down.
			for (size_t k = 0; k + 1 < at; ++k) {
				r_list[k] = r_list[k + 1];
			}
			r_list[at - 1] = c;
			continue;
		}
		for (size_t k = n; k > at; --k) {
			r_list[k] = r_list[k - 1];
		}
		r_list[at] = c;
		++n;
	}
	return n;
}

bool same_bits(const OtherPose &p_a, const OtherPose &p_b) {
	return p_a.slot == p_b.slot && p_a.generation == p_b.generation && p_a.pose == p_b.pose && p_a.target_pose == p_b.target_pose &&
			p_a.transition == p_b.transition && p_a.mode == p_b.mode && p_a.flags == p_b.flags && p_a.extrapolated == p_b.extrapolated && p_a.held == p_b.held &&
			same_vec(p_a.position, p_b.position) && same_vec(p_a.velocity, p_b.velocity) && same_float(p_a.yaw, p_b.yaw) && same_float(p_a.pitch, p_b.pitch) &&
			same_float(p_a.eye_height, p_b.eye_height) && p_a.tick_a == p_b.tick_a && p_a.tick_b == p_b.tick_b && p_a.kind == p_b.kind;
}

bool same_bits(const TimedOther &p_a, const TimedOther &p_b) {
	return p_a.server_tick == p_b.server_tick && same_other(p_a.soldier, p_b.soldier);
}

} // namespace bf3::net
