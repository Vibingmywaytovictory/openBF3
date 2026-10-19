// Spawn points, settle, the spawn choice and the deterministic test-course searches (docs/design/m1.md §10, §14;
// the header spawn.h is the contract).
//
// Settle (§10.2): place_spawn finds the floor under the point (a kMovement ray from 1 m above to 2 m below),
// puts the feet on it plus the character padding, lifts in 0.1 m steps up to 0.6 m when the stand shape does not
// fit, then searches rings of 0.1 m up to 0.5 m (8 samples each, closest ring first, each sample on its own floor);
// the caller runs the idle ticks; judge_settle accepts a grounded, non-overlapping soldier whose feet are within
// padding + 1 cm of the floor under it, lifted and shifted within the limits.
#include "bf3/sim/spawn.h"

#include "bf3/core/level/spawn_points.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_motor.h"

#include <Jolt/Jolt.h>

#include <Jolt/Math/Trigonometry.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace bf3::sim {

namespace {

constexpr float kLiftStep = 0.1f;
// Floor normals within this of up count as level for the test courses (docs §14: "normals within 2 degrees").
const float kLevelCos = std::cos(2.0f * kPi / 180.0f);

bool finite3(const Vec3 &p_v) {
	return std::isfinite(p_v[0]) && std::isfinite(p_v[1]) && std::isfinite(p_v[2]);
}

float horizontal_distance(const Vec3 &p_a, const Vec3 &p_b) {
	const float dx = p_a[0] - p_b[0];
	const float dz = p_a[2] - p_b[2];
	return std::sqrt(dx * dx + dz * dz);
}

// The floor under (p_x, p_z), searched from p_top down to p_bottom: the first kMovement surface a ray meets.
bool floor_at(const CollisionWorld &p_world, float p_x, float p_z, float p_top, float p_bottom, StaticHit &r_hit) {
	if (!(p_top > p_bottom)) {
		return false;
	}
	return p_world.ray({ p_x, p_top, p_z }, { 0.0f, p_bottom - p_top, 0.0f }, query_mask::kMovement, &r_hit);
}

// How far the bottom sphere of a soldier standing at p_feet (radius p_radius) can drop straight down before it
// touches kMovement geometry, at most p_depth: a hover gap that bridges floor seams, stands on an edge the centre
// misses, and is ~0 on a slope (where the feet rest r / cos - r above the surface under the centre). The sphere
// starts p_lift above the feet (inside the settled stand shape, so it is free); a start in contact gives -p_lift.
bool drop_below(const CollisionWorld &p_world, const Vec3 &p_feet, float p_radius, float p_lift, float p_depth, float &r_drop) {
	const Vec3 start = { p_feet[0], p_feet[1] + p_lift, p_feet[2] };
	StaticHit h;
	if (!p_world.sweep_capsule(start, p_radius, 2.0f * p_radius, { 0.0f, -(p_lift + p_depth), 0.0f }, query_mask::kMovement, &h)) {
		return false;
	}
	r_drop = h.fraction * (p_lift + p_depth) - p_lift;
	return true;
}

// Course lips (find_clear_heading): a stand capsule this far above the feet, swept along the course, meets any
// step, curb, mat or seam higher than this under its whole footprint (a 4 mm mat on Metro bumps a walker 1 cm up and
// costs 3 % of its speed for a tick: sim.motor).
constexpr float kCourseLip = 0.002f;
// Floor samples along a course: spacing and probe sphere (the motor's hug probe size).
constexpr float kCourseProbeRadius = 0.05f;
constexpr float kCourseRayStep = 0.1f;

} // namespace

std::vector<SpawnPoint> to_spawn_points(const core::level::LevelSpawns &p_spawns) {
	std::vector<SpawnPoint> out;
	out.reserve(p_spawns.points.size());
	for (size_t i = 0; i < p_spawns.points.size(); ++i) {
		const core::level::SpawnPointDef &d = p_spawns.points[i];
		SpawnPoint p;
		p.position = { d.transform.trans[0], d.transform.trans[1], d.transform.trans[2] };
		const float fx = d.transform.forward[0];
		const float fz = d.transform.forward[2];
		// Yaw 0 looks down +Z, a quarter turn down +X (command.h): yaw = atan2(forward.x, forward.z). Not replicated
		// arithmetic (the result is quantised once, here, and sent as the point's yaw).
		p.yaw = (fx * fx + fz * fz) > 1e-12f ? quantize_yaw(std::atan2(fx, fz)) : uint16_t(0);
		p.team = d.team;
		p.priority = std::isfinite(d.priority) ? d.priority : 0.0f;
		p.index = uint32_t(i);
		out.push_back(p);
	}
	return out;
}

