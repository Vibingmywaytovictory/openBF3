#include "bf3/sim_harness/water_spots.h"

#include "bf3/sim/command.h"
#include "bf3/sim/static_world.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace bf3::sim {

namespace {

constexpr float kBedProbeAbove = 100.0f;
constexpr float kBedProbeDrop = 400.0f;
constexpr uint32_t kBankHeadings = 16;
constexpr uint32_t kSettleTicks = 30;
constexpr uint32_t kRunTicks = 1200;
constexpr uint32_t kStillTicks = 90;

struct Candidate {
	Vec3 deep{};
	float surface = 0.0f;
	float spawn_distance = 0.0f;
};

Command walk(Tick p_tick, uint16_t p_yaw) {
	Command c = idle_command(p_tick, p_yaw, 0, Pose::Stand);
	c.move_forward = quantize_axis(1.0f);
	return c;
}

}

std::optional<float> bed_below(const CollisionWorld &p_world, float p_x, float p_z, float p_from) {
	StaticHit hit;
	if (!p_world.ground_below({ p_x, p_from + kBedProbeAbove, p_z }, kBedProbeDrop, query_mask::kMovement, &hit)) {
		return std::nullopt;
	}
	return hit.position[1];
}

std::optional<WaterSpot> find_water_spot(const SoldierMotor &p_motor, const CollisionWorld &p_world,
		const WaterSurface &p_water, std::span<const SpawnPoint> p_points) {
	std::vector<Candidate> candidates;
	for (size_t b = 0; b < p_water.body_count(); ++b) {
		const Vec3 lo = p_water.body_min(b), hi = p_water.body_max(b);
		const size_t columns = size_t(std::floor((hi[0] - lo[0]) / kWaterSpotGrid)) + 1;
		const size_t rows = size_t(std::floor((hi[2] - lo[2]) / kWaterSpotGrid)) + 1;
		for (size_t iz = 0; iz < rows; ++iz) {
			for (size_t ix = 0; ix < columns; ++ix) {
				const float x = lo[0] + (float(ix) + 0.5f) * kWaterSpotGrid;
				const float z = lo[2] + (float(iz) + 0.5f) * kWaterSpotGrid;
				const std::optional<float> surface = p_water.height(x, z);
				if (!surface) {
					continue;
				}
				const std::optional<float> bed = bed_below(p_world, x, z, *surface);
				if (!bed || *surface - *bed < kWaterSpotDepth) {
					continue;
				}
				Candidate c;
				c.deep = { x, *bed, z };
				c.surface = *surface;
				std::map<int, float> nearest;
				for (const SpawnPoint &p : p_points) {
					const float d = std::hypot(p.position[0] - x, p.position[2] - z);
					const auto [at, added] = nearest.emplace(p.team, d);
					at->second = added ? d : std::min(at->second, d);
				}
				c.spawn_distance = 0.0f;
				for (const auto &[team, d] : nearest) {
					c.spawn_distance = std::max(c.spawn_distance, d);
				}
				candidates.push_back(c);
			}
		}
	}
	std::stable_sort(candidates.begin(), candidates.end(),
			[](const Candidate &p_a, const Candidate &p_b) { return p_a.spawn_distance < p_b.spawn_distance; });
	const float climb = std::tan(p_motor.params().max_ascend_degrees * core::kDegToRadF) * kWaterSpotBankStep;
	for (size_t i = 0; i < std::min(candidates.size(), kWaterSpotCandidates); ++i) {
		const Candidate &c = candidates[i];
		std::optional<WaterSpot> best;
		for (uint32_t k = 0; k < kBankHeadings; ++k) {
			const float a = float(k) * (kTwoPi / float(kBankHeadings));
			const float dx = std::sin(a), dz = std::cos(a);
			float previous = c.deep[1];
			for (float r = kWaterSpotBankStep; r <= kWaterSpotBankReach; r += kWaterSpotBankStep) {
				const float x = c.deep[0] + dx * r, z = c.deep[2] + dz * r;
				const std::optional<float> ground = bed_below(p_world, x, z, c.surface);
				if (!ground || std::fabs(*ground - previous) > climb) {
					break;
				}
				previous = *ground;
				if (*ground >= c.surface + kWaterSpotBankRise) {
					const Vec3 bank{ x, *ground, z };
					if (p_motor.fits_standing(bank) && (!best || r < best->run)) {
						WaterSpot s;
						s.bank = bank;
						s.deep = c.deep;
						s.yaw = quantize_yaw(a + kPi);
						s.surface = c.surface;
						s.run = r;
						best = s;
					}
					break;
				}
			}
		}
		if (best) {
			return best;
		}
	}
	return std::nullopt;
}

float water_spot_along(const WaterSpot &p_spot, const Vec3 &p_at) {
	const float a = yaw_radians(p_spot.yaw);
	return (p_at[0] - p_spot.bank[0]) * std::sin(a) + (p_at[2] - p_spot.bank[2]) * std::cos(a);
}

WaterSpotRun run_at_water_spot(const SoldierMotor &p_motor, const CollisionWorld &p_world, const WaterSurface &p_water,
		const WaterSpot &p_spot) {
	WaterSpotRun out;
	SoldierState s = p_motor.spawn_state(p_spot.bank, p_spot.yaw, 1, 1000, false);
	for (uint32_t i = 0; i < kSettleTicks; ++i) {
		p_motor.step(s, idle_command(s.tick + 1, p_spot.yaw, 0, Pose::Stand));
	}
	for (uint32_t i = 0; i < kRunTicks; ++i) {
		p_motor.step(s, walk(s.tick + 1, p_spot.yaw));
		if (s.mode == MoveMode::Swimming) {
			out.first_swim_tick = out.swim_ticks == 0 ? i + 1 : out.first_swim_tick;
			++out.swim_ticks;
		}
		if (water_spot_along(p_spot, s.position) >= p_spot.run) {
			out.reached = true;
			break;
		}
	}
	for (uint32_t i = 0; i < kStillTicks; ++i) {
		p_motor.step(s, idle_command(s.tick + 1, p_spot.yaw, 0, Pose::Stand));
	}
	out.in = s;
	out.surface = p_water.height(s.position[0], s.position[2]).value_or(NAN);
	out.bed = bed_below(p_world, s.position[0], s.position[2], out.surface).value_or(NAN);
	const uint16_t back = uint16_t(p_spot.yaw + 0x8000u);
	for (uint32_t i = 0; i < kRunTicks; ++i) {
		p_motor.step(s, walk(s.tick + 1, back));
		if (s.mode == MoveMode::OnGround && water_spot_along(p_spot, s.position) <= 0.0f) {
			out.left = true;
			break;
		}
	}
	out.out = s;
	return out;
}

}
