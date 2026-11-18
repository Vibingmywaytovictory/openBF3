#include "bf3/sim_harness/vault_spots.h"

#include "bf3/sim/command.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/vault.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace bf3::sim {

namespace {

constexpr uint32_t kSpotHeadings = 16;
constexpr uint32_t kSpotRingStarts = 8;
constexpr float kSpotRingRadius = 6.0f;
constexpr float kSpotSameFace = 1.0f;
constexpr float kSpotFloorTolerance = 0.1f;
constexpr float kSpotFloorStep = 0.5f;
constexpr float kSpotFaceStep = 0.2f;
constexpr float kSpotTopProbe = 1.8f;
constexpr float kSpotTopInset = 0.05f;
constexpr uint32_t kRunTicks = 600;
constexpr uint32_t kTicksAfterPress = 150;

Vec3 heading(uint16_t p_yaw) {
	const float a = yaw_radians(p_yaw);
	return { std::sin(a), 0.0f, std::cos(a) };
}

Vec3 ahead(const Vec3 &p_feet, const Vec3 &p_dir, float p_distance, float p_up) {
	return { p_feet[0] + p_dir[0] * p_distance, p_feet[1] + p_up, p_feet[2] + p_dir[2] * p_distance };
}

Vec3 beside(const Vec3 &p_feet, const Vec3 &p_dir, float p_side) {
	return { p_feet[0] - p_dir[2] * p_side, p_feet[1], p_feet[2] + p_dir[0] * p_side };
}

std::optional<StaticHit> forward_hit(
		const CollisionWorld &p_world, const Vec3 &p_from, const Vec3 &p_dir, float p_up, float p_reach) {
	StaticHit hit;
	if (!p_world.ray(ahead(p_from, p_dir, 0.0f, p_up), { p_dir[0] * p_reach, 0.0f, p_dir[2] * p_reach },
				query_mask::kMovement, &hit)) {
		return std::nullopt;
	}
	hit.fraction *= p_reach;
	return hit;
}

bool flat_face(const StaticHit &p_hit, const Vec3 &p_dir, float p_face) {
	const float facing = -(p_hit.normal[0] * p_dir[0] + p_hit.normal[2] * p_dir[2]);
	return p_hit.terrain == kNoStaticTerrain && std::fabs(p_hit.fraction - p_face) <= kVaultSpotFaceSpread &&
			facing >= kVaultSpotFacing;
}

std::optional<VaultSpot> spot_ahead(const CollisionWorld &p_world, const Vec3 &p_feet, uint16_t p_yaw) {
	const Vec3 dir = heading(p_yaw);
	const std::optional<StaticHit> low = forward_hit(p_world, p_feet, dir, kVaultSpotLowest, kVaultSpotReach);
	if (!low || low->fraction < kVaultSpotRunUp) {
		return std::nullopt;
	}
	const float face = low->fraction;
	StaticHit top;
	if (!p_world.ray(ahead(p_feet, dir, face + kSpotTopInset, kSpotTopProbe), { 0.0f, -kSpotTopProbe, 0.0f },
				query_mask::kMovement, &top) ||
			top.normal[1] < kVaultSpotTopUp) {
		return std::nullopt;
	}
	const float height = top.position[1] - p_feet[1];
	if (height < kVaultSpotLowest || height > kVaultSpotHighest) {
		return std::nullopt;
	}
	for (const float side : { 0.0f, vault_rules::kSideOffset, -vault_rules::kSideOffset }) {
		const Vec3 from = beside(p_feet, dir, side);
		for (float up = vault_rules::kMiddleHeight + vault_rules::kLowOffset; up <= height - kSpotTopInset;
				up += kSpotFaceStep) {
			const std::optional<StaticHit> h = forward_hit(p_world, from, dir, up, kVaultSpotReach);
			if (!h || !flat_face(*h, dir, face)) {
				return std::nullopt;
			}
		}
		for (const float up : { vault_rules::kMiddleHeight + vault_rules::kHighOffset, height + kVaultSpotLowest }) {
			if (forward_hit(p_world, from, dir, std::max(up, height + kSpotTopInset), face + kVaultSpotClearBeyond)) {
				return std::nullopt;
			}
		}
	}
	for (float d = kSpotFloorStep; d < face - kSpotFloorStep; d += kSpotFloorStep) {
		StaticHit floor;
		if (!p_world.ray(ahead(p_feet, dir, d, kSpotFloorStep), { 0.0f, -2.0f * kSpotFloorStep, 0.0f },
					query_mask::kMovement, &floor) ||
				std::fabs(floor.position[1] - p_feet[1]) > kSpotFloorTolerance) {
			return std::nullopt;
		}
	}
	return VaultSpot{ 0, p_feet, p_yaw, face, height };
}

bool same_face(const VaultSpot &p_a, const VaultSpot &p_b) {
	const Vec3 a = ahead(p_a.feet, heading(p_a.yaw), p_a.face, 0.0f);
	const Vec3 b = ahead(p_b.feet, heading(p_b.yaw), p_b.face, 0.0f);
	return p_a.yaw == p_b.yaw && std::fabs(a[0] - b[0]) + std::fabs(a[2] - b[2]) < kSpotSameFace;
}

}