SettleResult place_spawn(const SoldierMotor &p_motor, const CollisionWorld &p_world, const SpawnPoint &p_point, uint32_t p_life, Tick p_tick,
		const SettleOptions &p_options) {
	SettleResult r;
	const MotorParams &params = p_motor.params();
	const float pad = params.tuning.character_padding;
	const Vec3 &at = p_point.position;
	r.placed = at;
	r.state = p_motor.spawn_state(at, p_point.yaw, p_life, p_tick);
	if (!finite3(at)) {
		r.failure = "the point is not finite";
		return r;
	}
	// The floor under the point; without one (a crack or seam in the floor right under it) the rings look nearby.
	StaticHit floor;
	const bool has_floor = floor_at(p_world, at[0], at[2], at[1] + p_options.probe_up, at[1] - p_options.probe_down, floor);
	// Straight up from the floor first, then the rings (each sample with the same lifts: a point on a slope next to
	// a wall needs both).
	const uint32_t lifts = uint32_t(std::floor(std::max(p_options.max_lift, 0.0f) / kLiftStep + 1e-3f));
	const auto try_lifts = [&](float p_x, float p_z, float p_floor) {
		for (uint32_t k = 0; k <= lifts; ++k) {
			const Vec3 feet = { p_x, p_floor + pad + float(k) * kLiftStep, p_z };
			if (p_motor.fits_standing(feet)) {
				r.placed = feet;
				r.floor_y = p_floor;
				r.lift = feet[1] - at[1];
				r.shift = horizontal_distance(feet, at);
				r.state = p_motor.spawn_state(feet, p_point.yaw, p_life, p_tick);
				return true;
			}
		}
		return false;
	};
	if (has_floor && try_lifts(at[0], at[2], floor.position[1])) {
		return r;
	}
	bool any_floor = has_floor;
	const uint32_t samples = std::clamp<uint32_t>(p_options.ring_samples, 1u, 64u);
	const uint32_t rings = std::min<uint32_t>(p_options.rings, 64u);
	for (uint32_t ring = 1; ring <= rings; ++ring) {
		const float radius = p_options.ring_step * float(ring);
		for (uint32_t i = 0; i < samples; ++i) {
			const float angle = kTwoPi * float(i) / float(samples);
			const float x = at[0] + radius * JPH::Sin(angle);
			const float z = at[2] + radius * JPH::Cos(angle);
			StaticHit h;
			if (!floor_at(p_world, x, z, at[1] + p_options.probe_up, at[1] - p_options.probe_down, h)) {
				continue;
			}
			any_floor = true;
			if (try_lifts(x, z, h.position[1])) {
				return r;
			}
		}
	}
	if (!any_floor) {
		r.failure = "no floor within " + std::to_string(p_options.probe_up) + " m above to " + std::to_string(p_options.probe_down) + " m below";
		return r;
	}
	r.failure = "no room for a standing soldier within " + std::to_string(p_options.max_lift) + " m up or " +
			std::to_string(p_options.ring_step * float(rings)) + " m sideways";
	return r;
}

