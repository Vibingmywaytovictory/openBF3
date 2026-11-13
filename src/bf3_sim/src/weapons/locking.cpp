#include "bf3/sim/weapons/locking.h"

#include "bf3/core/math.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/static_world.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace bf3::sim::weapons {

namespace {

double squared_reach(const LockingControllerSimDef &p_def, const Vec3 &p_from, const Vec3 &p_to) {
	const double dx = double(p_to[0]) - double(p_from[0]);
	const double dy = p_def.ignore_height ? 0.0 : double(p_to[1]) - double(p_from[1]);
	const double dz = double(p_to[2]) - double(p_from[2]);
	return dx * dx + dy * dy + dz * dz;
}

bool visible(const CollisionWorld &p_world, const Vec3 &p_from, const Vec3 &p_aim, LockStepStats *r_stats) {
	const Vec3 to_aim = core::vec::sub(p_aim, p_from);
	const float reach = core::vec::length(to_aim);
	if (!(reach > 0.0f)) {
		return true;
	}
	const Vec3 ray = core::vec::scale(to_aim, (reach + kLockVisibilityBeyond) / reach);
	if (r_stats != nullptr) {
		++r_stats->rays;
	}
	StaticHit hit;
	if (!p_world.ray(p_from, ray, query_mask::kProjectile, &hit)) {
		return true;
	}
	return hit.fraction * (reach + kLockVisibilityBeyond) >= reach;
}

}

float lock_signature(const LockCandidate &p_candidate, LockType p_type) {
	if (!p_candidate.lockable) {
		return 0.0f;
	}
	switch (p_type) {
		case LockType::Always:
			return 1.0f;
		case LockType::Radar:
			return p_candidate.radar;
		case LockType::Heat:
			return p_candidate.heat;
		case LockType::LaserPainted:
			return p_candidate.painted ? 1.0f : 0.0f;
		case LockType::Never:
		case LockType::Count:
			return 0.0f;
	}
	return 0.0f;
}

const LockCandidate *pick_lock_candidate(const LockingControllerSimDef &p_def, LockType p_type, const LockAim &p_aim,
		std::span<const LockCandidate> p_candidates, const CollisionWorld *p_world, LockStepStats *r_stats) {
	const double ray2 = double(p_def.ray_length) * double(p_def.ray_length);
	const double cone = double(p_def.acceptance_angle_deg) * std::numbers::pi / 180.0;
	const bool nearest = cone <= double(kLockConeFloor);
	const double min2 = double(kLockMinDistance) * double(kLockMinDistance);
	const LockCandidate *best = nullptr;
	double best_score = -1.0;
	for (const LockCandidate &c : p_candidates) {
		const float signature = lock_signature(c, p_type);
		if (signature < kLockSignatureFloor) {
			continue;
		}
		if (!c.occupied && !p_def.lock_on_empty_vehicles && !c.never_report_empty) {
			continue;
		}
		if (p_aim.team != 0 && c.team == p_aim.team) {
			continue;
		}
		const double d2 = squared_reach(p_def, p_aim.origin, c.position);
		if (!(d2 < ray2) || d2 < min2) {
			continue;
		}
		const Vec3 toward = core::vec::sub(c.position, p_aim.origin);
		const float along = core::vec::dot(toward, p_aim.forward);
		if (!(along > 0.0f)) {
			continue;
		}
		if (r_stats != nullptr) {
			++r_stats->candidates;
		}
		const double cosine = std::clamp(double(along) / std::max(double(core::vec::length(toward)), 1e-9), -1.0, 1.0);
		const double angle = std::acos(cosine);
		double score = 0.0;
		if (nearest) {
			score = ray2 - d2;
		} else {
			if (angle > cone) {
				continue;
			}
			score = (double(p_def.distance_constant) * (1.0 - d2 / ray2) +
							double(p_def.angle_constant) * (1.0 - angle / std::numbers::pi)) *
					double(signature);
		}
		if (!(score > best_score)) {
			continue;
		}
		if (p_def.visible_only && p_world != nullptr && !visible(*p_world, p_aim.origin, c.aim, r_stats)) {
			continue;
		}
		best = &c;
		best_score = score;
	}
	return best;
}

void step_lock(const LockingControllerSimDef &p_def, LockState &r_state, const LockAim &p_aim, SimTime p_dt,
		std::span<const LockCandidate> p_candidates, const CollisionWorld *p_world, LockStepStats *r_stats) {
	const LockType type = p_def.lock_type(p_aim.zoom);
	if (!p_def.present || type == LockType::Never) {
		r_state = LockState();
		return;
	}
	r_state.elapsed += p_dt;
	r_state.hold -= p_dt;
	if (r_state.elapsed < p_def.sample_rate || r_state.hold > 0) {
		return;
	}
	const SimTime elapsed = r_state.elapsed;
	r_state.elapsed = 0;
	++r_state.samples;
	if (r_stats != nullptr) {
		++r_stats->samples;
	}
	const SimTime full = scale_units(p_def.lock_time, double(r_state.time_multiplier));
	const LockCandidate *pick = pick_lock_candidate(p_def, type, p_aim, p_candidates, p_world, r_stats);
	if (pick != nullptr && pick->id == r_state.target) {
		if (r_state.timer >= full) {
			r_state.release = p_def.release_time;
		}
		r_state.timer = std::min(r_state.timer + elapsed, full);
		r_state.time_multiplier = pick->time_multiplier;
	} else if (pick != nullptr) {
		r_state.release -= elapsed;
		if (r_state.release < p_def.release_time - p_def.release_on_new_target_time) {
			r_state.target = pick->id;
			r_state.timer = 0;
			r_state.time_multiplier = pick->time_multiplier;
		}
	} else {
		r_state.release -= elapsed;
		if (r_state.release < 0) {
			r_state.timer -= elapsed;
			if (r_state.timer < 0) {
				r_state.target = kNoLockTarget;
				r_state.timer = 0;
				r_state.time_multiplier = 1;
			}
		}
	}
	const SimTime lock_time = scale_units(p_def.lock_time, double(r_state.time_multiplier));
	r_state.amount = lock_time <= 0 ? 1.0f : float(double(r_state.timer) / double(lock_time));
	if (r_state.target != kNoLockTarget && r_state.amount > kLockedAmount) {
		r_state.amount = 1.0f;
		r_state.locked = true;
		r_state.hold = p_def.minimum_lock_time;
	} else {
		r_state.locked = false;
	}
}

void step_weapon_lock(const LockingSimDef &p_def, WeaponLockState &r_state, const LockAim &p_aim, SimTime p_dt,
		std::span<const LockCandidate> p_candidates, const CollisionWorld *p_world, LockStepStats *r_stats) {
	step_lock(p_def.primary, r_state.primary, p_aim, p_dt, p_candidates, p_world, r_stats);
	step_lock(p_def.secondary, r_state.secondary, p_aim, p_dt, p_candidates, p_world, r_stats);
}

}