std::vector<VaultSpot> find_vault_spots(const SoldierMotor &p_motor, const CollisionWorld &p_world,
		std::span<const SpawnPoint> p_points, size_t p_limit) {
	std::vector<VaultSpot> out;
	for (const SpawnPoint &p : p_points) {
		if (out.size() >= p_limit) {
			break;
		}
		size_t here = 0;
		for (uint32_t ring = 0; ring <= kSpotRingStarts && here < kVaultSpotsPerPoint; ++ring) {
			SpawnPoint start = p;
			if (ring > 0) {
				const Vec3 out_dir = heading(uint16_t((ring - 1) * (65536u / kSpotRingStarts)));
				start.position = ahead(p.position, out_dir, kSpotRingRadius, 0.0f);
			}
			const SettleResult settled = settle_spawn(p_motor, p_world, start, 1, 1000);
			if (!settled.ok) {
				continue;
			}
			for (uint32_t k = 0; k < kSpotHeadings && out.size() < p_limit && here < kVaultSpotsPerPoint; ++k) {
				std::optional<VaultSpot> spot =
						spot_ahead(p_world, settled.state.position, uint16_t(k * (65536u / kSpotHeadings)));
				if (spot && std::none_of(out.begin(), out.end(), [&](const VaultSpot &p_seen) {
						return same_face(p_seen, *spot);
					})) {
					spot->point = p.index;
					out.push_back(*spot);
					++here;
				}
			}
		}
	}
	return out;
}

float vault_spot_along(const VaultSpot &p_spot, const Vec3 &p_at) {
	const Vec3 dir = heading(p_spot.yaw);
	return (p_at[0] - p_spot.feet[0]) * dir[0] + (p_at[2] - p_spot.feet[2]) * dir[2];
}

bool vault_spot_crossed(const VaultSpot &p_spot, const Vec3 &p_at) {
	return vault_spot_along(p_spot, p_at) > p_spot.face;
}

namespace {

VaultSpotPress press_of(const CollisionWorld &p_world, const VaultSpot &p_spot, const SoldierState &p_state) {
	VaultSpotPress p;
	p.grounded = (p_state.flags & soldier_flags::kGrounded) != 0;
	p.mode = uint8_t(p_state.mode);
	p.pose = uint8_t(p_state.pose);
	p.transition = p_state.transition_ticks;
	p.speed = std::sqrt(p_state.velocity[0] * p_state.velocity[0] + p_state.velocity[2] * p_state.velocity[2]);
	p.reach = vault_probe_reach(p.speed);
	p.face = p_spot.face - vault_spot_along(p_spot, p_state.position);
	const Vec3 dir = heading(p_state.yaw);
	using namespace vault_rules;
	const float ups[] = { kMiddleHeight + kHighOffset, kMiddleHeight + kMiddleOffset, kMiddleHeight + kLowOffset,
		kMiddleHeight + kMiddleOffset, kMiddleHeight + kMiddleOffset };
	const float sides[] = { 0.0f, 0.0f, 0.0f, kSideOffset, -kSideOffset };
	for (size_t i = 0; i < p.rays.size(); ++i) {
		const std::optional<float> h = vault_face_distance(p_world,
				ahead(beside(p_state.position, dir, sides[i]), dir, 0.0f, ups[i]), dir, query_mask::kMovement, p.reach);
		p.rays[i] = h ? *h : kVaultSpotRayMiss;
	}
	const float d = p.rays[1] >= 0.0f ? p.rays[1] : p.rays[2];
	if (d >= 0.0f) {
		p.front = vault_height_at(p_world, p_state.position, dir, d + kFrontEpsilon, query_mask::kMovement);
		p.further = vault_height_at(p_world, p_state.position, dir, d + kUpLength, query_mask::kMovement);
	}
	return p;
}

}

VaultSpotRun run_at_vault_spot(const SoldierMotor &p_motor, const CollisionWorld &p_world, const VaultSpot &p_spot,
		float p_press_at, bool p_sprint) {
	SoldierState s = p_motor.spawn_state(p_spot.feet, p_spot.yaw, 1, 1000, false);
	for (uint32_t i = 0; i < 30; ++i) {
		p_motor.step(s, idle_command(s.tick + 1, p_spot.yaw, 0, Pose::Stand));
	}
	VaultSpotRun r;
	uint32_t after = 0;
	for (uint32_t i = 0; i < kRunTicks && after < kTicksAfterPress && !r.ended; ++i) {
		Command c = idle_command(s.tick + 1, p_spot.yaw, 0, Pose::Stand);
		c.sequence = c.tick;
		c.move_forward = kAxisMax;
		c.buttons = p_sprint ? buttons::kSprint : 0u;
		if (!r.pressed && p_spot.face - vault_spot_along(p_spot, s.position) <= p_press_at) {
			c.buttons |= buttons::kJump;
			r.pressed = true;
			r.press = press_of(p_world, p_spot, s);
		}
		p_motor.step(s, c);
		if ((s.events & soldier_events::kVaulted) != 0) {
			r.vaulted = true;
			r.kind = s.vault.kind;
			r.variant = s.vault.variant;
			r.distance = s.vault.distance;
			r.height = s.vault.height;
			r.length = s.vault.length;
		}
		if (r.vaulted) {
			++r.ticks;
			r.highest = std::max(r.highest, s.position[1] - p_spot.feet[1]);
			if ((s.events & soldier_events::kVaultEnded) != 0) {
				r.ended = true;
				r.stopped = s.velocity == Vec3{ 0.0f, 0.0f, 0.0f };
			}
		}
		after += r.pressed ? 1 : 0;
	}
	r.crossed = vault_spot_crossed(p_spot, s.position);
	return r;
}

}