void judge_settle(const SoldierMotor &p_motor, const CollisionWorld &p_world, const SpawnPoint &p_point, const SettleOptions &p_options,
		SettleResult &r_result) {
	r_result.ok = false;
	if (!r_result.failure.empty()) {
		return;
	}
	const MotorParams &params = p_motor.params();
	const float pad = params.tuning.character_padding;
	const SoldierState &s = r_result.state;
	r_result.shift = horizontal_distance(s.position, p_point.position);
	r_result.lift = r_result.placed[1] - p_point.position[1];
	// Sinking: the stand shape where the solver keeps it (padding above the feet) overlaps geometry.
	const Vec3 bottom = { s.position[0], s.position[1] + pad, s.position[2] };
	r_result.overlapping = p_world.overlap_capsule(bottom, params.radius, params.poses[size_t(Pose::Stand)].height, query_mask::kMovement,
			params.tuning.pose_change_max_penetration);
	// The gap: how far the soldier could still drop (drop_below; on a slope or an edge a resting soldier's feet are
	// above the surface under its centre, which is not hovering).
	float drop = 0.0f;
	const bool has_floor = drop_below(p_world, s.position, params.radius, 0.01f, p_options.probe_down, drop);
	r_result.gap = has_floor ? drop : 1e9f;
	const bool grounded = (s.flags & soldier_flags::kGrounded) != 0;
	if ((s.flags & soldier_flags::kAlive) == 0) {
		r_result.failure = "died while settling";
	} else if (!grounded) {
		r_result.failure = "not grounded after " + std::to_string(p_options.settle_ticks) + " idle ticks";
	} else if (r_result.overlapping) {
		r_result.failure = "the settled soldier overlaps geometry";
	} else if (!has_floor) {
		r_result.failure = "no floor under the settled soldier";
	} else if (r_result.gap > pad + p_options.hover_tolerance || r_result.gap < -pad - p_options.hover_tolerance) {
		r_result.failure = "the settled soldier can still drop " + std::to_string(r_result.gap) + " m";
	} else if (r_result.lift > p_options.max_lift + 1e-4f) {
		r_result.failure = "lifted " + std::to_string(r_result.lift) + " m";
	} else if (r_result.shift > p_options.ring_step * float(p_options.rings) + 0.05f) {
		r_result.failure = "moved " + std::to_string(r_result.shift) + " m sideways";
	} else {
		r_result.ok = true;
	}
}

void stamp_spawn(const SoldierMotor &p_motor, Tick p_tick, SoldierState &r_state) {
	r_state.tick = p_tick;
	r_state.protect_ticks = p_motor.params().immortal_ticks;
	if (r_state.protect_ticks > 0) {
		r_state.flags |= soldier_flags::kSpawnProtected;
	} else {
		r_state.flags = uint16_t(r_state.flags & ~soldier_flags::kSpawnProtected);
	}
	r_state.events = soldier_events::kSpawned;
	r_state.air_ticks = 0;
	r_state.jump_penalty_ticks = 0;
	r_state.impact_speed = 0.0f;
	r_state.damage = 0.0f;
}

SettleResult settle_spawn(const SoldierMotor &p_motor, const CollisionWorld &p_world, const SpawnPoint &p_point, uint32_t p_life, Tick p_tick,
		const SettleOptions &p_options) {
	SettleResult r = place_spawn(p_motor, p_world, p_point, p_life, p_tick, p_options);
	if (r.failure.empty()) {
		const uint32_t ticks = std::min<uint32_t>(p_options.settle_ticks, 600u * kSimHz);
		for (uint32_t i = 0; i < ticks; ++i) {
			const SoldierState &s = r.state;
			p_motor.step(r.state, idle_command(s.tick + 1u, s.yaw, s.pitch, s.target_pose));
		}
		judge_settle(p_motor, p_world, p_point, p_options, r);
	}
	stamp_spawn(p_motor, p_tick, r.state);
	return r;
}

size_t choose_spawn(const std::vector<SpawnPoint> &p_points, int p_team, const std::vector<Vec3> &p_live_soldiers, float p_protection_radius,
		uint64_t p_seed) {
	const float r2 = std::isfinite(p_protection_radius) ? p_protection_radius * p_protection_radius : 0.0f;
	float best = -INFINITY;
	size_t count = 0;
	// First pass: the highest priority among the eligible points; second: the seeded pick among those.
	auto eligible = [&](const SpawnPoint &p_point) {
		if (p_point.team != 0 && p_point.team != p_team) {
			return false;
		}
		for (const Vec3 &live : p_live_soldiers) {
			const float dx = live[0] - p_point.position[0];
			const float dy = live[1] - p_point.position[1];
			const float dz = live[2] - p_point.position[2];
			if (dx * dx + dy * dy + dz * dz < r2) {
				return false;
			}
		}
		return true;
	};
	for (const SpawnPoint &p : p_points) {
		if (!eligible(p)) {
			continue;
		}
		if (p.priority > best) {
			best = p.priority;
			count = 1;
		} else if (p.priority == best) {
			++count;
		}
	}
	if (count == 0) {
		return SIZE_MAX;
	}
	// splitmix64 of the seed picks one of the `count` best points.
	uint64_t z = p_seed + 0x9E3779B97F4A7C15ull;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	z ^= z >> 31;
	size_t pick = size_t(z % uint64_t(count));
	for (size_t i = 0; i < p_points.size(); ++i) {
		if (p_points[i].priority == best && eligible(p_points[i])) {
			if (pick == 0) {
				return i;
			}
			--pick;
		}
	}
	return SIZE_MAX;
}

