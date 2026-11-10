#include "bf3/sim/weapons/spread_recoil.h"

#include "bf3/sim/command.h"
#include "bf3/sim/weapons/suppression.h"
#include "bf3/sim/weapons/weapon_rng.h"
#include "weapons_internal.h"

#include <algorithm>
#include <cmath>

namespace bf3::sim::weapons {

namespace {

constexpr double kSwayPi = double(3.14159265358979323846f);
constexpr double kDeg = core::kDegToRad;

size_t stance_of(const SwayContext &p_context) {
	return std::min<size_t>(size_t(p_context.stance), 2);
}
size_t zoom_of(const SwayContext &p_context) {
	return std::min<size_t>(size_t(p_context.zoom), 1);
}

bool gate_open(const WeaponSimDef::Conditional &p_c, const SwayContext &p_context) {
	return (!p_c.only_supported || p_context.supported) && (!p_c.only_light || p_context.light_on) &&
			(p_c.only_supported || p_c.only_light);
}

SwayModifier attachment_product(const WeaponSimDef &p_def, const SwayContext &p_context) {
	SwayModifier m;
	for (const WeaponSimDef::Conditional &c : p_def.conditional) {
		if (!gate_open(c, p_context)) {
			continue;
		}
		const SwayModifier &a = c.mod[stance_of(p_context)][zoom_of(p_context)];
		m.min_angle *= a.min_angle;
		if (a.max_angle != 0) {
			m.max_angle *= a.max_angle;
		}
		m.increase_per_shot *= a.increase_per_shot;
		m.decrease_per_second *= a.decrease_per_second;
		m.recoil_magnitude *= a.recoil_magnitude;
		m.recoil_angle *= a.recoil_angle;
	}
	return m;
}

Dispersion with_modifiers(const Dispersion &p_d, const WeaponSimDef &p_def, const SwayContext &p_context) {
	const Dispersion s =
			suppressed_dispersion(p_d, p_def.suppression_modifier[zoom_of(p_context)], p_context.suppression);
	const SwayModifier a = attachment_product(p_def, p_context);
	Dispersion d;
	d.min_angle = s.min_angle * a.min_angle;
	d.max_angle = s.max_angle * a.max_angle;
	d.increase_per_shot = s.increase_per_shot * a.increase_per_shot;
	d.decrease_per_second = s.decrease_per_second * a.decrease_per_second;
	return d;
}

const SwayState &state_of(const WeaponSimDef &p_def, size_t p_stance, size_t p_zoom) {
	return p_def.sway[p_stance][p_zoom];
}

double toward_zero(double p_value, double p_step) {
	if (p_value <= 0) {
		const double v = p_value + p_step;
		return v > 0 ? 0.0 : v;
	}
	const double v = p_value - p_step;
	return v < 0 ? 0.0 : v;
}

void spring_step(float &r_position, float &r_rate, double p_accumulator, double p_threshold, double p_constant,
		double p_damping) {
	const double target = std::fabs(p_accumulator) > p_threshold ? p_accumulator : 0.0;
	double w = double(r_rate) + p_constant * (target - double(r_position)) * double(kSwaySeconds);
	w *= 1.0 - p_damping;
	r_rate = float(w);
	r_position = float(double(r_position) + w * double(kSwaySeconds));
}

DVec3 rotate_x(const DVec3 &p_v, double p_angle) {
	double s, c;
	detail::poly_sincos(p_angle, s, c);
	return { p_v[0], p_v[1] * c - p_v[2] * s, p_v[1] * s + p_v[2] * c };
}
DVec3 rotate_y(const DVec3 &p_v, double p_angle) {
	double s, c;
	detail::poly_sincos(p_angle, s, c);
	return { p_v[0] * c + p_v[2] * s, p_v[1], -p_v[0] * s + p_v[2] * c };
}

DVec3 sway_rotate_d(const DVec3 &p_v, double p_pitch, double p_yaw) {
	return rotate_x(rotate_y(p_v, p_yaw), p_pitch);
}

}

std::optional<SwayRecord> sway_record(const WeaponSimDef &p_def, const SwayContext &p_context, float p_since_air) {
	const size_t s = stance_of(p_context);
	const size_t z = zoom_of(p_context);
	if (p_context.move == SwayMove::Sprinting) {
		const SwayState &st = state_of(p_def, 0, 0);
		const size_t m = size_t(SwayMove::Sprinting);
		return st.has_move[m] ? std::optional<SwayRecord>(SwayRecord{ st.move[m], false }) : std::nullopt;
	}
	if (p_context.move == SwayMove::Jumping || p_since_air < kSwayAirHold) {
		const SwayState &st = state_of(p_def, 0, z);
		const size_t m = size_t(SwayMove::Jumping);
		if (s != 0 || !st.has_move[m]) {
			return std::nullopt;
		}
		return SwayRecord{ st.move[m], false };
	}
	const SwayState &st = state_of(p_def, s, z);
	const Dispersion &base = st.move[size_t(SwayMove::Base)];
	if (p_context.move == SwayMove::Moving && st.has_move[size_t(SwayMove::Moving)]) {
		Dispersion d = st.move[size_t(SwayMove::Moving)];
		const float f = std::clamp(p_context.move_fraction, 0.0f, 1.0f);
		d.min_angle = base.min_angle + (d.min_angle - base.min_angle) * f;
		return SwayRecord{ with_modifiers(d, p_def, p_context), true };
	}
	return SwayRecord{ with_modifiers(base, p_def, p_context), true };
}

Dispersion effective_dispersion(const WeaponSimDef &p_def, const SwayContext &p_context) {
	const std::optional<SwayRecord> r = sway_record(p_def, p_context, kSwayAirHold);
	if (r) {
		return r->dispersion;
	}
	SwayContext still = p_context;
	still.move = SwayMove::Base;
	return sway_record(p_def, still, kSwayAirHold)->dispersion;
}

void sway_fire_start(GunSwayState &r_sway) {
	r_sway.flags = uint8_t(r_sway.flags | sway_flags::kTriggerHeld);
	r_sway.shots = 0;
}

void sway_fire_stop(GunSwayState &r_sway) {
	r_sway.flags = uint8_t(r_sway.flags & ~sway_flags::kTriggerHeld);
	r_sway.shots = 0;
}

void sway_shot(GunSwayState &r_sway, uint32_t p_seed) {
	r_sway.flags = uint8_t(r_sway.flags | sway_flags::kFired);
	if (r_sway.shots < kSwayShotCountCap) {
		++r_sway.shots;
	}
	if (p_seed == r_sway.last_seed) {
		return;
	}
	r_sway.last_seed = p_seed;
	r_sway.rng = sway_rng_seeded(p_seed);
	const double theta = sway_uniform(r_sway.rng, -kSwayPi, kSwayPi);
	const double radius = sway_uniform(r_sway.rng, 0.0, double(r_sway.dispersion) * kDeg);
	double s, c;
	detail::poly_sincos(theta, s, c);
	r_sway.deviation_pitch = float(s * radius);
	r_sway.deviation_yaw = float(c * radius);
}

void sway_update(GunSwayState &r_sway, const WeaponSimDef &p_def, const SwayContext &p_context) {
	const double dt = double(kSwaySeconds);
	const bool fired = (r_sway.flags & sway_flags::kFired) != 0;
	const bool held = (r_sway.flags & sway_flags::kTriggerHeld) != 0;
	const size_t s = stance_of(p_context);
	const size_t z = zoom_of(p_context);

	r_sway.since_air =
			p_context.move == SwayMove::Jumping ? 0.0f : std::min(kSwayAirHold, r_sway.since_air + kSwaySeconds);
	if (const std::optional<SwayRecord> rec = sway_record(p_def, p_context, r_sway.since_air)) {
		const Dispersion &d = rec->dispersion;
		if (fired && r_sway.dispersion < d.max_angle) {
			r_sway.dispersion = r_sway.dispersion + d.increase_per_shot;
		}
		if (!held) {
			r_sway.dispersion =
					std::max(float(double(r_sway.dispersion) - double(d.decrease_per_second) * dt), d.min_angle);
		}
	}

	const Recoil &rec = state_of(p_def, s, z).recoil;
	double pitch = r_sway.recoil_pitch;
	double yaw = r_sway.recoil_yaw;
	if (fired) {
		const float l = p_context.suppression > 0.0f ? p_context.suppression : 0.0f;
		const SwayModifier a = attachment_product(p_def, p_context);
		const SwayModifier &sm = p_def.suppression_modifier[z];
		const double mag_v = double(a.recoil_magnitude) + double(sm.recoil_magnitude) * double(l);
		const double mag_h = double(a.recoil_angle) + double(sm.recoil_angle) * double(l);
		const double first = r_sway.shots == kSwayFirstShotCount ? double(p_def.first_shot_recoil_multiplier) : 1.0;
		const double h = sway_uniform(r_sway.rng, double(rec.horizontal_min) * kDeg, double(rec.horizontal_max) * kDeg);
		const double inc = double(rec.up_per_shot) * kDeg;
		const double v = sway_uniform(r_sway.rng, -double(kSwayVerticalLow) * inc, -inc);
		yaw += h * mag_h;
		pitch += v * mag_v * first;
		const double h_max = std::fabs(double(rec.horizontal_amplitude_max)) * kDeg;
		const double v_max = std::fabs(double(rec.up_max)) * kDeg;
		yaw = std::clamp(yaw, -h_max, h_max);
		pitch = std::clamp(pitch, -v_max, v_max);
		r_sway.since_shot = 0;
	} else {
		r_sway.since_shot = float(double(r_sway.since_shot) + dt);
	}
	const double t = r_sway.since_shot;
	double dec = double(rec.decrease_factor) * t * t * kSwayRecoveryScale;
	if (held) {
		dec *= double(p_def.shooting_recoil_decrease_scale);
	}
	pitch = toward_zero(pitch, dec);
	yaw = toward_zero(yaw, dec);
	r_sway.recoil_pitch = float(pitch);
	r_sway.recoil_yaw = float(yaw);

	if (p_context.zoom == Zoom::NoZoom) {
		const double threshold = double(p_def.camera_spring_min_threshold_angle) * kDeg;
		const double k = p_def.camera_spring_constant;
		const double damping = p_def.camera_spring_damping;
		spring_step(r_sway.spring_yaw, r_sway.spring_yaw_rate, yaw, threshold, k, damping);
		spring_step(r_sway.spring_pitch, r_sway.spring_pitch_rate, pitch, threshold, k, damping);
	}
	r_sway.flags = uint8_t(r_sway.flags & ~sway_flags::kFired);
}

float gameplay_deviation_scale(const WeaponSimDef &p_def, Zoom p_zoom) {
	return p_def.gameplay_deviation_scale[std::min<size_t>(size_t(p_zoom), 1)];
}

RecoilOffset gameplay_recoil(const WeaponSimDef &p_def, const GunSwayState &p_sway, Zoom p_zoom) {
	const double g = gameplay_deviation_scale(p_def, p_zoom);
	return { float(-double(p_sway.recoil_pitch) * g / kDeg), float(double(p_sway.recoil_yaw) * g / kDeg) };
}

RecoilOffset camera_recoil(const WeaponSimDef &p_def, const GunSwayState &p_sway, Zoom p_zoom) {
	const size_t z = std::min<size_t>(size_t(p_zoom), 1);
	const double k = p_def.deviation_scale[z];
	if (p_zoom == Zoom::Zoom) {
		return { float(-double(p_sway.recoil_pitch) * k / kDeg), float(double(p_sway.recoil_yaw) * k / kDeg) };
	}
	return { float(-double(p_sway.spring_pitch) * k / kDeg), float(double(p_sway.spring_yaw) * k / kDeg) };
}

Vec3 sway_rotate(const Vec3 &p_local, float p_pitch, float p_yaw) {
	return detail::to_f(sway_rotate_d(detail::to_d(p_local), p_pitch, p_yaw));
}

Vec3 shoot_direction(uint16_t p_yaw, int16_t p_pitch, const WeaponSimDef &p_def, const GunSwayState &p_sway,
		Zoom p_zoom, bool p_dispersion) {
	const double g = gameplay_deviation_scale(p_def, p_zoom);
	DVec3 v{ 0, 0, 1 };
	if (p_dispersion) {
		v = sway_rotate_d(v, double(p_sway.deviation_pitch) * g, double(p_sway.deviation_yaw) * g);
	}
	v = sway_rotate_d(v, double(p_sway.recoil_pitch) * g, double(p_sway.recoil_yaw) * g);
	v = rotate_y(rotate_x(v, -double(pitch_radians(p_pitch))), double(yaw_radians(p_yaw)));
	return detail::to_f(detail::normalize(v));
}

Vec3 sample_cone(const Vec3 &p_aim, float p_spread_degrees, float p_u_radius, float p_u_angle) {
	using namespace detail;
	const DVec3 aim = normalize(to_d(p_aim));
	if (!(p_spread_degrees > 0)) {
		return to_f(aim);
	}
	const double u = std::clamp(double(p_u_radius), 0.0, 1.0);
	const double radius = double(p_spread_degrees) * core::kDegToRad * std::sqrt(u);
	double sr, cr, sa, ca;
	poly_sincos(radius, sr, cr);
	poly_sincos(2.0 * kPiD * double(p_u_angle), sa, ca);
	const Frame f = frame_of(aim);
	const DVec3 off = add(scale(f.left, ca), scale(f.up, sa));
	return to_f(normalize(add(scale(aim, cr), scale(off, sr))));
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

float sway_carry_at(SimTime p_time) {
	return float(double(sway_tick_end(sway_tick(p_time)) - p_time) / double(kTimeUnitsPerSecond));
}

SwayWait sway_wait(SimTime p_from, float p_carry, float p_seconds) {
	uint32_t k = sway_tick(p_from);
	float timer = p_seconds;
	float rem = p_carry;
	const uint32_t limit = k + uint32_t(kMaxDefTime / kSwayUnits) + 2u;
	while (k < limit) {
		if (rem > 0.0f) {
			if (timer > 0.0f) {
				const float t = timer - rem;
				if (t >= 0.0f) {
					timer = t;
					rem = 0.0f;
				} else {
					timer = 0.0f;
					rem = -t;
				}
			}
			if (!(timer > 0.0f) && rem > 0.0f) {
				const SimTime units = std::clamp<SimTime>(
						SimTime(std::llround(double(rem) * double(kTimeUnitsPerSecond))), 1, kSwayUnits);
				return { sway_tick_end(k) - units, rem };
			}
		}
		++k;
		rem = kSwaySeconds;
	}
	return { sway_tick_start(limit), kSwaySeconds };
}
}
