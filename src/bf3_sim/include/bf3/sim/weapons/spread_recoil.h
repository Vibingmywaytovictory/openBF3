#pragma once

#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/sim/weapons/mechanics.h"
#include "bf3/sim/weapons/sim_time.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <cstdint>

namespace bf3::sim::weapons {

using core::gameplay::Stance;
using core::gameplay::Zoom;

struct SwayContext {
	Stance stance = Stance::Stand;
	Zoom zoom = Zoom::NoZoom;
	SwayMove move = SwayMove::Base;
	bool suppressed = false;
	bool supported = false;
	bool light_on = false;

	constexpr bool operator==(const SwayContext &) const = default;
};

Dispersion effective_dispersion(const WeaponSimDef &p_def, const SwayContext &p_context);
Recoil effective_recoil(const WeaponSimDef &p_def, const SwayContext &p_context);

float decay_spread(float p_spread, const Dispersion &p_dispersion, SimTime p_elapsed);

float enter_spread(float p_spread, const Dispersion &p_new, const Mechanics &p_mechanics);

float spread_after_shot(float p_spread, const Dispersion &p_dispersion);

Vec3 sample_cone(const Vec3 &p_aim, float p_spread_degrees, float p_u_radius, float p_u_angle, DiscDistribution p_disc);

struct RecoilOffset {
	float pitch = 0;
	float yaw = 0;

	constexpr bool operator==(const RecoilOffset &) const = default;
};

RecoilOffset recoil_after_shot(const RecoilOffset &p_offset, const Recoil &p_recoil, bool p_first, float p_first_shot_multiplier,
		float p_u_horizontal);

RecoilOffset recover_recoil_tick(const RecoilOffset &p_offset, const WeaponSimDef &p_def, const SwayContext &p_context, bool p_firing,
		const Mechanics &p_mechanics);

Vec3 aim_direction(uint16_t p_yaw, int16_t p_pitch, const RecoilOffset &p_offset);

}
