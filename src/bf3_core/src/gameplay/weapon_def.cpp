#include "bf3/core/gameplay/weapon_def.h"

#include <algorithm>
#include <cmath>

namespace bf3::core::gameplay {

const char *fire_logic_name(FireLogic p_logic) {
	switch (p_logic) {
		case FireLogic::Single:
			return "fltSingleFire";
		case FireLogic::SingleWithBoltAction:
			return "fltSingleFireWithBoltAction";
		case FireLogic::Automatic:
			return "fltAutomaticFire";
		case FireLogic::Burst:
			return "fltBurstFire";
		case FireLogic::HoldAndRelease:
			return "fltHoldAndRelease";
		case FireLogic::Detonated:
			return "fltDetonatedFiring";
		default:
			return "unknown";
	}
}

const char *reload_type_name(ReloadType p_type) {
	switch (p_type) {
		case ReloadType::SingleBullet:
			return "rtSingleBullet";
		case ReloadType::Magazine:
			return "rtMagazine";
		case ReloadType::MagazineWithPossibleShorterReload:
			return "rtMagazineWithPossibleShorterReload";
		default:
			return "unknown";
	}
}

const char *projectile_kind_name(ProjectileKind p_kind) {
	switch (p_kind) {
		case ProjectileKind::Bullet:
			return "bullet";
		case ProjectileKind::Missile:
			return "missile";
		case ProjectileKind::Grenade:
			return "grenade";
		case ProjectileKind::Other:
			return "other";
		default:
			return "none";
	}
}

FireLogic fire_logic_from(int32_t p_value) {
	return (p_value >= 0 && p_value <= 5) ? FireLogic(p_value) : FireLogic::Unknown;
}

double shot_interval(const FireFunction &p_fire) {
	const double rof = p_fire.rate_of_fire > 0 ? 60.0 / p_fire.rate_of_fire : 0.0;
	if (p_fire.logic == FireLogic::SingleWithBoltAction) {
		return std::max(rof, double(p_fire.bolt_action_delay) + double(p_fire.bolt_action_time));
	}
	return rof;
}

double burst_interval(const FireFunction &p_fire) {
	return p_fire.rate_of_fire_for_burst > 0 ? 60.0 / p_fire.rate_of_fire_for_burst : 0.0;
}

double effective_rpm(const FireFunction &p_fire) {
	const double t = shot_interval(p_fire);
	return t > 0 ? 60.0 / t : 0.0;
}

double muzzle_speed(const WeaponDef &p_weapon) {
	return p_weapon.fire.initial_speed[2];
}

double damage_at(const ProjectileDef &p_projectile, double p_distance) {
	const double a = p_projectile.damage_falloff_start;
	const double b = p_projectile.damage_falloff_end;
	if (p_distance <= a || b <= a) {
		return p_distance <= a ? p_projectile.start_damage : p_projectile.end_damage;
	}
	if (p_distance >= b) {
		return p_projectile.end_damage;
	}
	const double t = (p_distance - a) / (b - a);
	return double(p_projectile.start_damage) + (double(p_projectile.end_damage) - double(p_projectile.start_damage)) * t;
}

double drop_at(double p_speed, double p_gravity, double p_distance) {
	if (p_speed <= 0) {
		return 0;
	}
	const double t = p_distance / p_speed;
	return 0.5 * std::fabs(p_gravity) * t * t;
}

double max_range(const WeaponDef &p_weapon) {
	return muzzle_speed(p_weapon) * double(p_weapon.projectile.time_to_live);
}

uint32_t loc_sid_hash(std::string_view p_sid) {
	uint32_t h = 0xFFFFFFFFu;
	for (const char c : p_sid) {
		h = h * 33u + uint8_t(c);
	}
	return h;
}

void apply_sway_modifier(std::array<std::array<SwayState, 2>, 3> &r_state, const SwayGatedModifier &p_modifier) {
	for (size_t stance = 0; stance < 3; ++stance) {
		for (size_t zoom = 0; zoom < 2; ++zoom) {
			SwayState &s = r_state[stance][zoom];
			const SwayModifier &m = p_modifier.mod[stance][zoom];
			for (Dispersion &d : s.move) {
				d.min_angle *= m.min_angle;
				if (m.max_angle != 0) {
					d.max_angle *= m.max_angle;
				}
				d.increase_per_shot *= m.increase_per_shot;
				d.decrease_per_second *= m.decrease_per_second;
			}
			s.recoil.up_per_shot *= m.recoil_magnitude;
			s.recoil.horizontal_min *= m.recoil_angle;
			s.recoil.horizontal_max *= m.recoil_angle;
		}
	}
}

}
