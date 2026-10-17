// Stationary target bots and the sky lane (docs/design/m2a.md §1 setup, §10.1; owner sv; target_bots.h).
#include "bf3/host/target_bots.h"

#include "bf3/sim/static_world.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace bf3::host {

TargetBotSource::TargetBotSource(const TargetBotConfig &p_config) :
		config_(p_config) {}

sim::Command TargetBotSource::command(sim::Tick p_tick, const sim::SoldierState &p_state) {
	(void)p_state;
	// Idle at the configured facing and pose (no weapon input): a bot fires at the present (quality None, D7).
	return sim::idle_command(p_tick, config_.yaw, config_.pitch, config_.pose);
}

void TargetBotSource::set(const TargetBotConfig &p_config) {
	config_ = p_config;
}

core::Result<SkyLane> find_sky_lane(const sim::CollisionWorld &p_world, float p_flight_m, float p_margin_m) {
	if (p_world.shapes() == nullptr) {
		return core::make_error("find_sky_lane: no static world");
	}
	if (!std::isfinite(p_flight_m) || !std::isfinite(p_margin_m) || p_flight_m <= 0.0f) {
		return core::make_error("find_sky_lane: bad flight length");
	}
	const sim::Vec3 lo = p_world.shapes()->min();
	const sim::Vec3 hi = p_world.shapes()->max();
	for (size_t k = 0; k < 3; ++k) {
		if (!std::isfinite(lo[k]) || !std::isfinite(hi[k]) || hi[k] < lo[k]) {
			return core::make_error("find_sky_lane: the static world has no bounds");
		}
	}
	// The longest horizontal axis (x = 0 or z = 2), the lane along it; the cross axis carries the lateral offsets.
	const size_t axis = (hi[2] - lo[2]) >= (hi[0] - lo[0]) ? 2 : 0;
	const size_t cross = axis == 2 ? 0 : 2;
	const float start = lo[axis] + kSkyLaneStart;
	const float in_bounds = hi[axis] + p_margin_m - start;
	const float need = p_flight_m + kSkyLaneSpare;
	const float height = hi[1] + kSkyLaneHeight;
	const float centre = 0.5f * (lo[cross] + hi[cross]);
	const float half = 0.5f * (hi[cross] - lo[cross]);
	sim::Vec3 dir{};
	dir[axis] = kSkyLaneClearRay;
	float best_clear = 0;
	for (int i = 0;; ++i) {
		// 0, +step, -step, +2 step, ... across the bounds.
		const int k = (i + 1) / 2;
		const float offset = float(k) * kSkyLaneStep * ((i % 2) == 1 ? 1.0f : -1.0f);
		if (float(k) * kSkyLaneStep > half) {
			break;
		}
		sim::Vec3 eye{};
		eye[axis] = start;
		eye[cross] = centre + (i == 0 ? 0.0f : offset);
		eye[1] = height + kSkyLaneEyeHeight;
		sim::StaticHit hit;
		const bool blocked = p_world.ray(eye, dir, sim::query_mask::kProjectile, &hit);
		const float clear = blocked ? hit.fraction * kSkyLaneClearRay : kSkyLaneClearRay;
		best_clear = std::max(best_clear, clear);
		if (blocked) {
			continue;
		}
		SkyLane lane;
		lane.eye = eye;
		lane.feet = eye;
		lane.feet[1] = height;
		lane.yaw = axis == 2 ? uint16_t(0) : uint16_t(16384); // +Z: yaw 0; +X: a quarter turn (command.h)
		lane.clear_length = clear;
		lane.in_bounds_length = in_bounds;
		if (in_bounds < need) {
			char text[256];
			std::snprintf(text, sizeof(text), "find_sky_lane: the clear lane holds %.1f m in bounds (+ %.0f m margin), the flight needs %.1f m (+ %.0f m spare)",
					double(in_bounds), double(p_margin_m), double(p_flight_m), double(kSkyLaneSpare));
			return core::make_error(text);
		}
		return lane;
	}
	char text[256];
	std::snprintf(text, sizeof(text), "find_sky_lane: no lateral offset gives a clear %.0f m ray (longest %.1f m); in-bounds length %.1f m, needed %.1f m",
			double(kSkyLaneClearRay), double(best_clear), double(in_bounds), double(need));
	return core::make_error(text);
}

} // namespace bf3::host
