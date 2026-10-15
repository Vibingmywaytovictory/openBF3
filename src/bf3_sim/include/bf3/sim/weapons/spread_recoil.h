#pragma once
// Spread (deviation) and recoil from GunSwayData as swappable functions (docs/design/m2a-weapons.md §5.6;
// plan §3.2, D7, R5; topics/01 §2.1, §2.2, §5.2).
//
// The model as the data and Symthic's tutorial describe it (structure [verified], semantics marked):
//   * spread: a cone half-angle that starts at the state's MinAngle (stance x zoom x {base, moving, jumping,
//     sprinting, vaulting}), grows by IncreasePerShot after each shot up to MaxAngle and decays at
//     DecreasePerSecond back to the minimum; suppression multiplies it by SuppressionModifier* (MaxAngle 0 = unset);
//     a gated attachment modifier (bipod, laser) multiplies while its gate holds;
//   * recoil: a view offset (pitch up, yaw; + = left, topics/01 C7): per shot pitch += RecoilAmplitudeIncPerShot
//     (x FirstShotRecoilMultiplier on the first shot), yaw += a uniform draw in [HorizontalMin, HorizontalMax],
//     each capped by its amplitude max; recovery by RecoilAmplitudeDecreaseFactor (x ShootingRecoilDecreaseScale
//     while firing) in the shape Mechanics::recovery names.
// The aim of a shot is the command's view + the recoil offset (the camera shows the same offset from the
// predicted weapon state, so the Command keeps carrying the player's own view, §5.6).
// Every function is pure and float-only with no libm call on the replicated path: the disc's azimuth uses the
// polynomial sin/cos of src/weapons/spread_recoil.cpp (identical on every machine, plan R4); exp() factors are
// precomputed in the def.

#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/sim/weapons/mechanics.h"
#include "bf3/sim/weapons/sim_time.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <cstdint>

namespace bf3::sim::weapons {

using core::gameplay::Stance;
using core::gameplay::Zoom;

// The soldier's state the sway tables are indexed by (from the motor's state after the tick, §5.2).
struct SwayContext {
	Stance stance = Stance::Stand;
	Zoom zoom = Zoom::NoZoom;
	SwayMove move = SwayMove::Base; // crouch / prone know Base and Moving only (others fall back to Base)
	bool suppressed = false; // suppression level >= Mechanics::suppression_threshold
	bool supported = false; // bipod deployed (no supported-shooting input in M2a: always false)
	bool light_on = false; // laser / light switched on

	constexpr bool operator==(const SwayContext &) const = default;
};

// The dispersion and recoil in effect: the table entry x the suppression modifier x every gated modifier whose
// gate holds (the same multiplication rule as gameplay::apply_sway_modifier).
Dispersion effective_dispersion(const WeaponSimDef &p_def, const SwayContext &p_context);
Recoil effective_recoil(const WeaponSimDef &p_def, const SwayContext &p_context);

// Spread after p_elapsed units of decay from p_spread (never below the minimum, never above the maximum).
float decay_spread(float p_spread, const Dispersion &p_dispersion, SimTime p_elapsed);
// The spread a state change starts from (Mechanics::min_spread_rise; Immediate: max(p_spread, new minimum)).
float enter_spread(float p_spread, const Dispersion &p_new, const Mechanics &p_mechanics);
// After a shot: min(p_spread + IncreasePerShot, MaxAngle).
float spread_after_shot(float p_spread, const Dispersion &p_dispersion);

// A direction inside the cone of half-angle p_spread_degrees around p_aim (unit vector), from two uniforms.
// The disc radius follows p_disc; the azimuth is 2 pi x p_u_angle. The result is a unit vector.
Vec3 sample_cone(const Vec3 &p_aim, float p_spread_degrees, float p_u_radius, float p_u_angle, DiscDistribution p_disc);

struct RecoilOffset {
	float pitch = 0; // degrees, + = up
	float yaw = 0; // degrees, + = left (data sign: horizontal Max = left)

	constexpr bool operator==(const RecoilOffset &) const = default;
};
// One shot's kick: p_first applies FirstShotRecoilMultiplier; p_u_horizontal places the yaw kick in [min, max].
RecoilOffset recoil_after_shot(const RecoilOffset &p_offset, const Recoil &p_recoil, bool p_first, float p_first_shot_multiplier,
		float p_u_horizontal);
// One whole tick of recovery toward 0 (Linear: by factor / kSimHz degrees per axis, scaled while firing;
// Exponential: x the def's precomputed per-tick factor).
RecoilOffset recover_recoil_tick(const RecoilOffset &p_offset, const WeaponSimDef &p_def, const SwayContext &p_context, bool p_firing,
		const Mechanics &p_mechanics);

// The aim direction of the view (yaw, pitch in Command steps) turned by a recoil offset (degrees): the unit
// vector (sin y cos p, sin p, cos y cos p) of command.h with y = view yaw + offset.yaw, p = view pitch + offset.pitch
// (clamped to the pitch limit).
Vec3 aim_direction(uint16_t p_yaw, int16_t p_pitch, const RecoilOffset &p_offset);

} // namespace bf3::sim::weapons
