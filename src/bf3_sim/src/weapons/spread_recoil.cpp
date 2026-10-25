#include "bf3/sim/weapons/spread_recoil.h"

#include "weapons_internal.h"

#include <algorithm>
#include <cmath>

namespace bf3::sim::weapons {

namespace {

void apply(Dispersion &r_d, const SwayModifier &p_m) {
	r_d.min_angle *= p_m.min_angle;
	if (p_m.max_angle != 0) {
		r_d.max_angle *= p_m.max_angle;
	}
	r_d.increase_per_shot *= p_m.increase_per_shot;
	r_d.decrease_per_second *= p_m.decrease_per_second;
}
void apply(Recoil &r_r, const SwayModifier &p_m) {
	r_r.up_per_shot *= p_m.recoil_magnitude;
	r_r.horizontal_min *= p_m.recoil_angle;
	r_r.horizontal_max *= p_m.recoil_angle;
}

const SwayState &state_of(const WeaponSimDef &p_def, const SwayContext &p_context) {
	const size_t s = std::min<size_t>(size_t(p_context.stance), 2);
	const size_t z = std::min<size_t>(size_t(p_context.zoom), 1);
	return p_def.sway[s][z];
}

template <class T>
void apply_gated(T &r_value, const WeaponSimDef &p_def, const SwayContext &p_context) {
	const size_t s = std::min<size_t>(size_t(p_context.stance), 2);
	const size_t z = std::min<size_t>(size_t(p_context.zoom), 1);
	if (p_context.suppressed) {
		apply(r_value, p_def.suppression_modifier[z]);
	}
	for (const WeaponSimDef::Conditional &c : p_def.conditional) {
		const bool gate = (!c.only_supported || p_context.supported) && (!c.only_light || p_context.light_on) && (c.only_supported || c.only_light);
		if (gate) {
			apply(r_value, c.mod[s][z]);
		}
	}
}

Dispersion move_dispersion(const SwayState &p_state, SwayMove p_move) {
	const size_t m = std::min<size_t>(size_t(p_move), size_t(SwayMove::Count) - 1);
	return p_state.has_move[m] ? p_state.move[m] : p_state.move[size_t(SwayMove::Base)];
}

Dispersion blend(const Dispersion &p_from, const Dispersion &p_to, float p_t) {
	Dispersion d;
	d.min_angle = p_from.min_angle + (p_to.min_angle - p_from.min_angle) * p_t;
	d.max_angle = p_from.max_angle + (p_to.max_angle - p_from.max_angle) * p_t;
	d.increase_per_shot = p_from.increase_per_shot + (p_to.increase_per_shot - p_from.increase_per_shot) * p_t;
	d.decrease_per_second = p_from.decrease_per_second + (p_to.decrease_per_second - p_from.decrease_per_second) * p_t;
	return d;
}

float approach_zero(float p_value, float p_step) {
	if (p_value > 0) {
		return std::max(0.0f, p_value - p_step);
	}
	return std::min(0.0f, p_value + p_step);
}

}

Dispersion effective_dispersion(const WeaponSimDef &p_def, const SwayContext &p_context) {
	const SwayState &st = state_of(p_def, p_context);
	Dispersion d = move_dispersion(st, p_context.move);
	if (p_context.move == SwayMove::Moving && p_context.move_fraction < 1.0f) {
		d = blend(move_dispersion(st, SwayMove::Base), d, std::max(0.0f, p_context.move_fraction));
	}
	apply_gated(d, p_def, p_context);

	d.min_angle = std::max(0.0f, d.min_angle);
	d.max_angle = std::max(0.0f, d.max_angle);
	d.increase_per_shot = std::max(0.0f, d.increase_per_shot);
	return d;
}

Recoil effective_recoil(const WeaponSimDef &p_def, const SwayContext &p_context) {
	Recoil r = state_of(p_def, p_context).recoil;
	apply_gated(r, p_def, p_context);
	return r;
}

float decay_spread(float p_spread, const Dispersion &p_dispersion, SimTime p_elapsed) {
	float s = p_spread;
	const float top = std::max(p_dispersion.max_angle, p_dispersion.min_angle);
	if (p_elapsed > 0 && p_dispersion.decrease_per_second < 0) {

		s = std::min(top, s - p_dispersion.decrease_per_second * float(units_to_seconds(p_elapsed)));
	} else if (p_elapsed > 0 && s > p_dispersion.min_angle) {
		s = std::max(p_dispersion.min_angle, s - p_dispersion.decrease_per_second * float(units_to_seconds(p_elapsed)));
	}
	return std::max(0.0f, std::min(s, top));
}

float enter_spread(float p_spread, const Dispersion &p_new, const Mechanics &p_mechanics) {
	if (p_mechanics.min_spread_rise == MinSpreadRise::Immediate) {
		return std::max(p_spread, p_new.min_angle);
	}
	return p_spread;
}

float spread_after_shot(float p_spread, const Dispersion &p_dispersion) {
	return std::min(p_spread + p_dispersion.increase_per_shot, std::max(p_dispersion.max_angle, p_dispersion.min_angle));
}

float spread_at(float p_spread, SimTime p_spread_time, SimTime p_now, const Dispersion &p_dispersion, const Mechanics &p_mechanics) {
	const SimTime elapsed = p_now > p_spread_time ? p_now - p_spread_time : 0;
	float spread = decay_spread(p_spread, p_dispersion, elapsed);
	if (p_mechanics.min_spread_rise == MinSpreadRise::AtDecreaseRate && spread < p_dispersion.min_angle) {
		spread = std::min(p_dispersion.min_angle, spread + p_dispersion.decrease_per_second * float(units_to_seconds(elapsed)));
	}
	return enter_spread(spread, p_dispersion, p_mechanics);
}

SwayContext suppressed_sway(const SwayContext &p_sway, float p_suppression, const Mechanics &p_mechanics) {
	SwayContext s = p_sway;
	s.suppressed = p_sway.suppressed || p_suppression >= p_mechanics.suppression_threshold;
	return s;
}

Vec3 sample_cone(const Vec3 &p_aim, float p_spread_degrees, float p_u_radius, float p_u_angle, DiscDistribution p_disc) {
	using namespace detail;
	const DVec3 aim = normalize(to_d(p_aim));
	if (!(p_spread_degrees > 0)) {
		return to_f(aim);
	}
	const double u = std::clamp(double(p_u_radius), 0.0, 1.0);
	const double radius = double(p_spread_degrees) * core::kDegToRad * (p_disc == DiscDistribution::UniformArea ? std::sqrt(u) : u);
	double sr, cr, sa, ca;
	poly_sincos(radius, sr, cr);
	poly_sincos(2.0 * kPiD * double(p_u_angle), sa, ca);
	const Frame f = frame_of(aim);
	const DVec3 off = add(scale(f.left, ca), scale(f.up, sa));
	return to_f(normalize(add(scale(aim, cr), scale(off, sr))));
}

RecoilOffset recoil_after_shot(const RecoilOffset &p_offset, const Recoil &p_recoil, bool p_first, float p_first_shot_multiplier, float p_u_horizontal) {
	RecoilOffset o = p_offset;
	const float up = p_recoil.up_per_shot * (p_first ? p_first_shot_multiplier : 1.0f);
	o.pitch = std::min(o.pitch + up, std::max(p_recoil.up_max, 0.0f));
	const float u = std::clamp(p_u_horizontal, 0.0f, 1.0f);
	const float yaw = p_recoil.horizontal_min + (p_recoil.horizontal_max - p_recoil.horizontal_min) * u;
	const float cap = std::max(p_recoil.horizontal_amplitude_max, 0.0f);
	o.yaw = std::clamp(o.yaw + yaw, -cap, cap);
	return o;
}

RecoilOffset recover_recoil_tick(const RecoilOffset &p_offset, const WeaponSimDef &p_def, const SwayContext &p_context, bool p_firing,
		const Mechanics &p_mechanics) {
	const size_t s = std::min<size_t>(size_t(p_context.stance), 2);
	const size_t z = std::min<size_t>(size_t(p_context.zoom), 1);
	RecoilOffset o = p_offset;
	if (p_mechanics.recovery == RecoveryShape::Exponential) {
		const float k = p_firing ? p_def.recovery_factor_firing[s][z] : p_def.recovery_factor_idle[s][z];
		o.pitch *= k;
		o.yaw *= k;
		return o;
	}
	const float rate = p_def.sway[s][z].recoil.decrease_factor * (p_firing ? p_def.shooting_recoil_decrease_scale : 1.0f);
	const float step = rate / float(kSimHz);
	o.pitch = approach_zero(o.pitch, step);
	o.yaw = approach_zero(o.yaw, step);
	return o;
}

Vec3 aim_direction(uint16_t p_yaw, int16_t p_pitch, const RecoilOffset &p_offset) {
	using namespace detail;
	const double yaw = double(yaw_radians(p_yaw)) + double(p_offset.yaw) * core::kDegToRad;
	constexpr double kMaxPitch = 89.99 * core::kDegToRad;
	const double pitch = std::clamp(double(pitch_radians(p_pitch)) + double(p_offset.pitch) * core::kDegToRad, -kMaxPitch, kMaxPitch);
	double sy, cy, sp, cp;
	poly_sincos(yaw, sy, cy);
	poly_sincos(pitch, sp, cp);
	return to_f(normalize(DVec3{ sy * cp, sp, cy * cp }));
}

}
