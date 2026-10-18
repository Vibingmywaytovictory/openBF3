// The firefight stub bots (firefight_brain.h; docs/design/m2a.md §10.2, §14.1, E13, E15; as built:
// docs/design/m2a-bt.md). Server thread only, serial like S1's brains.
//
// One FirefightShared per GameServer holds what every brain of that server reads: the server (view(), gunplay(),
// place_soldier), the config, the soldier's eye heights and capsule, the nav hook (bot_fill.h BotNav: the S1
// NavQuery / PathService the walkers move through) and the Pairs lanes. A brain is a small state machine per life:
//   life     a new SoldierState::life starts the life's random stream (splitmix64 of the brain seed and the life),
//            resets the target, the fire and the strafe state; Pairs places the soldier at its lane end (a live
//            soldier: life and weapons kept, protection cleared) once the RespawnPolicy has respawned it;
//   perceive every retarget_ticks (staggered by slot) or at once when hit without a target: the nearest enemy (team
//            1 / 2, not ours, alive; peers included, so a human is a target like any bot) with a clear eye-to-chest
//            ray (query_mask::kProjectile), at most kMaxRaysPerThink rays nearest first; the current target is
//            re-checked every kTargetCheckTicks and dropped after kLoseTicks unseen;
//   aim      the direction to the target's chest plus a seeded error (sigma from aim_sigma_start_deg to
//            aim_sigma_end_deg over aim_sigma_decay_s on target), the view turned at most kTurnRateDegS; the
//            weapon's own spread and recoil are the sim's (the command carries only the view);
//   fire     automatic weapons in bursts (burst_min_rounds .. burst_max_rounds, burst_pause_* between), switched
//            to their automatic mode first through the fire-mode counter; others tapped at semi_rate_hz; fire only
//            when the view is within kFireConeDeg of the aim; reload at an empty magazine, or without a target
//            below reload_fraction; the sidearm when the primary has no round left at all;
//   move     Free with walk: S1's WalkerBrain (bot_fill.h BotNav) with the nearest enemy as its goal set (hunting),
//            its move re-expressed in the brain's own view yaw, no sprint while engaged; engaged within
//            kEngageStopMetres, and Pairs while engaged: strafe legs of kStrafeHalfMetres either side of where
//            the leg started, reversed at a missing floor;
//   blind    Free without a target for no_target_s: one burst along the longest clear of 8 headings.
// Everything random comes from the brain's seed (firefight_seed(match_seed, slot)) and the life, and the server runs
// the brains in slot order, so a firefight reproduces bit for bit (E15).

#include "bf3/host/firefight_brain.h"

#include "bf3/host/bot_fill.h"
#include "bf3/host/level_load.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/nav.h"
#include "bf3/sim/nav_query.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/weapons/shooting_range.h"
#include "bf3/sim/weapons/weapon_input.h"
#include "bf3/sim/weapons/weapon_rng.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