std::optional<uint16_t> find_clear_heading(const SoldierMotor &p_motor, const CollisionWorld &p_world, const Vec3 &p_feet, float p_length,
		float p_max_rise, uint32_t p_headings, uint16_t p_first_yaw) {
	if (!finite3(p_feet) || !std::isfinite(p_length) || p_length <= 0.0f || p_length > 1000.0f || p_headings == 0) {
		return std::nullopt;
	}
	const MotorParams &params = p_motor.params();
	const float pad = params.tuning.character_padding;
	const float r = params.radius;
	const float stand = params.poses[size_t(Pose::Stand)].height;
	const float crouch = params.poses[size_t(Pose::Crouch)].height;
	const float tolerance = params.tuning.pose_change_max_penetration;
	const Vec3 bottom = { p_feet[0], p_feet[1] + pad, p_feet[2] };
	// The poses that do not depend on the heading, and the room for the E1b jump (the stand shape swept up by the
	// jump height and 5 cm).
	if (p_world.overlap_capsule(bottom, r, crouch, query_mask::kMovement, tolerance) ||
			p_world.overlap_capsule(bottom, r, 2.0f * r, query_mask::kMovement, tolerance) ||
			p_world.sweep_capsule(bottom, r, stand, { 0.0f, params.jump_height + 0.05f, 0.0f }, query_mask::kMovement)) {
		return std::nullopt;
	}
	const PoseParams &prone = params.poses[size_t(Pose::Prone)];
	const uint32_t headings = std::min<uint32_t>(p_headings, 4096u);
	const uint32_t samples = uint32_t(std::floor(p_length / kCourseRayStep));
	for (uint32_t i = 0; i < headings; ++i) {
		const uint16_t yaw = uint16_t(uint32_t(p_first_yaw) + uint32_t((uint64_t(i) * 65536u) / headings));
		const float a = yaw_radians(yaw);
		const float dx = JPH::Sin(a);
		const float dz = JPH::Cos(a);
		// The prone footprint at this heading: CollisionBoxMin/MaxExpand, from the feet (as the motor checks it).
		if (params.tuning.prone_footprint_check) {
			const float half_x = 0.5f * (prone.box_min_expand[0] + prone.box_max_expand[0]);
			const float half_z = 0.5f * (prone.box_min_expand[2] + prone.box_max_expand[2]);
			const float cx = 0.5f * (prone.box_max_expand[0] - prone.box_min_expand[0]);
			const float cz = 0.5f * (prone.box_max_expand[2] - prone.box_min_expand[2]);
			const float top = params.tuning.prone_footprint_height;
			const float base = 0.1f; // the motor's footprint floor clearance
			if (half_x > 0.0f && half_z > 0.0f && top > base) {
				const Vec3 center = { p_feet[0] + cx * dz + cz * dx, p_feet[1] + 0.5f * (top + base), p_feet[2] - cx * dx + cz * dz };
				if (p_world.overlap_box(center, { half_x, 0.5f * (top - base), half_z }, a, query_mask::kMovement, tolerance)) {
					continue;
				}
			}
		}
		// The stand capsule kCourseLip above the feet: walls, and every step, curb or seam higher than that under
		// its footprint.
		const Vec3 start = { p_feet[0], p_feet[1] + kCourseLip, p_feet[2] };
		if (p_world.sweep_capsule(start, r, stand - kCourseLip, { dx * p_length, 0.0f, dz * p_length }, query_mask::kMovement)) {
			continue;
		}
		// Floor samples on the centre line (a kCourseProbeRadius sphere lowered from half a metre up, so the
		// millimetre cracks between floor meshes, which the motor's hug bridges, do not count as holes): no hole, no
		// drop beyond p_max_rise, level normals.
		bool level = true;
		for (uint32_t k = 1; k <= samples && level; ++k) {
			StaticHit h;
			const float x = p_feet[0] + dx * (float(k) * kCourseRayStep);
			const float z = p_feet[2] + dz * (float(k) * kCourseRayStep);
			const float top = p_feet[1] + 0.5f;
			const float depth = 1.5f;
			// The normal from a ray at the same spot (the sphere may touch a crack's side); a ray that falls through
			// the crack judges nothing.
			StaticHit n;
			const bool ray_hit = floor_at(p_world, x, z, top, top - depth, n);
			if (!p_world.sweep_capsule({ x, top, z }, kCourseProbeRadius, 2.0f * kCourseProbeRadius, { 0.0f, -depth, 0.0f }, query_mask::kMovement, &h) ||
					std::fabs(top - h.fraction * depth - p_feet[1]) > p_max_rise || (ray_hit && n.normal[1] < kLevelCos)) {
				level = false;
			}
		}
		if (level) {
			return yaw;
		}
	}
	return std::nullopt;
}

