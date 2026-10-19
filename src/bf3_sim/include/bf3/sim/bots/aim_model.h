#pragma once
// The bot aim model (docs/design/m2b.md §5.3, §2.6 #13; declared in W1.0, owner bb). Pure and Godot-free.
//
// Error cone sigma(t) = e_inf + (e0 - e_inf) exp(-t / tau) from the moment the target was acquired (e0 3 deg, e_inf
// 0.5 deg at skill 0.5, tau 0.6 s), + moving_penalty_deg while the bot moves, + crossing_deg_per_mps per m/s of the
// target's crossing speed. Lead from the weapon's muzzle speed, drop from its gravity (the constants the weapon sim
// uses: ProjectileSimDef::muzzle_speed, gravity). Recoil: the bot compensates skill x 100 % of the kick per shot.
// Never a damage multiplier (research §3.6). Skill per bot from its seed: uniform skill_min .. skill_max (TdmBotConfig).

#include "bf3/sim/sim_types.h"

#include <cstdint>

namespace bf3::sim::bots {

struct AimParams {
	float e0_deg = 3.0f; // at skill 0.5 [inferred, J5]
	float e_inf_deg = 0.5f; // at skill 0.5
	float tau_s = 0.6f;
	float moving_penalty_deg = 1.5f; // while the bot itself moves
	float crossing_deg_per_mps = 0.5f; // per m/s of the target's speed across the line of sight
	float skill_scale = 1.0f; // how the cone scales with skill (bb's formula; 1 = as stated above at skill 0.5)
};

// The cone's standard deviation in degrees.
float aim_error(const AimParams &p_params, float p_skill, float p_seconds_on_target, bool p_moving, float p_crossing_speed_mps);

// Where to aim so a projectile from p_origin with p_muzzle_speed (m/s) and p_gravity (m/s^2 along world Y, negative =
// down) meets a target at p_target moving at p_target_velocity. Instant-hit weapons (or speed <= 0): p_target.
Vec3 lead_point(const Vec3 &p_origin, const Vec3 &p_target, const Vec3 &p_target_velocity, float p_muzzle_speed, float p_gravity);

} // namespace bf3::sim::bots
