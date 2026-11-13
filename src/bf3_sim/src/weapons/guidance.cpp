#include "bf3/sim/weapons/guidance.h"

#include "bf3/sim/weapons/weapon_rng.h"
#include "bf3/sim/weapons/weapon_sim_def.h"
#include "weapons_internal.h"

#include <algorithm>
#include <cmath>

namespace bf3::sim::weapons {

namespace {

using namespace detail;

double horizontal_distance(const DVec3 &p_a, const DVec3 &p_b) {
	const double dx = p_a[0] - p_b[0];
	const double dz = p_a[2] - p_b[2];
	return std::sqrt(dx * dx + dz * dz);
}

bool engine_state(MissileState p_state) {
	switch (p_state) {
		case MissileState::Powered:
		case MissileState::StaticPoint:
		case MissileState::TargetOffset:
		case MissileState::Homing:
		case MissileState::Proximity:
			return true;
		case MissileState::PreIgnition:
		case MissileState::BurntOut:
			return false;
	}
	return false;
}

}

DVec3 missile_static_point(const DVec3 &p_origin, const DVec3 &p_direction, const std::array<float, 2> &p_static) {
	const DVec3 flat{ p_direction[0], 0.0, p_direction[2] };
	const double l = length(flat);
	if (!(l > kVerticalLaunchFloor)) {
		return kVerticalStaticPoint;
	}
	const DVec3 ahead = scale(flat, double(p_static[0]) / l);
	return add(add(p_origin, ahead), DVec3{ 0.0, double(p_static[1]), 0.0 });
}

DVec3 steer_toward(const DVec3 &p_velocity, const DVec3 &p_position, const DVec3 &p_target, double p_max_turn_rad) {
	const double speed = length(p_velocity);
	if (!(speed > kMissileDirectionFloor)) {
		return p_velocity;
	}
	const DVec3 u = normalize(sub(p_target, p_position), scale(p_velocity, 1.0 / speed));
	const DVec3 d = scale(p_velocity, 1.0 / speed);
	const double theta = std::acos(std::clamp(dot(d, u), -1.0, 1.0));
	if (!(theta > kSteerThreshold)) {
		return p_velocity;
	}
	const double phi = std::min(theta, std::max(p_max_turn_rad, 0.0));
	double sin_theta = 0, cos_theta = 0, sin_rest = 0, cos_rest = 0, sin_phi = 0, cos_phi = 0;
	poly_sincos(theta, sin_theta, cos_theta);
	poly_sincos(theta - phi, sin_rest, cos_rest);
	poly_sincos(phi, sin_phi, cos_phi);
	if (!(sin_theta > kSteerThreshold)) {
		return p_velocity;
	}
	const DVec3 turned = scale(add(scale(d, sin_rest), scale(u, sin_phi)), 1.0 / sin_theta);
	return scale(turned, speed);
}

MissileStep step_guided_missile(const ProjectileSimDef &p_def, const MissileGuide &p_guide, const DVec3 &p_position,
		const DVec3 &p_velocity, double p_flight, double p_dt, SimTime p_now,
		const std::optional<GuidanceTarget> &p_target, uint64_t p_seed) {
	const GuidanceSimDef &g = p_def.guidance;
	MissileStep out;
	out.guide = p_guide;
	MissileGuide &m = out.guide;
	if (!p_target) {
		m.target = kNoLockTarget;
	}
	const bool has_target = m.target != kNoLockTarget;
	const DVec3 aim = has_target ? to_d(p_target->aim) : DVec3{};
	const double ignition = units_to_seconds(p_def.engine_ignition);
	const double burnout = ignition + units_to_seconds(p_def.engine_time_to_live);
	const double activate = units_to_seconds(g.activate);
	const auto homing = [&] {
		return has_target && p_flight > activate &&
				(!g.use_target || horizontal_distance(aim, p_position) < double(g.target_offset[0]));
	};
	const auto statics = [&] { return g.use_static && (m.has_static || has_target); };
	const auto target_position = [&] { return p_flight > activate && g.use_target; };
	const auto near = [&] {
		if (!g.near_target) {
			return false;
		}
		DVec3 to{};
		if (has_target) {
			to = to_d(p_target->position);
		} else if (g.use_static && m.has_static) {
			to = m.static_point;
		} else {
			return false;
		}
		const DVec3 gap = sub(to, p_position);
		return dot(gap, gap) <= double(g.near_radius) * double(g.near_radius);
	};
	const auto arm_fuse = [&] {
		m.state = MissileState::Proximity;
		const float u = unit_float(shot_random(p_seed, RngPurpose::NearTargetDelay));
		m.detonate_at = p_now + g.near_min_delay + scale_units(g.near_max_delay - g.near_min_delay, double(u));
	};
	switch (m.state) {
		case MissileState::PreIgnition:
			if (p_flight >= ignition) {
				m.state = MissileState::Powered;
			}
			break;
		case MissileState::Powered:
			if (p_flight > burnout) {
				m.state = MissileState::BurntOut;
			} else if (near()) {
				arm_fuse();
			} else if (homing()) {
				m.state = MissileState::Homing;
			} else if (statics()) {
				m.state = MissileState::StaticPoint;
			} else if (target_position()) {
				m.state = MissileState::TargetOffset;
			}
			break;
		case MissileState::StaticPoint:
			if (target_position()) {
				m.state = MissileState::TargetOffset;
			} else if (homing()) {
				m.state = MissileState::Homing;
			}
			break;
		case MissileState::TargetOffset:
			if (homing()) {
				m.state = MissileState::Homing;
			}
			break;
		case MissileState::Homing:
			if (p_flight > burnout) {
				m.state = MissileState::BurntOut;
			} else if (near()) {
				arm_fuse();
			} else if (!has_target && !m.has_static) {
				m.state = MissileState::Powered;
			}
			break;
		case MissileState::BurntOut:
		case MissileState::Proximity:
			break;
	}
	DVec3 v = p_velocity;
	const double turn = double(g.max_turn_deg_per_s) * kPiD / 180.0 * p_dt;
	const double gravity = double(p_def.gravity);
	switch (m.state) {
		case MissileState::PreIgnition:
		case MissileState::Powered:
			if (p_def.gravity_when_guided) {
				v[1] += gravity * p_dt;
			}
			break;
		case MissileState::StaticPoint:
			if (m.has_static) {
				v = steer_toward(v, p_position, m.static_point, turn);
			}
			break;
		case MissileState::TargetOffset:
			if (has_target) {
				v = steer_toward(v, p_position, add(aim, DVec3{ 0.0, double(g.target_offset[1]), 0.0 }), turn);
			}
			break;
		case MissileState::Homing:
			if (has_target) {
				v = steer_toward(v, p_position, aim, turn);
			}
			break;
		case MissileState::BurntOut:
			v = with_speed(v, std::max(length(v) - double(p_def.drag), 0.0));
			v[1] += gravity * p_dt;
			break;
		case MissileState::Proximity:
			if (has_target) {
				v = steer_toward(v, p_position, aim, turn);
			} else if (p_def.gravity_when_guided) {
				v[1] += gravity * p_dt;
			}
			out.detonate = p_now > m.detonate_at;
			break;
	}
	out.engine = engine_state(m.state) && p_flight >= ignition && p_flight <= burnout;
	if (out.engine) {
		v = with_speed(v, std::min(length(v) + double(p_def.engine_acceleration) * p_dt, double(p_def.max_speed)));
	}
	if (length(v) > double(p_def.max_speed)) {
		v = with_speed(v, double(p_def.max_speed));
	}
	out.velocity = v;
	return out;
}

}