std::optional<Vec3> find_drop_spot(const SoldierMotor &p_motor, const CollisionWorld &p_world, float p_clearance, float p_grid) {
	if (!std::isfinite(p_clearance) || p_clearance <= 0.0f || p_clearance > 10000.0f || !std::isfinite(p_grid) || p_grid < 0.1f) {
		return std::nullopt;
	}
	const std::shared_ptr<const StaticWorldShapes> &shapes = p_world.shapes();
	if (shapes == nullptr || shapes->tile_count() == 0) {
		return std::nullopt;
	}
	const Vec3 lo = shapes->min();
	const Vec3 hi = shapes->max();
	if (!finite3(lo) || !finite3(hi)) {
		return std::nullopt;
	}
	const MotorParams &params = p_motor.params();
	const float pad = params.tuning.character_padding;
	const float stand = params.poses[size_t(Pose::Stand)].height;
	const float top = hi[1] + 1.0f;
	const float bottom = lo[1] - 1.0f;
	// At most 2^20 grid points (a 5 m grid covers 5 km x 5 km).
	const double cells_x = std::floor(double(hi[0] - lo[0]) / p_grid) + 1.0;
	const double cells_z = std::floor(double(hi[2] - lo[2]) / p_grid) + 1.0;
	if (cells_x * cells_z > double(1u << 20)) {
		return std::nullopt;
	}
	for (uint32_t ix = 0; ix < uint32_t(cells_x); ++ix) {
		const float x = lo[0] + float(ix) * p_grid;
		for (uint32_t iz = 0; iz < uint32_t(cells_z); ++iz) {
			const float z = lo[2] + float(iz) * p_grid;
			StaticHit h;
			if (!floor_at(p_world, x, z, top, bottom, h) || h.normal[1] < kLevelCos) {
				continue;
			}
			// A level 1 m patch: four rays half a metre out land within 1 cm of the centre's height, normals level.
			bool patch = true;
			const float offsets[4][2] = { { 0.5f, 0.0f }, { -0.5f, 0.0f }, { 0.0f, 0.5f }, { 0.0f, -0.5f } };
			for (const auto &o : offsets) {
				StaticHit e;
				if (!floor_at(p_world, x + o[0], z + o[1], h.position[1] + 0.5f, h.position[1] - 0.5f, e) || std::fabs(e.position[1] - h.position[1]) > 0.01f ||
						e.normal[1] < kLevelCos) {
					patch = false;
					break;
				}
			}
			if (!patch) {
				continue;
			}
			const Vec3 feet = { x, h.position[1], z };
			const Vec3 above = { x, h.position[1] + pad + 0.01f, z };
			if (!p_motor.fits_standing({ x, h.position[1] + pad, z }) ||
					p_world.sweep_capsule(above, params.radius, stand, { 0.0f, p_clearance, 0.0f }, query_mask::kMovement)) {
				continue;
			}
			return feet;
		}
	}
	return std::nullopt;
}

// ---- M2b ru (docs/design/m2b.md §3.5): the scored TDM spawn choice ---------------------------------------------------

