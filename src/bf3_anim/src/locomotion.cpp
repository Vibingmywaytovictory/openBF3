// Run blend for the 3p crowd (S3; docs/design/s3.md §5.3). See locomotion.h.
#include "bf3/anim/locomotion.h"

#include <algorithm>
#include <cmath>

namespace bf3::anim {

namespace {

constexpr float kTwoPi = 6.28318530718f;

// Angle of a horizontal direction about +Y, measured from +Z towards +X, in [0, 2 pi).
float heading_of(float p_x, float p_z) {
	float a = std::atan2(p_x, p_z);
	if (a < 0.0f) {
		a += kTwoPi;
	}
	return a >= kTwoPi ? 0.0f : a;
}

} // namespace

LocoWeights locomotion_weights(const std::array<LocoClipInfo, kLocoClips> &p_clips, const Vec3 &p_local, const LocoParams &p_params) {
	LocoWeights out;
	float x = p_local.x, z = p_local.z;
	if (!std::isfinite(x) || !std::isfinite(z)) {
		x = z = 0.0f; // a non-finite velocity is idle
	}
	const float speed = std::sqrt(x * x + z * z);
	// Idle by speed: 1 at or below idle_speed, 0 at or above full_speed, linear between.
	float idle = 1.0f;
	if (!(speed <= p_params.idle_speed)) {
		const float span = p_params.full_speed - p_params.idle_speed;
		idle = span > 0.0f ? std::clamp(1.0f - (speed - p_params.idle_speed) / span, 0.0f, 1.0f) : 0.0f;
	}
	// The direction cycles, ordered by the heading of their measured root direction.
	struct Cycle {
		size_t clip;
		float heading;
	};
	std::array<Cycle, kLocoClips> cycles{};
	size_t n = 0;
	for (size_t c = size_t(LocoClip::Fwd); c < kLocoClips; ++c) {
		const Vec3 &d = p_clips[c].root_dir;
		if (p_clips[c].root_speed > 0.0f && std::isfinite(d.x) && std::isfinite(d.z) && (d.x * d.x + d.z * d.z) > 1.0e-6f) {
			cycles[n++] = Cycle{ c, heading_of(d.x, d.z) };
		}
	}
	if (n == 0 || idle >= 1.0f) {
		out.weight[size_t(LocoClip::Idle)] = 1.0f;
		return out; // cycle_seconds 0: the phase holds
	}
	std::sort(cycles.begin(), cycles.begin() + n, [](const Cycle &a, const Cycle &b) { return a.heading < b.heading; });
	const float h = heading_of(x, z);
	// The bracket [cycles[k], cycles[k + 1]] (cyclic) that contains h.
	size_t lo = n - 1, hi = 0;
	for (size_t k = 0; k < n; ++k) {
		if (cycles[k].heading > h) {
			hi = k;
			lo = (k + n - 1) % n;
			break;
		}
		if (k == n - 1) {
			lo = n - 1;
			hi = 0;
		}
	}
	float span = cycles[hi].heading - cycles[lo].heading;
	float into = h - cycles[lo].heading;
	if (span <= 0.0f) {
		span += kTwoPi;
	}
	if (into < 0.0f) {
		into += kTwoPi;
	}
	float t = n == 1 || span <= 0.0f ? 0.0f : std::clamp(into / span, 0.0f, 1.0f);
	// Within kSnap of a cycle's own direction the neighbour is not sampled at all (a weight of 1-2 % is invisible but
	// would cost a whole clip sample and count as a two-cycle blend).
	constexpr float kSnap = 0.02f;
	t = t < kSnap ? 0.0f : (t > 1.0f - kSnap ? 1.0f : t);
	const float moving = 1.0f - idle;
	out.weight[size_t(LocoClip::Idle)] = idle;
	if (t < 1.0f) {
		out.weight[cycles[lo].clip] += moving * (1.0f - t);
	}
	if (n > 1 && t > 0.0f) {
		out.weight[cycles[hi].clip] += moving * t;
	}
	// Playback rate from the blended root speed of the weighted cycles.
	float cycle_w = 0.0f, root_speed = 0.0f, duration = 0.0f;
	for (size_t c = size_t(LocoClip::Fwd); c < kLocoClips; ++c) {
		const float w = out.weight[c];
		if (w > 0.0f) {
			cycle_w += w;
			root_speed += w * p_clips[c].root_speed;
			duration += w * p_clips[c].duration;
		}
	}
	if (cycle_w > 0.0f) {
		root_speed /= cycle_w;
		duration /= cycle_w;
		float rate = root_speed > 0.0f ? speed / root_speed : 1.0f;
		rate = std::clamp(rate, p_params.min_rate, p_params.max_rate);
		out.cycle_seconds = rate > 0.0f && duration > 0.0f ? duration / rate : 0.0f;
	}
	return out;
}

} // namespace bf3::anim