namespace bf3::host {

namespace {

namespace sw = sim::weapons;

// bt's named constants (docs/design/m2a-bt.md "Constants"); the §10.2 numbers are FirefightConfig's.
constexpr float kTurnRateDegS = 540.0f; // view turn limit (S1's WalkerParams::yaw_rate_deg_s)
constexpr float kPitchTurnRateDegS = 360.0f;
constexpr float kMaxPitchDeg = 80.0f;
constexpr float kFireConeDeg = 4.0f; // fire only when the view is this close to the aim (error included)
constexpr uint32_t kTargetCheckTicks = 10; // the current target's ray
constexpr uint32_t kLoseTicks = 60; // unseen this long: the target is dropped
constexpr uint32_t kMaxRaysPerThink = 6; // perception rays per think, nearest first
constexpr float kMaxEngageMetres = 150.0f;
constexpr float kKeepTargetRatio = 1.25f; // a visible current target is kept unless another is this much nearer
constexpr uint32_t kErrorTicks = 6; // a new aim error draw
constexpr float kErrorPitchShare = 0.6f; // the pitch error's sigma against the yaw error's
constexpr float kChestShare = 0.8f; // chest = feet + this x the pose's eye height
constexpr uint32_t kModeDelayTicks = 3; // after a spawn, before the fire-mode counter moves (§5.2's spawn rule)
constexpr uint32_t kModeWaitTicks = 10;
constexpr float kEngageStopMetres = 25.0f; // Free: closer than this the bot stops walking and strafes
constexpr float kStrafeHalfMetres = 2.0f;
constexpr float kStrafeAxis = 0.8f; // stick share of a strafe leg
constexpr uint32_t kStrafeMinTicks = 60, kStrafeMaxTicks = 150;
constexpr uint32_t kStrafeGroundTicks = 6; // the floor-ahead ray
constexpr float kBlindRayMetres = 60.0f;
constexpr uint32_t kBlindTicks = 60; // one blind burst
constexpr float kHuntShare = 0.8f; // Free: walker goals that are the hunt goal (else S1's random nav points)
constexpr float kHuntArriveMetres = 3.0f;
constexpr float kSprintConeDeg = 20.0f;
// An enemy whose life began less than this ago is tracked but not fired at (place_soldier clears the spawn protection
// a Pairs bot would otherwise have; measured before: a recon bot on a lane died 78 times in 120 s without one shot).
constexpr uint32_t kFreshTargetTicks = 60;

// Layout (firefight_layout).
constexpr float kLayoutProbeUp = 1.5f; // rays down start this far above the observer's floor
constexpr float kLayoutProbeDrop = 4.5f;
constexpr float kLayoutFloorTolerance = 3.0f; // an end's floor within this of the observer's
constexpr float kLayoutMinNormalY = 0.64f; // cos 50 deg (MaxAscendAngle)
constexpr float kLayoutCapsuleTolerance = 0.02f;
constexpr float kLayoutNavExtent = 1.0f;
constexpr float kRangeLaneMinMetres = 20.0f; // the default observer: Metro's range lane (find_range_lane)
constexpr float kSharedLaneOffset = 1.2f; // pairs on a reused lane move this far along its normal per reuse
constexpr uint32_t kSharedLaneTries = 6; // offsets tried per reuse (+-1, +-2, +-3 steps); none holds: the lane as is

constexpr float kDeg = sim::kPi / 180.0f;

uint64_t mix(uint64_t p_x) {
	return sw::splitmix64(p_x);
}

uint64_t next(uint64_t &r_state) {
	r_state = mix(r_state);
	return r_state;
}

float unit(uint64_t &r_state) {
	return float(next(r_state) >> 40) / float(1ull << 24);
}

float uniform(uint64_t &r_state, float p_min, float p_max) {
	return p_min + (p_max - p_min) * unit(r_state);
}

uint32_t uniform_u(uint64_t &r_state, uint32_t p_min, uint32_t p_max) {
	if (p_max <= p_min) {
		return p_min;
	}
	return p_min + uint32_t(next(r_state) % uint64_t(p_max - p_min + 1));
}

// Box-Muller on two of the stream's draws (deterministic on one build).
float gauss(uint64_t &r_state) {
	const float u1 = std::max(unit(r_state), 1e-7f);
	const float u2 = unit(r_state);
	return std::sqrt(-2.0f * std::log(u1)) * std::cos(sim::kTwoPi * u2);
}

float wrap(float p_radians) {
	float a = std::fmod(p_radians, sim::kTwoPi);
	if (a > sim::kPi) {
		a -= sim::kTwoPi;
	} else if (a < -sim::kPi) {
		a += sim::kTwoPi;
	}
	return a;
}

float yaw_of(float p_dx, float p_dz) {
	return std::atan2(p_dx, p_dz); // yaw 0 looks down +Z, a quarter turn down +X (command.h)
}

float horizontal(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	const float dx = p_b[0] - p_a[0], dz = p_b[2] - p_a[2];
	return std::sqrt(dx * dx + dz * dz);
}

float distance(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	const float dx = p_b[0] - p_a[0], dy = p_b[1] - p_a[1], dz = p_b[2] - p_a[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

sim::Vec3 add(const sim::Vec3 &p_a, float p_x, float p_y, float p_z) {
	return { p_a[0] + p_x, p_a[1] + p_y, p_a[2] + p_z };
}

sim::Vec3 sub(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	return { p_a[0] - p_b[0], p_a[1] - p_b[1], p_a[2] - p_b[2] };
}

bool is_alive(const sim::SoldierState &p_state) {
	return (p_state.flags & sim::soldier_flags::kAlive) != 0 && p_state.mode != sim::MoveMode::Dead;
}

bool clear_ray(const sim::CollisionWorld &p_world, const sim::Vec3 &p_from, const sim::Vec3 &p_to) {
	return !p_world.ray(p_from, sub(p_to, p_from), sim::query_mask::kProjectile);
}

} // namespace

struct FirefightShared {
	GameServer *server = nullptr;
	std::shared_ptr<const FirefightConfig> config;
	std::array<float, sim::kPoseCount> eye{ 1.6f, 1.0f, 0.3f }; // MotorParams::poses[].eye_height when a level is given
	std::array<float, sim::kPoseCount> height{ 1.8f, 1.15f, 0.3f };
	float radius = 0.35f;
	std::unique_ptr<BotNav> nav; // null: no walking
	sim::bots::WalkerParams walker;
	std::vector<FirefightLane> lanes;
	uint64_t match_seed = 0;
	FirefightStats *stats = nullptr;
	// Every slot's life and the tick it was first seen (scan_lives, once per tick by the first brain).
	std::vector<uint32_t> lives;
	std::vector<sim::Tick> life_since;
	sim::Tick scan_tick = UINT32_MAX;

	void scan_lives(sim::Tick p_tick, std::span<const sim::SoldierState> p_states) {
		if (scan_tick == p_tick) {
			return;
		}
		scan_tick = p_tick;
		if (lives.size() < p_states.size()) {
			lives.resize(p_states.size(), 0);
			life_since.resize(p_states.size(), p_tick);
		}
		for (size_t s = 0; s < p_states.size(); ++s) {
			if (p_states[s].life != lives[s]) {
				lives[s] = p_states[s].life;
				life_since[s] = p_tick;
			}
		}
	}
	bool fresh(size_t p_slot, sim::Tick p_tick) const {
		return p_slot < life_since.size() && p_tick - life_since[p_slot] < kFreshTargetTicks;
	}

	void set_motor(const sim::MotorParams &p_motor) {
		radius = p_motor.radius;
		for (size_t i = 0; i < sim::kPoseCount; ++i) {
			if (p_motor.poses[i].eye_height > 0.0f) {
				eye[i] = p_motor.poses[i].eye_height;
			}
			if (p_motor.poses[i].height > 0.0f) {
				height[i] = p_motor.poses[i].height;
			}
		}
	}
	float eye_of(sim::Pose p_pose) const { return eye[size_t(p_pose) < sim::kPoseCount ? size_t(p_pose) : 0]; }
	sim::Vec3 eye_point(const sim::SoldierState &p_state) const { return add(p_state.position, 0.0f, eye_of(p_state.pose), 0.0f); }
	sim::Vec3 chest_point(const sim::SoldierState &p_state) const { return add(p_state.position, 0.0f, kChestShare * eye_of(p_state.pose), 0.0f); }
	template <typename T>
	void count(std::atomic<T> FirefightStats::*p_field, T p_amount = 1) {
		if (stats != nullptr) {
			(stats->*p_field).fetch_add(p_amount, std::memory_order_relaxed);
		}
	}
};

// ---- the layout -------------------------------------------------------------------------------------------------

namespace {

constexpr float kLayoutEye = 1.6f, kLayoutRadius = 0.35f, kLayoutHeight = 1.8f; // the layout's stand soldier

// A lane end's foothold at (x, z): a walkable floor within the tolerance of p_floor_y, room for the stand capsule and
// (with a nav) a polygon of the main component within kLayoutNavExtent.
bool lane_foot(const sim::CollisionWorld &p_world, BotNav *p_nav, float p_floor_y, float p_x, float p_z, sim::Vec3 &r_feet) {
	sim::StaticHit hit;
	if (!p_world.ground_below({ p_x, p_floor_y + kLayoutProbeUp, p_z }, kLayoutProbeDrop, sim::query_mask::kMovement, &hit)) {
		return false;
	}
	if (std::fabs(hit.position[1] - p_floor_y) > kLayoutFloorTolerance || hit.normal[1] < kLayoutMinNormalY) {
		return false;
	}
	r_feet = hit.position;
	if (p_world.overlap_capsule(add(r_feet, 0.0f, 0.05f, 0.0f), kLayoutRadius, kLayoutHeight, sim::query_mask::kMovement, kLayoutCapsuleTolerance)) {
		return false;
	}
	if (p_nav != nullptr && p_nav->query != nullptr && p_nav->level != nullptr) {
		sim::NavPolyRef poly = 0;
		sim::Vec3 at{};
		if (!p_nav->query->nearest(r_feet, { kLayoutNavExtent, 2.0f, kLayoutNavExtent }, &poly, &at) || poly == 0) {
			return false;
		}
		if (sim::nav_poly_component(p_nav->query->world(), p_nav->level->components, poly) != p_nav->level->main_component) {
			return false;
		}
	}
	return true;
}

// Both ends see each other's chest (eye to chest, both ways).
bool lane_clear(const sim::CollisionWorld &p_world, const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	const float chest = kChestShare * kLayoutEye;
	return clear_ray(p_world, add(p_a, 0.0f, kLayoutEye, 0.0f), add(p_b, 0.0f, chest, 0.0f)) &&
			clear_ray(p_world, add(p_b, 0.0f, kLayoutEye, 0.0f), add(p_a, 0.0f, chest, 0.0f));
}

// p_lane moved p_offset along its normal, both ends re-footed and the pair ray checked; false when it does not hold.
bool offset_lane(const sim::CollisionWorld &p_world, BotNav *p_nav, const FirefightLane &p_lane, float p_offset, FirefightLane &r_out) {
	const float nx = std::cos(p_lane.yaw_a), nz = -std::sin(p_lane.yaw_a);
	FirefightLane l = p_lane;
	if (!lane_foot(p_world, p_nav, p_lane.a[1], p_lane.a[0] + p_offset * nx, p_lane.a[2] + p_offset * nz, l.a) ||
			!lane_foot(p_world, p_nav, p_lane.b[1], p_lane.b[0] + p_offset * nx, p_lane.b[2] + p_offset * nz, l.b) || !lane_clear(p_world, l.a, l.b)) {
		return false;
	}
	l.yaw_a = yaw_of(l.b[0] - l.a[0], l.b[2] - l.a[2]);
	l.yaw_b = yaw_of(l.a[0] - l.b[0], l.a[2] - l.b[2]);
	r_out = l;
	return true;
}

} // namespace

core::Result<std::vector<FirefightLane>> firefight_layout(const sim::CollisionWorld &p_world, const FirefightConfig &p_config, uint64_t p_seed) {
	return firefight_layout(p_world, p_config, p_seed, nullptr, nullptr);
}

core::Result<std::vector<FirefightLane>> firefight_layout(const sim::CollisionWorld &p_world, const FirefightConfig &p_config, uint64_t p_seed,
		BotNav *p_nav, uint32_t *r_rejected) {
	if (!p_config.observer.valid) {
		return core::make_error("firefight layout: no observer pose");
	}
	const sim::Vec3 o = p_config.observer.position;
	if (!std::isfinite(o[0]) || !std::isfinite(o[1]) || !std::isfinite(o[2]) || !std::isfinite(p_config.observer.yaw)) {
		return core::make_error("firefight layout: non-finite observer pose");
	}
	const float fan = std::clamp(p_config.lane_fan_deg, 0.0f, 180.0f) * kDeg;
	const float lane_min = std::max(p_config.lane_min_m, 1.0f);
	const float lane_max = std::max(p_config.lane_max_m, lane_min);
	const float pair_min = std::max(p_config.pair_min_m, 1.0f);
	const float pair_max = std::max(p_config.pair_max_m, pair_min);
	const uint32_t tries = std::clamp<uint32_t>(p_config.lane_retries, 1u, 64u);
	const uint32_t wanted = std::min<uint32_t>(p_config.count / 2u, 128u);
	// The observer's floor.
	sim::StaticHit floor_hit;
	const float floor_y = p_world.ground_below(o, 4.0f, sim::query_mask::kMovement, &floor_hit) ? floor_hit.position[1] : o[1] - 1.6f;
	const float chest = kChestShare * kLayoutEye;
	uint64_t rng = mix(p_seed ^ 0x4C414E45ull); // "LANE"
	uint32_t rejected = 0;
	std::vector<FirefightLane> out;
	const auto foot = [&](float p_x, float p_z, sim::Vec3 &r_feet) { return lane_foot(p_world, p_nav, floor_y, p_x, p_z, r_feet); };
	for (uint32_t i = 0; i < wanted; ++i) {
		const float base_angle = wanted > 1 ? -fan + 2.0f * fan * float(i) / float(wanted - 1) : 0.0f;
		const float golden = float(i) * 0.6180340f;
		const float base_dist = lane_min + (lane_max - lane_min) * (golden - std::floor(golden));
		bool accepted = false;
		for (uint32_t t = 0; t < tries && !accepted; ++t) {
			const float angle = t == 0 ? base_angle : uniform(rng, -fan, fan);
			const float dist = t == 0 ? base_dist : uniform(rng, lane_min, lane_max);
			const float length = uniform(rng, pair_min, pair_max);
			const float ray_yaw = p_config.observer.yaw + angle;
			const float cx = o[0] + dist * std::sin(ray_yaw), cz = o[2] + dist * std::cos(ray_yaw);
			// Even tries: across the view ray; odd tries: along it.
			const float axis_yaw = (t % 2u == 0) ? ray_yaw + 0.5f * sim::kPi : ray_yaw;
			const float ax = std::sin(axis_yaw), az = std::cos(axis_yaw);
			sim::Vec3 a{}, b{};
			if (!foot(cx - 0.5f * length * ax, cz - 0.5f * length * az, a) || !foot(cx + 0.5f * length * ax, cz + 0.5f * length * az, b)) {
				++rejected;
				continue;
			}
			const sim::Vec3 a_chest = add(a, 0.0f, chest, 0.0f), b_chest = add(b, 0.0f, chest, 0.0f);
			if (!lane_clear(p_world, a, b)) {
				++rejected;
				continue;
			}
			if (t < (tries + 1u) / 2u && (!clear_ray(p_world, o, a_chest) || !clear_ray(p_world, o, b_chest))) {
				++rejected; // the observer must see both ends on the first half of the tries
				continue;
			}
			FirefightLane lane;
			lane.a = a;
			lane.b = b;
			lane.yaw_a = yaw_of(b[0] - a[0], b[2] - a[2]);
			lane.yaw_b = yaw_of(a[0] - b[0], a[2] - b[2]);
			lane.tries = t + 1u;
			out.push_back(lane);
			accepted = true;
		}
	}
	if (r_rejected != nullptr) {
		*r_rejected = rejected;
	}
	if (out.empty() && wanted > 0) {
		return core::make_error("firefight layout: no lane holds around the observer (" + std::to_string(rejected) + " tries rejected)");
	}
	return out;
}

// ---- the brain ----------------------------------------------------------------------------------------------------

struct FirefightBrain::Impl {
	std::shared_ptr<FirefightShared> shared;
	uint32_t index = 0;
	uint8_t slot = 0xFF;
	bool have_place = false;
	sim::Vec3 place_feet{};
	float place_yaw = 0.0f;
	uint64_t seed = 0;
	uint64_t rng = 0;
	std::optional<sim::bots::WalkerBrain> walker;
	std::vector<sim::bots::BotGoal> goals;

	uint32_t life = UINT32_MAX;
	sim::Tick life_tick = 0;
	float view_yaw = 0.0f, view_pitch = 0.0f;
	// The target (a slot) and when it was acquired and last seen.
	int target = -1;
	uint32_t target_life = 0;
	sim::Tick target_since = 0, target_seen = 0;
	float err_yaw = 0.0f, err_pitch = 0.0f;
	sim::Tick err_next = 0;
	// Blind fire (no target for FirefightConfig::no_target_s).
	sim::Tick no_target_since = 0;
	bool blind = false;
	float blind_yaw = 0.0f;
	sim::Tick blind_until = 0;
	// Fire.
	bool bursting = false;
	uint32_t burst_rounds = 0, burst_start = 0;
	sim::Tick pause_until = 0, next_tap = 0;
	bool tapped = false;
	uint8_t mode_counter = 0;
	sim::Tick mode_wait = 0;
	uint8_t want_slot = 0xFF;
	bool reload_held = false;
	// Strafe.
	bool strafing = false;
	float strafe_dir = 1.0f;
	sim::Vec3 strafe_anchor{};
	float strafe_ax = 1.0f, strafe_az = 0.0f;
	sim::Tick strafe_until = 0;

	void new_life(sim::Tick p_tick, const sim::SoldierState &p_state) {
		const FirefightConfig &cfg = *shared->config;
		life = p_state.life;
		life_tick = p_tick;
		rng = mix(seed ^ (uint64_t(life) << 8));
		view_yaw = sim::yaw_radians(p_state.yaw);
		view_pitch = 0.0f;
		target = -1;
		target_since = target_seen = 0;
		err_yaw = err_pitch = 0.0f;
		err_next = 0;
		no_target_since = p_tick;
		blind = false;
		bursting = false;
		pause_until = p_tick;
		next_tap = p_tick;
		tapped = false;
		mode_wait = 0;
		want_slot = 0xFF;
		reload_held = false;
		strafing = false;
		if (cfg.mode == FirefightMode::Pairs && have_place) {
			// The RespawnPolicy has (re)spawned the slot at choose_spawn; it is alive, so the placement keeps its life
			// and weapons and clears the spawn protection (sv's place handling).
			const core::Status st = shared->server->place_soldier(slot, place_feet, sim::quantize_yaw(place_yaw), sim::Pose::Stand);
			if (st.ok()) {
				shared->count(&FirefightStats::placements, uint64_t(1));
				view_yaw = place_yaw;
			}
		}
	}

	// The nearest visible enemy (or the current target when it is still the best), -1 for none.
	int acquire(const ServerView &p_view, const sim::SoldierState &p_self, std::span<const sim::SoldierState> p_states) {
		const sim::CollisionWorld &world = p_view.world();
		const uint8_t team = p_view.team(slot);
		const sim::Vec3 eye = shared->eye_point(p_self);
		std::array<std::pair<float, uint8_t>, 128> order{};
		size_t n = 0;
		for (size_t s = 0; s < p_states.size() && n < order.size(); ++s) {
			if (s == slot || !p_view.alive(uint8_t(s))) {
				continue;
			}
			const uint8_t other = p_view.team(uint8_t(s));
			if (other == 0 || other == team) {
				continue;
			}
			const float d = distance(eye, shared->chest_point(p_states[s]));
			if (d <= kMaxEngageMetres && std::isfinite(d)) {
				order[n++] = { d, uint8_t(s) };
			}
		}
		std::sort(order.begin(), order.begin() + ptrdiff_t(n));
		float current_d = -1.0f;
		if (target >= 0 && size_t(target) < p_states.size()) {
			current_d = distance(eye, shared->chest_point(p_states[size_t(target)]));
		}
		uint32_t rays = 0;
		for (size_t k = 0; k < n && rays < kMaxRaysPerThink; ++k) {
			const uint8_t s = order[k].second;
			if (int(s) == target && current_d >= 0.0f) {
				return target; // the current target is the nearest of the rest (it is re-checked on its own cadence)
			}
			if (target >= 0 && current_d >= 0.0f && order[k].first * kKeepTargetRatio > current_d) {
				return target; // nobody visible is enough nearer than the current target
			}
			++rays;
			shared->count(&FirefightStats::los_rays, uint64_t(1));
			if (clear_ray(world, eye, shared->chest_point(p_states[s]))) {
				return int(s);
			}
		}
		return target;
	}

	// Free with walk: the hunt goal (the nearest live enemy, on the main component).
	void hunt(const ServerView &p_view, const sim::SoldierState &p_self, std::span<const sim::SoldierState> p_states) {
		BotNav &nav = *shared->nav;
		const uint8_t team = p_view.team(slot);
		float best = 1e30f;
		int pick = -1;
		for (size_t s = 0; s < p_states.size(); ++s) {
			if (s == slot || !p_view.alive(uint8_t(s))) {
				continue;
			}
			const uint8_t other = p_view.team(uint8_t(s));
			if (other == 0 || other == team) {
				continue;
			}
			const float d = distance(p_self.position, p_states[s].position);
			if (d < best) {
				best = d;
				pick = int(s);
			}
		}
		goals.clear();
		if (pick < 0) {
			return;
		}
		sim::NavPolyRef poly = 0;
		sim::Vec3 at{};
		if (!nav.query->nearest(p_states[size_t(pick)].position, { 2.0f, 3.0f, 2.0f }, &poly, &at) || poly == 0) {
			return;
		}
		if (sim::nav_poly_component(nav.query->world(), nav.level->components, poly) != nav.level->main_component) {
			return;
		}
		sim::bots::BotGoal g;
		g.kind = sim::bots::BotGoal::Kind::NavPoint;
		g.position = at;
		g.poly = poly;
		g.arrive_radius = kHuntArriveMetres;
		g.index = uint32_t(pick);
		goals.push_back(g);
		shared->count(&FirefightStats::hunts, uint64_t(1));
	}

	// The longest clear of 8 headings around the view (blind fire).
	float longest_heading(const sim::CollisionWorld &p_world, const sim::SoldierState &p_self) {
		const sim::Vec3 eye = shared->eye_point(p_self);
		float best_len = -1.0f, best_yaw = view_yaw;
		for (int k = 0; k < 8; ++k) {
			const float y = view_yaw + float(k) * (sim::kTwoPi / 8.0f);
			const sim::Vec3 delta{ kBlindRayMetres * std::sin(y), 0.0f, kBlindRayMetres * std::cos(y) };
			sim::StaticHit hit;
			const float len = p_world.ray(eye, delta, sim::query_mask::kProjectile, &hit) ? hit.fraction * kBlindRayMetres : kBlindRayMetres;
			shared->count(&FirefightStats::los_rays, uint64_t(1));
			if (len > best_len + 1e-3f) {
				best_len = len;
				best_yaw = y;
			}
		}
		return wrap(best_yaw);
	}

	// One strafe tick: the world-space move direction (x, z) of the leg.
	void strafe(sim::Tick p_tick, const sim::CollisionWorld &p_world, const sim::SoldierState &p_self, float &r_dx, float &r_dz) {
		const FirefightShared &sh = *shared;
		if (!strafing) {
			strafing = true;
			strafe_anchor = p_self.position;
			strafe_ax = -std::cos(view_yaw); // the view's right (command.h motor axes: right = (-cos, sin))
			strafe_az = std::sin(view_yaw);
			strafe_dir = unit(rng) < 0.5f ? -1.0f : 1.0f;
			strafe_until = p_tick + uniform_u(rng, kStrafeMinTicks, kStrafeMaxTicks);
			shared->count(&FirefightStats::strafes, uint64_t(1));
		} else if (p_tick >= strafe_until) {
			strafe_dir = -strafe_dir;
			strafe_until = p_tick + uniform_u(rng, kStrafeMinTicks, kStrafeMaxTicks);
			shared->count(&FirefightStats::strafes, uint64_t(1));
		}
		const sim::Vec3 off = sub(p_self.position, strafe_anchor);
		const float along = off[0] * strafe_ax + off[2] * strafe_az;
		if (along * strafe_dir > kStrafeHalfMetres) {
			strafe_dir = -strafe_dir;
		} else if ((p_tick + slot) % kStrafeGroundTicks == 0) {
			const sim::Vec3 ahead = add(p_self.position, strafe_dir * strafe_ax * 0.8f, 0.5f, strafe_dir * strafe_az * 0.8f);
			sim::StaticHit hit;
			if (!p_world.ground_below(ahead, 1.2f, sim::query_mask::kMovement, &hit)) {
				strafe_dir = -strafe_dir; // no floor there
			}
		}
		(void)sh;
		r_dx = strafe_dir * strafe_ax * kStrafeAxis;
		r_dz = strafe_dir * strafe_az * kStrafeAxis;
	}

	sim::Command command(sim::Tick p_tick, const sim::SoldierState &p_state) {
		FirefightShared &sh = *shared;
		const FirefightConfig &cfg = *sh.config;
		if (sh.nav != nullptr) {
			sh.nav->run_once(p_tick); // the first brain of the tick runs the path searches
		}
		sim::Command c = sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
		c.flags = 0;
		if (slot == 0xFF || !is_alive(p_state)) {
			target = -1;
			return c; // a dead soldier's command is never stepped; the RespawnPolicy respawns it
		}
		if (p_state.life != life) {
			new_life(p_tick, p_state);
			c.yaw = sim::quantize_yaw(view_yaw);
			return c; // a Pairs placement lands in this tick's rules step
		}
		const ServerView &view = sh.server->view();
		const std::span<const sim::SoldierState> states = view.states();
		sh.scan_lives(p_tick, states);
		const sim::CollisionWorld &world = view.world();
		const sim::Vec3 eye = sh.eye_point(p_state);
		const bool walking = cfg.mode == FirefightMode::Free && cfg.walk && sh.nav != nullptr && walker.has_value();

		// ---- perceive -----------------------------------------------------------------------------------------------
		if (target >= 0) {
			const size_t t = size_t(target);
			const bool gone = t >= states.size() || !view.alive(uint8_t(t)) || states[t].life != target_life || view.team(uint8_t(t)) == view.team(slot);
			if (gone) {
				target = -1;
			} else if ((p_tick + slot) % kTargetCheckTicks == 0) {
				sh.count(&FirefightStats::los_rays, uint64_t(1));
				if (clear_ray(world, eye, sh.chest_point(states[t]))) {
					target_seen = p_tick;
				} else if (p_tick - target_seen > kLoseTicks) {
					target = -1;
				}
			}
		}
		const uint32_t retarget = std::max<uint32_t>(cfg.retarget_ticks, 1u);
		const bool think = (p_tick + slot) % retarget == 0;
		const bool hit_now = (p_state.events & sim::soldier_events::kDamaged) != 0;
		if (think || (hit_now && target < 0)) {
			const int before = target;
			const int found = acquire(view, p_state, states);
			if (found >= 0 && found != before) {
				target = found;
				target_life = states[size_t(found)].life;
				target_since = p_tick;
				target_seen = p_tick;
				err_next = p_tick;
				sh.count(&FirefightStats::retargets, uint64_t(1));
			}
			if (walking && think) {
				hunt(view, p_state, states);
			}
		}
		const bool engaged = target >= 0;
		float target_distance = 0.0f;

		// ---- aim ------------------------------------------------------------------------------------------------------
		float want_yaw = view_yaw, want_pitch = 0.0f;
		bool have_aim = false;
		if (engaged) {
			const sim::Vec3 aim = sh.chest_point(states[size_t(target)]);
			const float dx = aim[0] - eye[0], dy = aim[1] - eye[1], dz = aim[2] - eye[2];
			const float h = std::sqrt(dx * dx + dz * dz);
			target_distance = std::sqrt(h * h + dy * dy);
			const float on = float(p_tick - target_since) * sim::kTickSeconds;
			const float decay = std::max(cfg.aim_sigma_decay_s, 1e-3f);
			const float share = std::clamp(1.0f - on / decay, 0.0f, 1.0f);
			const float sigma = (cfg.aim_sigma_end_deg + (cfg.aim_sigma_start_deg - cfg.aim_sigma_end_deg) * share) * kDeg;
			if (p_tick >= err_next) {
				err_yaw = gauss(rng) * sigma;
				err_pitch = gauss(rng) * sigma * kErrorPitchShare;
				err_next = p_tick + kErrorTicks;
			}
			want_yaw = wrap(yaw_of(dx, dz) + err_yaw);
			want_pitch = std::atan2(dy, std::max(h, 1e-3f)) + err_pitch;
			have_aim = true;
			no_target_since = p_tick;
			blind = false;
		} else {
			// Free: along the longest clear heading; Pairs: down the lane, where the partner reappears.
			const uint32_t blind_after = sim::seconds_to_ticks(std::max(cfg.no_target_s, 0.0f));
			if (!blind && p_tick - no_target_since >= blind_after && blind_after > 0) {
				blind = true;
				blind_yaw = (cfg.mode == FirefightMode::Pairs && have_place) ? place_yaw : longest_heading(world, p_state);
				blind_until = p_tick + kBlindTicks;
				sh.count(&FirefightStats::no_target_fire, uint64_t(1));
			}
			if (blind && p_tick >= blind_until) {
				blind = false;
				no_target_since = p_tick;
			}
			if (blind) {
				want_yaw = blind_yaw;
				have_aim = true;
			}
		}

		// ---- move ---------------------------------------------------------------------------------------------------
		float move_dx = 0.0f, move_dz = 0.0f;
		bool sprint = false;
		sim::Pose pose = sim::Pose::Stand;
		if (walking) {
			const sim::bots::WalkerView wv = sh.nav->view(p_tick, slot, p_state, goals, sh.eye_of(sim::Pose::Stand));
			const sim::Command wc = walker->command(wv);
			pose = wc.pose;
			const float wy = sim::yaw_radians(wc.yaw);
			const float f = sim::axis_value(wc.move_forward), r = sim::axis_value(wc.move_right);
			move_dx = f * std::sin(wy) - r * std::cos(wy);
			move_dz = f * std::cos(wy) + r * std::sin(wy);
			if (!have_aim) {
				want_yaw = wy;
			}
			sprint = (wc.buttons & sim::buttons::kSprint) != 0 && !have_aim;
			if (engaged && target_distance < kEngageStopMetres) {
				strafe(p_tick, world, p_state, move_dx, move_dz);
				sprint = false;
			} else {
				strafing = false;
			}
		} else if (engaged) {
			strafe(p_tick, world, p_state, move_dx, move_dz);
		} else {
			strafing = false;
			if (cfg.mode == FirefightMode::Pairs && have_place) {
				want_yaw = place_yaw; // face the partner's end
			}
		}

		// ---- turn ---------------------------------------------------------------------------------------------------
		const float yaw_step = kTurnRateDegS * kDeg * sim::kTickSeconds;
		const float pitch_step = kPitchTurnRateDegS * kDeg * sim::kTickSeconds;
		view_yaw = wrap(view_yaw + std::clamp(wrap(want_yaw - view_yaw), -yaw_step, yaw_step));
		want_pitch = std::clamp(want_pitch, -kMaxPitchDeg * kDeg, kMaxPitchDeg * kDeg);
		view_pitch += std::clamp(want_pitch - view_pitch, -pitch_step, pitch_step);
		const bool aligned = have_aim && std::fabs(wrap(want_yaw - view_yaw)) <= kFireConeDeg * kDeg && std::fabs(want_pitch - view_pitch) <= kFireConeDeg * kDeg;
		if (sprint) {
			// Only straight ahead: the walker's sprint cone, in the brain's own view.
			const float len = std::sqrt(move_dx * move_dx + move_dz * move_dz);
			sprint = len > 0.5f && std::fabs(wrap(yaw_of(move_dx, move_dz) - view_yaw)) <= kSprintConeDeg * kDeg;
		}

		// ---- weapon -------------------------------------------------------------------------------------------------
		sw::WeaponInput in;
		in.fire_mode_counter = mode_counter;
		const Gunplay *g = sh.server->gunplay();
		if (g != nullptr && g->armed(slot)) {
			const sw::SoldierWeaponsState &ws = g->weapons(slot);
			const sw::WeaponTable &table = g->table();
			const sw::WeaponSlotState &ss = ws.slots[ws.active < sw::kMaxWeaponSlots ? ws.active : 0];
			if (ss.def != sw::kNoWeapon && ss.def < table.defs.size()) {
				const sw::WeaponSimDef &d = table.defs[ss.def];
				const uint8_t modes = std::min<uint8_t>(d.mode_count, uint8_t(d.modes.size()));
				int auto_mode = -1;
				for (uint8_t k = 0; k < modes; ++k) {
					if (d.modes[k].logic == core::gameplay::FireLogic::Automatic) {
						auto_mode = k;
						break;
					}
				}
				const sw::WeaponPhase phase = ss.phase;
				if (auto_mode >= 0 && ss.mode != uint8_t(auto_mode) && phase == sw::WeaponPhase::Ready && p_tick > life_tick + kModeDelayTicks &&
						p_tick >= mode_wait && modes > 1) {
					const uint8_t steps = uint8_t((uint32_t(auto_mode) + modes - ss.mode) % modes);
					if (steps > 0 && steps <= 3) {
						mode_counter = uint8_t((mode_counter + steps) & 3u);
						in.fire_mode_counter = mode_counter;
						mode_wait = p_tick + kModeWaitTicks;
						sh.count(&FirefightStats::mode_switches, uint64_t(steps));
					}
				}
				const sw::FireModeDef &fm = d.modes[ss.mode < modes ? ss.mode : 0];
				const bool automatic = fm.logic == core::gameplay::FireLogic::Automatic;
				const bool busy = phase == sw::WeaponPhase::Deploying || phase == sw::WeaponPhase::Reloading || phase == sw::WeaponPhase::ReloadShell ||
						phase == sw::WeaponPhase::Holding || phase == sw::WeaponPhase::Releasing || phase == sw::WeaponPhase::None;
				// The sidearm when the primary has no round left at all.
				if (ws.active == 0 && ss.magazine <= 0 && ss.reserve <= 0 && !d.unlimited_ammo && want_slot != 1) {
					const sw::WeaponSlotState &side = ws.slots[1];
					if (side.def != sw::kNoWeapon && side.magazine + side.reserve > 0) {
						want_slot = 1;
						sh.count(&FirefightStats::sidearm_switches, uint64_t(1));
					}
				}
				in.select_slot = want_slot;
				const bool reserve = ss.reserve > 0 || d.unlimited_ammo;
				const bool need = !busy && ss.magazine <= 0 && reserve;
				const float capacity = float(std::max(d.magazine_capacity, 1));
				const bool tactical = !have_aim && !busy && reserve && float(ss.magazine) < cfg.reload_fraction * capacity;
				const bool reload = need || tactical;
				if (reload && !reload_held) {
					sh.count(&FirefightStats::reloads, uint64_t(1));
				}
				reload_held = reload;
				in.reload = reload;
				// A target is fired at only while it was seen at its last check (the aim follows it for kLoseTicks).
				const bool seen = !engaged || p_tick - target_seen <= kTargetCheckTicks;
				const bool fresh = engaged && sh.fresh(size_t(target), p_tick);
				const bool want_fire = aligned && seen && !fresh && !busy && !reload && ss.magazine > 0 && !sprint;
				bool fire = false;
				if (automatic) {
					if (bursting) {
						const uint32_t fired = ws.next_shot_index - burst_start;
						if (fired >= burst_rounds || !want_fire) {
							bursting = false;
							pause_until = p_tick + sim::seconds_to_ticks(uniform(rng, cfg.burst_pause_min_s, cfg.burst_pause_max_s));
						} else {
							fire = true;
						}
					} else if (want_fire && p_tick >= pause_until) {
						bursting = true;
						burst_rounds = uniform_u(rng, std::max(cfg.burst_min_rounds, 1u), std::max(cfg.burst_max_rounds, std::max(cfg.burst_min_rounds, 1u)));
						burst_start = ws.next_shot_index;
						fire = true;
						sh.count(&FirefightStats::bursts, uint64_t(1));
					}
				} else if (tapped) {
					tapped = false; // the release tick: single fire reads the trigger transition
				} else if (want_fire && p_tick >= next_tap) {
					fire = true;
					tapped = true;
					next_tap = p_tick + std::max<uint32_t>(1u, uint32_t(std::lround(float(sim::kSimHz) / std::max(cfg.semi_rate_hz, 0.1f))));
					sh.count(&FirefightStats::bursts, uint64_t(1));
				}
				in.fire = fire;
			}
		}

		// ---- the command --------------------------------------------------------------------------------------------
		c.yaw = sim::quantize_yaw(view_yaw);
		c.pitch = sim::quantize_pitch(view_pitch);
		c.pose = pose;
		const float sy = std::sin(view_yaw), cy = std::cos(view_yaw);
		const float fwd = move_dx * sy + move_dz * cy;
		const float rgt = -move_dx * cy + move_dz * sy;
		c.move_forward = sim::quantize_axis(fwd);
		c.move_right = sim::quantize_axis(rgt);
		uint32_t b = sprint ? sim::buttons::kSprint : 0u;
		b = sw::with_weapon_input(b, in);
		c.buttons = b;
		return c;
	}
};

FirefightBrain::FirefightBrain(GameServer &r_server, std::shared_ptr<const FirefightConfig> p_config, uint32_t p_index, const FirefightLane *p_lane) :
		impl_(std::make_unique<Impl>()) {
	auto shared = std::make_shared<FirefightShared>();
	shared->server = &r_server;
	shared->config = p_config != nullptr ? std::move(p_config) : std::make_shared<const FirefightConfig>();
	shared->stats = shared->config->stats.get();
	shared->match_seed = r_server.gunplay() != nullptr ? r_server.gunplay()->match_seed() : 0;
	if (p_lane != nullptr) {
		shared->lanes.push_back(*p_lane);
	}
	Impl &m = *impl_;
	m.index = p_index;
	if (!shared->lanes.empty()) {
		const FirefightLane &lane = shared->lanes.front();
		m.have_place = true;
		m.place_feet = (p_index % 2u == 0) ? lane.a : lane.b;
		m.place_yaw = (p_index % 2u == 0) ? lane.yaw_a : lane.yaw_b;
	}
	m.shared = std::move(shared);
}

FirefightBrain::FirefightBrain(std::shared_ptr<FirefightShared> p_shared, uint32_t p_index, const FirefightLane *p_lane, uint8_t p_end, float p_offset) :
		impl_(std::make_unique<Impl>()) {
	Impl &m = *impl_;
	m.shared = std::move(p_shared);
	m.index = p_index;
	if (p_lane != nullptr) {
		m.have_place = true;
		m.place_feet = p_end == 0 ? p_lane->a : p_lane->b;
		m.place_yaw = p_end == 0 ? p_lane->yaw_a : p_lane->yaw_b;
		if (p_offset != 0.0f) {
			// Along the lane's normal (both ends of a pair move the same way, so they still face each other).
			const float nx = std::cos(p_lane->yaw_a), nz = -std::sin(p_lane->yaw_a);
			m.place_feet[0] += p_offset * nx;
			m.place_feet[2] += p_offset * nz;
		}
	}
}

FirefightBrain::~FirefightBrain() = default;

sim::Command FirefightBrain::command(sim::Tick p_tick, const sim::SoldierState &p_state) {
	return impl_->command(p_tick, p_state);
}

void FirefightBrain::set_slot(uint8_t p_slot) {
	Impl &m = *impl_;
	m.slot = p_slot;
	m.seed = firefight_seed(m.shared->match_seed, p_slot);
	m.walker.reset();
	if (m.shared->nav != nullptr && m.shared->config->walk && m.shared->config->mode == FirefightMode::Free) {
		m.walker.emplace(p_slot, m.seed, m.shared->walker);
	}
}

// ---- the hook ------------------------------------------------------------------------------------------------------

core::Status add_firefight_bots(GameServer &r_server, const GameServerConfig &p_config) {
	if (p_config.firefight == nullptr || p_config.firefight->count == 0) {
		return core::Status::success();
	}
	if (p_config.level == nullptr) {
		return core::make_error("firefight bots: no level");
	}
	const FirefightConfig &cfg = *p_config.firefight;
	auto shared = std::make_shared<FirefightShared>();
	shared->server = &r_server;
	shared->config = p_config.firefight;
	shared->stats = cfg.stats.get();
	shared->set_motor(p_config.level->motor);
	shared->match_seed = r_server.gunplay() != nullptr ? r_server.gunplay()->match_seed() : p_config.seed;
	if (cfg.walk) {
		if (p_config.bots != nullptr && p_config.bots->level != nullptr && p_config.bots->level->nav != nullptr) {
			core::Result<std::unique_ptr<BotNav>> nav = make_bot_nav(p_config.bots->level, p_config.bots->paths, p_config.bots->query);
			if (!nav) {
				return core::error_at("firefight bots", nav.error());
			}
			shared->nav = std::move(*nav);
			shared->walker = p_config.bots->walker;
		} else if (shared->stats != nullptr && cfg.mode == FirefightMode::Free) {
			shared->stats->walk_unavailable.store(1, std::memory_order_relaxed); // Pairs strafes on its lanes, it never walks
		}
	}
	shared->walker.objective_share = kHuntShare;

	uint32_t count = cfg.count;
	if (cfg.mode == FirefightMode::Pairs) {
		count -= count % 2u; // an odd count leaves the last bot out
		FirefightConfig layout = cfg;
		if (!layout.observer.valid) {
			// Headless runs: the start of the level's range lane (Metro: spawn 87, m2a.md §1).
			const std::optional<sim::weapons::RangeLane> lane = sim::weapons::find_range_lane(r_server.view().world(), p_config.level->spawns,
					shared->eye_of(sim::Pose::Stand), kRangeLaneMinMetres);
			if (!lane) {
				return core::make_error("firefight bots: no observer pose given and no range lane of " + std::to_string(int(kRangeLaneMinMetres)) + " m");
			}
			layout.observer.valid = true;
			layout.observer.position = lane->eye;
			layout.observer.yaw = sim::yaw_radians(lane->yaw);
		}
		uint32_t rejected = 0;
		core::Result<std::vector<FirefightLane>> lanes = firefight_layout(r_server.view().world(), layout, shared->match_seed, shared->nav.get(), &rejected);
		if (!lanes) {
			return core::error_at("firefight bots", lanes.error());
		}
		shared->lanes = std::move(*lanes);
		if (shared->stats != nullptr) {
			shared->stats->lanes.store(uint32_t(shared->lanes.size()), std::memory_order_relaxed);
			shared->stats->lanes_rejected.store(rejected, std::memory_order_relaxed);
		}
	}
	FirefightLane moved; // the current pair's lane on a reuse
	for (uint32_t i = 0; i < count; ++i) {
		const FirefightLane *lane = nullptr;
		if (cfg.mode == FirefightMode::Pairs && !shared->lanes.empty()) {
			const size_t pair = i / 2u;
			lane = &shared->lanes[pair % shared->lanes.size()];
			const size_t reuse = pair / shared->lanes.size();
			if (reuse > 0) {
				// A reused lane moves along its normal: the k-th reuse tries +-k, +-(k + 1), ... steps of 1.2 m, each with
				// both ends re-footed and the pair ray checked; when none holds the pair shares the lane as it is.
				if (i % 2u == 0) {
					moved = *lane;
					bool held = false;
					for (uint32_t t = 0; t < kSharedLaneTries && !held; ++t) {
						const float step = kSharedLaneOffset * float(reuse + t / 2u);
						held = offset_lane(r_server.view().world(), shared->nav.get(), *lane, (t % 2u == 0) ? step : -step, moved);
					}
					if (!held) {
						moved = *lane;
					}
					if (shared->stats != nullptr) {
						shared->stats->lanes_shared.fetch_add(1, std::memory_order_relaxed);
					}
				}
				lane = &moved;
			}
		}
		auto brain = std::make_unique<FirefightBrain>(shared, i, lane, uint8_t(i % 2u), 0.0f);
		FirefightBrain *raw = brain.get();
		// Teams alternate within a pair and flip every four bots, so each team gets every kit of kit = slot % 4 (with
		// 1 + i % 2 alone team 1 held kits 0 and 2 and team 2 kits 1 and 3: measured 5,367 vs 3,164 shots in 60 s).
		const int slot = r_server.add_local_soldier(std::move(brain), 1 + int((i + i / 4u) % 2u));
		if (slot < 0) {
			return core::make_error("firefight bots: no slot for bot " + std::to_string(i) + " of " + std::to_string(count));
		}
		raw->set_slot(uint8_t(slot));
		if (shared->stats != nullptr) {
			shared->stats->bots.fetch_add(1, std::memory_order_relaxed);
		}
	}
	return core::Status::success();
}

} // namespace bf3::host