namespace {

uint64_t spawn_mix(uint64_t p_x) {
	uint64_t z = p_x + 0x9E3779B97F4A7C15ull;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

float spawn_dist2(const Vec3 &p_a, const Vec3 &p_b) {
	const float dx = p_a[0] - p_b[0], dy = p_a[1] - p_b[1], dz = p_a[2] - p_b[2];
	return dx * dx + dy * dy + dz * dz;
}

bool spawn_finite(const Vec3 &p_v) {
	return std::isfinite(p_v[0]) && std::isfinite(p_v[1]) && std::isfinite(p_v[2]);
}

} // namespace

size_t choose_spawn_scored(const SpawnQuery &p_query) {
	struct Candidate {
		size_t index = 0;
		float score = 0; // the nearest enemy's distance, capped at safe_distance
		uint64_t draw = 0;
	};
	const float r = std::isfinite(p_query.protection_radius) ? std::max(0.0f, p_query.protection_radius) : 0.0f;
	const float r2 = r * r;
	const float block = std::isfinite(p_query.friendly_block_radius) ? std::max(0.0f, p_query.friendly_block_radius) : 0.0f;
	const float block2 = block * block;
	const float safe = std::isfinite(p_query.safe_distance) && p_query.safe_distance > 0.0f ? p_query.safe_distance : INFINITY;
	std::vector<Candidate> candidates;
	candidates.reserve(p_query.points.size());
	for (size_t i = 0; i < p_query.points.size(); ++i) {
		const SpawnPoint &p = p_query.points[i];
		// (1) the slot's team or neutral.
		if (p.team != 0 && p.team != int(p_query.team)) {
			continue;
		}
		if (!spawn_finite(p.position)) {
			continue;
		}
		// (2) no enemy within the protection radius; the nearest enemy's distance for (4).
		float nearest2 = INFINITY;
		for (const Vec3 &e : p_query.enemies) {
			if (spawn_finite(e)) {
				nearest2 = std::min(nearest2, spawn_dist2(e, p.position));
			}
		}
		if (nearest2 < r2) {
			continue;
		}
		// (3) not where friendly_block_count teammates died recently.
		if (p_query.friendly_block_count > 0) {
			int deaths = 0;
			for (const FriendlyDeath &d : p_query.recent_deaths) {
				if (d.team == p_query.team && p_query.tick >= d.tick && p_query.tick - d.tick <= p_query.friendly_block_ticks &&
						spawn_dist2(d.position, p.position) <= block2) {
					++deaths;
				}
			}
			if (deaths >= p_query.friendly_block_count) {
				continue;
			}
		}
		Candidate c;
		c.index = i;
		c.score = std::min(std::sqrt(nearest2), safe);
		c.draw = spawn_mix(p_query.seed ^ (uint64_t(i) * 0x9E3779B97F4A7C15ull));
		candidates.push_back(c);
	}
	if (candidates.empty()) {
		return SIZE_MAX;
	}
	// (4) rank by the (capped) nearest-enemy distance, ties by the seeded draw (5).
	std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
		if (a.score != b.score) {
			return a.score > b.score;
		}
		return a.draw != b.draw ? a.draw < b.draw : a.index < b.index;
	});
	if (p_query.world == nullptr || p_query.enemies.empty() || p_query.max_los_rays <= 0) {
		return candidates.front().index;
	}
	// TryToSpawnOutOfSight: the best 4, eye-to-chest rays from the 4 nearest enemies of each (<= max_los_rays in all);
	// the first one no enemy sees wins, else the best.
	constexpr size_t kBest = 4;
	constexpr size_t kEnemies = 4;
	int rays = 0;
	const size_t tested = std::min(kBest, candidates.size());
	for (size_t k = 0; k < tested; ++k) {
		const SpawnPoint &p = p_query.points[candidates[k].index];
		Vec3 chest = p.position;
		chest[1] += std::isfinite(p_query.chest_height) ? p_query.chest_height : 1.3f;
		std::pair<float, size_t> near[kEnemies] = {};
		size_t near_count = 0;
		for (size_t e = 0; e < p_query.enemies.size() && e < p_query.enemy_eyes.size(); ++e) {
			if (!spawn_finite(p_query.enemy_eyes[e]) || !spawn_finite(p_query.enemies[e])) {
				continue;
			}
			const std::pair<float, size_t> entry{ spawn_dist2(p_query.enemies[e], p.position), e };
			if (near_count < kEnemies) {
				near[near_count++] = entry;
				std::sort(near, near + near_count);
			} else if (entry < near[kEnemies - 1]) {
				near[kEnemies - 1] = entry;
				std::sort(near, near + kEnemies);
			}
		}
		bool seen = false;
		for (size_t n = 0; n < near_count && !seen; ++n) {
			if (rays >= p_query.max_los_rays) {
				return candidates.front().index; // the ray budget ran out: the best ranked
			}
			++rays;
			const Vec3 &eye = p_query.enemy_eyes[near[n].second];
			const Vec3 delta{ chest[0] - eye[0], chest[1] - eye[1], chest[2] - eye[2] };
			seen = !p_query.world->ray(eye, delta, query_mask::kProjectile);
		}
		if (!seen) {
			return candidates[k].index;
		}
	}
	return candidates.front().index;
}

} // namespace bf3::sim
