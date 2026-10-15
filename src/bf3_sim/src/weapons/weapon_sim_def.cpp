// WeaponSimDef from a resolved WeaponDef, the WeaponTable and its hash (docs/design/m2a-weapons.md §5.3, §5.5, §9).
// Every number the simulation runs on is checked here once, so the step functions never see NaN, a zero interval,
// an endless burst or an absurd count: a bad value fails with an error naming the variant and the field.
#include "bf3/sim/weapons/weapon_sim_def.h"

#include "bf3/sim/weapons/surface_materials.h"
#include "weapons_internal.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string>
#include <system_error>

namespace bf3::sim::weapons {

namespace {

using core::gameplay::FireFunction;
using core::gameplay::ProjectileDef;
using core::gameplay::WeaponDef;

// The smallest interval a def may have: kMaxShotsPerTick shots per tick (28,800 RPM at 60 Hz).
constexpr SimTime kMinInterval = kTimeUnitsPerTick / kMaxShotsPerTick;
constexpr float kMaxDistance = 100'000.0f; // m: falloff distances
constexpr float kMaxFactor = 10'000.0f; // dimensionless multipliers (sway modifiers, recoil scales)
constexpr float kMaxOffset = kMaxRadius; // m: ShotConfigData.InitialPosition components

// The decimal the data's author wrote for a float: the shortest decimal that reads back as the same float
// (std::to_chars, an exactly specified algorithm, so every machine gets the same digits), read back as a double.
// The data's 1.8 s is the float 1.79999995..., 107,999,997 units when converted as it stands; its shortest decimal is
// "1.8", exactly 108,000,000 units, the number the plan's oracles quote (1.3 s bolt = 78,000,000, 0.48 s pump =
// 28,800,000, 3.6 s fuse = 216,000,000). Every data time, rate and fraction goes through this once, in the def
// build; the resulting integers are what the simulation adds and hashes.
double data_decimal(float p_value) {
	if (!std::isfinite(p_value)) {
		return double(p_value);
	}
	char buf[48];
	const std::to_chars_result w = std::to_chars(buf, buf + sizeof(buf), p_value);
	if (w.ec != std::errc()) {
		return double(p_value);
	}
	double out = 0;
	const std::from_chars_result r = std::from_chars(buf, w.ptr, out);
	return r.ec == std::errc() && r.ptr == w.ptr ? out : double(p_value);
}

// Collects the first violated bound as "<variant>: <field> <what>".
class Checker {
public:
	explicit Checker(std::string p_variant) :
			variant_(std::move(p_variant)) {}

	bool ok() const { return error_.empty(); }
	core::Error error() const { return core::make_error(error_); }
	void fail(const std::string &p_field, const std::string &p_what) {
		if (error_.empty()) {
			error_ = (variant_.empty() ? std::string("(no variant)") : variant_) + ": " + p_field + " " + p_what;
		}
	}
	// A finite value in [p_lo, p_hi].
	float range(float p_value, float p_lo, float p_hi, const std::string &p_field) {
		if (!std::isfinite(p_value)) {
			fail(p_field, "is not finite");
			return p_lo;
		}
		if (p_value < p_lo || p_value > p_hi) {
			fail(p_field, "= " + std::to_string(p_value) + " is outside [" + std::to_string(p_lo) + ", " + std::to_string(p_hi) + "]");
			return p_lo;
		}
		return p_value;
	}
	float finite(float p_value, const std::string &p_field) {
		if (!std::isfinite(p_value)) {
			fail(p_field, "is not finite");
			return 0;
		}
		return p_value;
	}
	// Seconds of data in [0, 60] -> units.
	SimTime time(float p_seconds, const std::string &p_field) {
		return seconds_to_units(data_decimal(range(p_seconds, 0.0f, float(units_to_seconds(kMaxDefTime)), p_field)));
	}
	// A time that saturates at 60 s instead of being refused (HoldAndReleaseData.MaxHoldTime: the M67's 9999 means
	// "no limit").
	SimTime time_saturating(float p_seconds, const std::string &p_field) {
		if (!std::isfinite(p_seconds) || p_seconds < 0) {
			fail(p_field, std::isfinite(p_seconds) ? "is negative" : "is not finite");
			return 0;
		}
		return seconds_to_units(data_decimal(std::min(p_seconds, float(units_to_seconds(kMaxDefTime)))));
	}
	float angle(float p_degrees, const std::string &p_field) { return range(p_degrees, 0.0f, kMaxAngle, p_field); }
	float signed_angle(float p_degrees, const std::string &p_field) { return range(p_degrees, -kMaxAngle, kMaxAngle, p_field); }
	float rate(float p_value, const std::string &p_field) { return range(p_value, 0.0f, kMaxRate, p_field); }
	float factor(float p_value, const std::string &p_field) { return range(p_value, 0.0f, kMaxFactor, p_field); }
	void dispersion(const Dispersion &p_d, const std::string &p_field) {
		angle(p_d.min_angle, p_field + ".MinAngle");
		angle(p_d.max_angle, p_field + ".MaxAngle");
		angle(p_d.increase_per_shot, p_field + ".IncreasePerShot");
		rate(p_d.decrease_per_second, p_field + ".DecreasePerSecond");
	}
	void modifier(const SwayModifier &p_m, const std::string &p_field) {
		factor(p_m.min_angle, p_field + ".MinAngleModifier");
		factor(p_m.max_angle, p_field + ".MaxAngleModifier");
		factor(p_m.increase_per_shot, p_field + ".IncreasePerShotModifier");
		factor(p_m.decrease_per_second, p_field + ".DecreasePerSecondModifier");
		factor(p_m.recoil_magnitude, p_field + ".RecoilMagnitudeModifier");
		factor(p_m.recoil_angle, p_field + ".RecoilAngleModifier");
		finite(p_m.lag_yaw, p_field + ".LagYawModifier");
		finite(p_m.lag_pitch, p_field + ".LagPitchModifier");
	}

private:
	std::string variant_;
	std::string error_;
};

const char *kStanceName[3] = { "stand", "crouch", "prone" };
const char *kZoomName[2] = { "hip", "ads" };
const char *kMoveName[size_t(SwayMove::Count)] = { "base", "moving", "jumping", "sprinting", "vault small", "vault medium" };

ExplosionSimDef make_explosion(Checker &r_c, const core::gameplay::Explosion &p_e, const SurfaceMaterialTable &p_materials, const std::string &p_field) {
	ExplosionSimDef x;
	if (!p_e.present) {
		return x;
	}
	x.present = true;
	x.blast_damage = r_c.range(p_e.blast_damage, 0.0f, kMaxDamage, p_field + ".BlastDamage");
	x.inner_radius = r_c.range(p_e.inner_blast_radius, 0.0f, kMaxRadius, p_field + ".InnerBlastRadius");
	x.radius = r_c.range(p_e.blast_radius, 0.0f, kMaxRadius, p_field + ".BlastRadius");
	x.impulse = r_c.range(p_e.blast_impulse, 0.0f, 1e9f, p_field + ".BlastImpulse");
	x.max_occlusion_ray_radius = r_c.range(p_e.max_occlusion_raycast_radius, 0.0f, kMaxRadius, p_field + ".MaxOcclusionRaycastRadius");
	x.disable_occlusion = p_e.disable_occlusion;
	x.disable_static_entity_occlusion = p_e.disable_static_entity_occlusion;
	if (p_e.material.present) {
		const SurfacePair pair = p_materials.pair_of(p_e.material);
		if (pair.resolved) {
			for (size_t z = 0; z < kHitZoneCount; ++z) {
				const core::gameplay::DamageRelation rel = p_materials.relation(pair.property, p_materials.zone_property(HitZone(z)));
				x.zone_multiplier[z] = rel.present ? r_c.range(rel.protection_multiplier, 0.0f, kMaxFactor, p_field + " zone cell") : 1.0f;
			}
			x.zone_multiplier_from_data = true;
		}
	}
	return x;
}

ProjectileSimDef make_projectile(Checker &r_c, const WeaponDef &p_def, const SurfaceMaterialTable &p_materials) {
	const FireFunction &f = p_def.fire;
	const ProjectileDef &p = p_def.projectile;
	ProjectileSimDef out;
	out.kind = p.kind;
	for (size_t i = 0; i < 3; ++i) {
		out.launch_velocity[i] = r_c.range(f.initial_speed[i], -kMaxSpeed, kMaxSpeed, "ShotConfigData.InitialSpeed");
		out.launch_offset[i] = r_c.range(f.initial_position[i], -kMaxOffset, kMaxOffset, "ShotConfigData.InitialPosition");
	}
	const double speed = detail::length(detail::to_d(out.launch_velocity));
	if (speed > double(kMaxSpeed)) {
		r_c.fail("ShotConfigData.InitialSpeed", "length " + std::to_string(speed) + " is above " + std::to_string(kMaxSpeed) + " m/s");
	}
	out.muzzle_speed = float(speed);
	out.gravity = r_c.range(p.gravity, -kMaxGravity, kMaxGravity, "ProjectileData.Gravity");
	const SimTime ttl = r_c.time(p.time_to_live, "ProjectileData.TimeToLive");
	out.time_to_live = ttl > 0 ? ttl : kMaxFlightTime;
	out.instant_hit = p.instant_hit;
	out.start_damage = r_c.range(p.start_damage, 0.0f, kMaxDamage, "BulletEntityData.StartDamage");
	out.end_damage = r_c.range(p.end_damage, 0.0f, kMaxDamage, "BulletEntityData.EndDamage");
	out.falloff_start = r_c.range(p.damage_falloff_start, 0.0f, kMaxDistance, "BulletEntityData.DamageFalloffStartDistance");
	out.falloff_end = r_c.range(p.damage_falloff_end, 0.0f, kMaxDistance, "BulletEntityData.DamageFalloffEndDistance");
	if (p.kind == ProjectileKind::Missile) {
		out.time_to_arm = r_c.time(p.time_to_arm, "MissileEntityData.TimeToArm");
		out.direct_damage = r_c.range(p.missile_damage, 0.0f, kMaxDamage, "MissileEntityData.Damage");
		out.engine_acceleration = r_c.range(p.engine_strength, 0.0f, kMaxRate, "MissileEntityData.EngineStrength");
		out.max_speed = r_c.range(p.max_speed, 0.0f, kMaxSpeed, "MissileEntityData.MaxSpeed");
		out.engine_ignition = r_c.time(p.engine_time_to_ignition, "MissileEntityData.EngineTimeToIgnition");
		out.engine_time_to_live = r_c.time(p.engine_time_to_live, "MissileEntityData.EngineTimeToLive");
		out.drag = r_c.range(p.drag, 0.0f, kMaxRate, "MissileEntityData.Drag");
	} else {
		out.time_to_arm = r_c.time(p.time_to_arm_explosion, "BulletEntityData.TimeToArmExplosion");
	}
	out.detonate_on_timeout = p.detonate_on_timeout;
	if (p.kind == ProjectileKind::Grenade) {
		out.bounce_speed_multiplier = r_c.range(p.collision_speed_multiplier, 0.0f, 10.0f, "GrenadeEntityData.CollisionSpeedMultiplier");
		out.min_bounce_speed = r_c.range(p.min_bounce_speed, 0.0f, kMaxSpeed, "GrenadeEntityData.MinBounceSpeed");
	}
	const SurfacePair pair = p_materials.pair_of(p.material);
	out.property = pair.property;
	out.physics_material = pair.physics_material;
	for (size_t z = 0; z < kHitZoneCount; ++z) {
		out.zone[z] = p_materials.relation(pair.property, p_materials.zone_property(HitZone(z)));
		if (out.zone[z].present) {
			r_c.range(out.zone[z].protection_multiplier, 0.0f, kMaxFactor, "zone cell DamageProtectionMultiplier");
			r_c.range(out.zone[z].penetration_multiplier, 0.0f, kMaxFactor, "zone cell DamagePenetrationMultiplier");
		}
	}
	// Whether every penetration cell of the projectile's row says NeverPenetrate (a fast path for buckshot,
	// melee and explosives; false when the row has no penetration data at all).
	if (pair.resolved && p_materials.has_grid()) {
		bool any = false;
		bool all_never = true;
		for (uint32_t t = 0; t < 256; ++t) {
			const core::gameplay::DamageRelation rel = p_materials.relation(pair.property, uint8_t(t));
			if (rel.has_penetration) {
				any = true;
				all_never = all_never && rel.never_penetrate;
			}
		}
		out.never_penetrate_any = any && all_never;
	}
	out.explosion = make_explosion(r_c, p.explosion, p_materials, "Explosion");
	out.dud = make_explosion(r_c, p.dud_explosion, p_materials, "DudExplosion");
	if (p.suppression.present) {
		out.suppression_min = r_c.range(p.suppression.min_multiplier, 0.0f, kMaxFactor, "WeaponSuppressionData.MinMultiplier");
		out.suppression_max = r_c.range(p.suppression.max_multiplier, 0.0f, kMaxFactor, "WeaponSuppressionData.MaxMultiplier");
		out.suppression_min_distance = r_c.range(p.suppression.min_distance, 0.0f, kMaxDistance, "WeaponSuppressionData.MinDistance");
		out.suppression_max_distance = r_c.range(p.suppression.max_distance, 0.0f, kMaxDistance, "WeaponSuppressionData.MaxDistance");
	}
	return out;
}

float exp_factor(float p_rate_per_second) {
	// exp(-rate / kSimHz): computed once here with libm and stored in the def (hashed), never per tick (plan R4).
	return float(std::exp(-double(p_rate_per_second) / double(kSimHz)));
}

void hash_dispersion(detail::Fnv64 &r_h, const Dispersion &p_d) {
	r_h.add(p_d.min_angle);
	r_h.add(p_d.max_angle);
	r_h.add(p_d.increase_per_shot);
	r_h.add(p_d.decrease_per_second);
}
void hash_recoil(detail::Fnv64 &r_h, const Recoil &p_r) {
	r_h.add(p_r.up_per_shot);
	r_h.add(p_r.up_max);
	r_h.add(p_r.horizontal_min);
	r_h.add(p_r.horizontal_max);
	r_h.add(p_r.horizontal_amplitude_max);
	r_h.add(p_r.decrease_factor);
}
void hash_modifier(detail::Fnv64 &r_h, const SwayModifier &p_m) {
	r_h.add(p_m.min_angle);
	r_h.add(p_m.max_angle);
	r_h.add(p_m.increase_per_shot);
	r_h.add(p_m.decrease_per_second);
	r_h.add(p_m.recoil_magnitude);
	r_h.add(p_m.recoil_angle);
	r_h.add(p_m.lag_yaw);
	r_h.add(p_m.lag_pitch);
}
void hash_relation(detail::Fnv64 &r_h, const core::gameplay::DamageRelation &p_r) {
	r_h.add(p_r.present);
	r_h.add(p_r.protection_multiplier);
	r_h.add(p_r.protection_threshold);
	r_h.add(p_r.penetration_multiplier);
	r_h.add(p_r.collision_multiplier);
	r_h.add(p_r.collision_threshold);
	r_h.add(p_r.explosion_cover_modifier);
	r_h.add(p_r.inflicts_demolition);
	r_h.add(p_r.has_penetration);
	r_h.add(p_r.never_penetrate);
}
void hash_explosion(detail::Fnv64 &r_h, const ExplosionSimDef &p_e) {
	r_h.add(p_e.present);
	r_h.add(p_e.blast_damage);
	r_h.add(p_e.inner_radius);
	r_h.add(p_e.radius);
	r_h.add(p_e.impulse);
	r_h.add(p_e.max_occlusion_ray_radius);
	r_h.add(p_e.disable_occlusion);
	r_h.add(p_e.disable_static_entity_occlusion);
	for (const float m : p_e.zone_multiplier) {
		r_h.add(m);
	}
	r_h.add(p_e.zone_multiplier_from_data);
}

void hash_def(detail::Fnv64 &r_h, const WeaponSimDef &p_d) {
	r_h.add_str(p_d.variant);
	r_h.add(p_d.unlock_identifier);
	r_h.add_str(p_d.weapon_class);
	for (const FireModeDef &m : p_d.modes) {
		r_h.add(m.logic);
		r_h.add(m.interval);
		r_h.add(m.burst_interval);
		r_h.add(m.bullets_per_burst);
	}
	r_h.add(p_d.mode_count);
	r_h.add(p_d.bolt_cycle);
	r_h.add(p_d.bolt_hold_until_fire_release);
	r_h.add(p_d.bolt_hold_until_zoom_release);
	r_h.add(p_d.unzoom_on_bolt);
	r_h.add(p_d.pre_fire_delay);
	r_h.add(p_d.spawn_delay);
	r_h.add(p_d.hold_delay);
	r_h.add(p_d.hold_max_time);
	r_h.add(p_d.hold_min_power);
	r_h.add(p_d.hold_max_power);
	r_h.add(p_d.hold_power_per_second);
	r_h.add(p_d.magazine_capacity);
	r_h.add(p_d.magazines);
	r_h.add(p_d.unlimited_ammo);
	r_h.add(p_d.pellets);
	r_h.add(p_d.trace_frequency);
	r_h.add(p_d.reload_type);
	r_h.add(p_d.reload_tactical);
	r_h.add(p_d.reload_empty);
	r_h.add(p_d.reload_delay);
	r_h.add(p_d.reload_threshold);
	r_h.add(p_d.reload_commit_tactical);
	r_h.add(p_d.reload_commit_empty);
	r_h.add(p_d.switch_cancels_reload);
	r_h.add(p_d.always_auto_reload);
	r_h.add(p_d.hold_off_reload_until_fire_release);
	r_h.add(p_d.deploy_time);
	r_h.add(p_d.zoom_blocked_after_deploy);
	for (const auto &stance : p_d.sway) {
		for (const SwayState &s : stance) {
			for (const Dispersion &d : s.move) {
				hash_dispersion(r_h, d);
			}
			for (const bool b : s.has_move) {
				r_h.add(b);
			}
			hash_recoil(r_h, s.recoil);
		}
	}
	for (const SwayModifier &m : p_d.suppression_modifier) {
		hash_modifier(r_h, m);
	}
	r_h.add(uint64_t(p_d.conditional.size()));
	for (const WeaponSimDef::Conditional &c : p_d.conditional) {
		r_h.add(c.only_supported);
		r_h.add(c.only_light);
		for (const auto &stance : c.mod) {
			for (const SwayModifier &m : stance) {
				hash_modifier(r_h, m);
			}
		}
	}
	r_h.add(p_d.first_shot_recoil_multiplier);
	r_h.add(p_d.shooting_recoil_decrease_scale);
	for (size_t s = 0; s < 3; ++s) {
		for (size_t z = 0; z < 2; ++z) {
			r_h.add(p_d.recovery_factor_idle[s][z]);
			r_h.add(p_d.recovery_factor_firing[s][z]);
		}
	}
	for (const Dispersion &d : p_d.pellet_cone) {
		hash_dispersion(r_h, d);
	}
	const ProjectileSimDef &p = p_d.projectile;
	r_h.add(p.kind);
	r_h.add3(p.launch_velocity);
	r_h.add3(p.launch_offset);
	r_h.add(p.muzzle_speed);
	r_h.add(p.gravity);
	r_h.add(p.time_to_live);
	r_h.add(p.instant_hit);
	r_h.add(p.start_damage);
	r_h.add(p.end_damage);
	r_h.add(p.falloff_start);
	r_h.add(p.falloff_end);
	r_h.add(p.time_to_arm);
	r_h.add(p.detonate_on_timeout);
	r_h.add(p.direct_damage);
	r_h.add(p.engine_acceleration);
	r_h.add(p.max_speed);
	r_h.add(p.engine_ignition);
	r_h.add(p.engine_time_to_live);
	r_h.add(p.drag);
	r_h.add(p.bounce_speed_multiplier);
	r_h.add(p.min_bounce_speed);
	r_h.add(p.property);
	r_h.add(p.physics_material);
	r_h.add(p.never_penetrate_any);
	for (const core::gameplay::DamageRelation &r : p.zone) {
		hash_relation(r_h, r);
	}
	hash_explosion(r_h, p.explosion);
	hash_explosion(r_h, p.dud);
	r_h.add(p.suppression_min);
	r_h.add(p.suppression_max);
	r_h.add(p.suppression_min_distance);
	r_h.add(p.suppression_max_distance);
}

void hash_mechanics(detail::Fnv64 &r_h, const Mechanics &p_m) {
	r_h.add(p_m.disc);
	r_h.add(p_m.recovery);
	r_h.add(p_m.first_shot_rearm);
	r_h.add(p_m.min_spread_rise);
	r_h.add(p_m.falloff);
	r_h.add(p_m.falloff_basis);
	r_h.add(p_m.blast_falloff);
	r_h.add(p_m.stance_transition_penalty);
	r_h.add(p_m.suppression_threshold);
	r_h.add(p_m.apply_protection_threshold);
	r_h.add(p_m.bullets_pass_soldiers);
	r_h.add(p_m.blast_occlusion);
	r_h.add(p_m.empty_reload_unchambered);
	r_h.add(p_m.sprint_blocks_fire);
	r_h.add(p_m.spawn_protection_blocks);
	r_h.add(p_m.bullet_damage_modifier);
	r_h.add(p_m.measured);
}

} // namespace

core::Result<WeaponSimDef> make_weapon_sim_def(const core::gameplay::WeaponDef &p_def, const SurfaceMaterialTable &p_materials) {
	Checker c(p_def.variant);
	const FireFunction &f = p_def.fire;
	WeaponSimDef d;
	d.variant = p_def.variant;
	d.unlock_identifier = p_def.unlock_identifier;
	d.weapon_class = p_def.weapon_class;

	// Fire modes: FireLogicType first, then FireLogicTypeArray in data order.
	std::vector<FireLogic> logics;
	logics.push_back(f.logic);
	for (const FireLogic l : f.extra_modes) {
		logics.push_back(l);
		if (logics.size() > d.modes.size()) {
			return core::make_error(p_def.variant + ": FireLogicTypeArray has " + std::to_string(f.extra_modes.size()) + " modes (limit " +
					std::to_string(d.modes.size() - 1) + ")");
		}
	}
	for (size_t i = 0; i < logics.size(); ++i) {
		FireModeDef &m = d.modes[i];
		m.logic = logics[i];
		if (m.logic == FireLogic::Unknown) {
			c.fail(i == 0 ? "FireLogicType" : "FireLogicTypeArray", "is not a known fire logic");
		}
		const float rof = c.range(f.rate_of_fire, 0.0f, 1e9f, "RateOfFire");
		if (!(rof > 0)) {
			c.fail("RateOfFire", "must be > 0");
		}
		m.interval = rpm_to_interval(data_decimal(rof));
		if (m.interval < kMinInterval) {
			c.fail("RateOfFire", "= " + std::to_string(rof) + " gives an interval below tick / " + std::to_string(kMaxShotsPerTick));
		}
		if (m.logic == FireLogic::Burst) {
			const float burst = c.range(f.rate_of_fire_for_burst, 0.0f, 1e9f, "RateOfFireForBurst");
			if (!(burst > 0)) {
				c.fail("RateOfFireForBurst", "must be > 0 for a burst mode");
			}
			m.burst_interval = rpm_to_interval(data_decimal(burst));
			if (m.burst_interval < kMinInterval) {
				c.fail("RateOfFireForBurst", "= " + std::to_string(burst) + " gives an interval below tick / " + std::to_string(kMaxShotsPerTick));
			}
			if (f.bullets_per_burst < 1 || f.bullets_per_burst > kMaxBulletsPerBurst) {
				c.fail("NumberOfBulletsPerBurst", "= " + std::to_string(f.bullets_per_burst) + " is outside [1, " + std::to_string(kMaxBulletsPerBurst) + "]");
			}
			m.bullets_per_burst = std::clamp(f.bullets_per_burst, 1, kMaxBulletsPerBurst);
		} else {
			m.burst_interval = m.interval;
			m.bullets_per_burst = 1;
		}
	}
	d.mode_count = uint8_t(logics.size());

	d.bolt_cycle = seconds_to_units(data_decimal(c.range(f.bolt_action_delay, 0.0f, 60.0f, "BoltActionDelay")) +
			data_decimal(c.range(f.bolt_action_time, 0.0f, 60.0f, "BoltActionTime")));
	if (d.bolt_cycle >= kMaxDefTime) {
		c.fail("BoltActionDelay + BoltActionTime", "is above 60 s");
	}
	d.bolt_hold_until_fire_release = f.hold_bolt_action_until_fire_release;
	d.bolt_hold_until_zoom_release = f.hold_bolt_action_until_zoom_release;
	d.unzoom_on_bolt = f.unzoom_on_bolt_action;
	d.pre_fire_delay = c.time(f.pre_fire_delay, "PreFireDelay");
	d.spawn_delay = c.time(f.spawn_delay, "ShotConfigData.SpawnDelay");
	d.hold_delay = c.time(f.hold_delay, "HoldAndReleaseData.Delay");
	d.hold_max_time = c.time_saturating(f.hold_max_time, "HoldAndReleaseData.MaxHoldTime");
	d.hold_min_power = c.range(f.hold_min_power, 0.0f, kMaxPower, "HoldAndReleaseData.MinPowerModifier");
	d.hold_max_power = c.range(f.hold_max_power, 0.0f, kMaxPower, "HoldAndReleaseData.MaxPowerModifier");
	if (d.hold_min_power > d.hold_max_power) {
		c.fail("HoldAndReleaseData.MinPowerModifier", "is above MaxPowerModifier");
	}
	d.hold_power_per_second = c.rate(f.hold_power_per_second, "HoldAndReleaseData.PowerIncreasePerSecond");

	// Ammunition.
	if (f.magazine_capacity < 0 || f.magazine_capacity > kMaxMagazine) {
		c.fail("MagazineCapacity", "= " + std::to_string(f.magazine_capacity) + " is outside [0, " + std::to_string(kMaxMagazine) + "]");
	}
	d.magazine_capacity = std::clamp(f.magazine_capacity, 0, kMaxMagazine);
	if (f.number_of_magazines > kMaxMagazines) {
		c.fail("NumberOfMagazines", "= " + std::to_string(f.number_of_magazines) + " is above " + std::to_string(kMaxMagazines));
	}
	d.unlimited_ammo = f.number_of_magazines < 0;
	d.magazines = std::clamp(f.number_of_magazines, 0, kMaxMagazines);
	if (f.bullets_per_shell < 1 || f.bullets_per_shell > kMaxPellets) {
		c.fail("NumberOfBulletsPerShell", "= " + std::to_string(f.bullets_per_shell) + " is outside [1, " + std::to_string(kMaxPellets) + "]");
	}
	d.pellets = std::clamp(f.bullets_per_shell, 1, kMaxPellets);
	if (f.trace_frequency > kMaxTraceFrequency) {
		c.fail("TraceFrequency", "= " + std::to_string(f.trace_frequency) + " is above " + std::to_string(kMaxTraceFrequency));
	}
	d.trace_frequency = f.trace_frequency > 0 ? std::min(f.trace_frequency, kMaxTraceFrequency) : 0;

	// Reload: only the fields the reload type uses are read (the 870's unused ReloadTimeBulletsLeft is -1).
	d.reload_type = f.reload_type;
	d.reload_threshold = c.range(f.reload_threshold, 0.0f, 1.0f, "ReloadThreshold");
	switch (f.reload_type) {
		case ReloadType::MagazineWithPossibleShorterReload:
			d.reload_tactical = c.time(f.reload_time_bullets_left, "ReloadTimeBulletsLeft");
			d.reload_empty = c.time(f.reload_time, "ReloadTime");
			break;
		case ReloadType::Magazine:
			d.reload_empty = c.time(f.reload_time, "ReloadTime");
			d.reload_tactical = d.reload_empty;
			break;
		case ReloadType::SingleBullet:
			d.reload_empty = c.time(f.reload_time, "ReloadTime");
			d.reload_tactical = d.reload_empty;
			d.reload_delay = c.time(f.reload_delay, "ReloadDelay");
			if (d.reload_empty <= 0 && d.magazine_capacity > 0) {
				c.fail("ReloadTime", "must be > 0 for rtSingleBullet (one shell per ReloadTime)");
			}
			break;
		default:
			if (d.magazine_capacity > 0 && !d.unlimited_ammo) {
				c.fail("ReloadType", "is unknown for a weapon with a magazine");
			}
			break;
	}
	d.reload_commit_tactical = scale_units(d.reload_tactical, data_decimal(d.reload_threshold));
	d.reload_commit_empty = scale_units(d.reload_empty, data_decimal(d.reload_threshold));
	d.switch_cancels_reload = f.reload_logic == "rlWeaponSwitchCancelsUnfinishedReload";
	d.always_auto_reload = f.always_auto_reload;
	d.hold_off_reload_until_fire_release = f.hold_off_reload_until_fire_release;

	d.deploy_time = c.time(p_def.deploy_time, "DeployTime");
	d.zoom_blocked_after_deploy = c.time(p_def.disable_zoom_on_deploy_time, "DisableZoomOnDeployTime");

	// Spread and recoil.
	d.sway = p_def.sway.state;
	for (size_t s = 0; s < 3; ++s) {
		for (size_t z = 0; z < 2; ++z) {
			const std::string where = std::string("GunSway ") + kStanceName[s] + " " + kZoomName[z];
			const SwayState &st = d.sway[s][z];
			for (size_t m = 0; m < size_t(SwayMove::Count); ++m) {
				c.dispersion(st.move[m], where + " " + kMoveName[m]);
			}
			const Recoil &r = st.recoil;
			c.signed_angle(r.up_per_shot, where + " RecoilAmplitudeIncPerShot");
			c.angle(r.up_max, where + " RecoilAmplitudeMax");
			c.signed_angle(r.horizontal_min, where + " HorizontalRecoilAmplitudeIncPerShotMin");
			c.signed_angle(r.horizontal_max, where + " HorizontalRecoilAmplitudeIncPerShotMax");
			c.angle(r.horizontal_amplitude_max, where + " HorizontalRecoilAmplitudeMax");
			c.rate(r.decrease_factor, where + " RecoilAmplitudeDecreaseFactor");
		}
	}
	for (size_t z = 0; z < 2; ++z) {
		d.suppression_modifier[z] = p_def.sway.suppression[z];
		c.modifier(d.suppression_modifier[z], std::string("GunSway suppression ") + kZoomName[z]);
	}
	if (p_def.sway.conditional.size() > 64) {
		c.fail("GunSwayData.Modifiers", "has " + std::to_string(p_def.sway.conditional.size()) + " conditional modifiers (limit 64)");
	}
	for (size_t i = 0; i < p_def.sway.conditional.size() && i < 64; ++i) {
		const core::gameplay::SwayGatedModifier &g = p_def.sway.conditional[i];
		WeaponSimDef::Conditional cm;
		cm.only_supported = g.only_in_supported_shooting;
		cm.only_light = g.only_on_weapon_light_enabled;
		cm.mod = g.mod;
		for (size_t s = 0; s < 3; ++s) {
			for (size_t z = 0; z < 2; ++z) {
				c.modifier(cm.mod[s][z], "GunSway conditional modifier " + g.unlock);
			}
		}
		d.conditional.push_back(cm);
	}
	d.first_shot_recoil_multiplier = c.factor(p_def.sway.first_shot_recoil_multiplier, "FirstShotRecoilMultiplier");
	d.shooting_recoil_decrease_scale = c.factor(p_def.sway.shooting_recoil_decrease_scale, "ShootingRecoilDecreaseScale");
	for (size_t s = 0; s < 3; ++s) {
		for (size_t z = 0; z < 2; ++z) {
			const float rate = d.sway[s][z].recoil.decrease_factor;
			d.recovery_factor_idle[s][z] = c.ok() ? exp_factor(rate) : 1.0f;
			d.recovery_factor_firing[s][z] = c.ok() ? exp_factor(rate * d.shooting_recoil_decrease_scale) : 1.0f;
		}
	}
	for (size_t s = 0; s < 3; ++s) {
		d.pellet_cone[s] = f.weapon_dispersion[s];
		c.dispersion(d.pellet_cone[s], std::string("SoldierWeaponDispersion ") + kStanceName[s]);
	}

	d.projectile = make_projectile(c, p_def, p_materials);
	if (!c.ok()) {
		return c.error();
	}
	return d;
}

core::Result<std::shared_ptr<const WeaponTable>> make_weapon_table(std::span<const core::gameplay::WeaponDef> p_defs,
		std::shared_ptr<const SurfaceMaterialTable> p_materials, const WeaponTableOptions &p_options) {
	if (p_materials == nullptr) {
		return core::make_error("make_weapon_table: no SurfaceMaterialTable");
	}
	if (p_defs.size() >= size_t(kNoWeapon)) {
		return core::make_error("make_weapon_table: " + std::to_string(p_defs.size()) + " defs (limit " + std::to_string(kNoWeapon - 1) + ")");
	}
	if (!std::isfinite(p_options.sprint_recover_seconds) || p_options.sprint_recover_seconds < 0 || p_options.sprint_recover_seconds > 60) {
		return core::make_error("make_weapon_table: sprint recover time " + std::to_string(p_options.sprint_recover_seconds) + " s is outside [0, 60]");
	}
	if (!std::isfinite(p_options.mechanics.suppression_threshold) || !std::isfinite(p_options.mechanics.bullet_damage_modifier) ||
			p_options.mechanics.bullet_damage_modifier < 0 || p_options.mechanics.bullet_damage_modifier > kMaxFactor) {
		return core::make_error("make_weapon_table: Mechanics suppression threshold or bullet damage modifier out of range");
	}
	auto table = std::make_shared<WeaponTable>();
	table->defs.reserve(p_defs.size());
	for (const core::gameplay::WeaponDef &w : p_defs) {
		core::Result<WeaponSimDef> d = make_weapon_sim_def(w, *p_materials);
		if (!d) {
			return d.error();
		}
		table->defs.push_back(std::move(*d));
	}
	table->materials = std::move(p_materials);
	table->mechanics = p_options.mechanics;
	table->infinite_magazine = p_options.infinite_magazine;
	table->sprint_recover_time = seconds_to_units(data_decimal(p_options.sprint_recover_seconds));
	table->hash = weapon_table_hash(*table);
	return std::shared_ptr<const WeaponTable>(std::move(table));
}

uint64_t weapon_table_hash(const WeaponTable &p_table) {
	detail::Fnv64 h;
	h.add(uint64_t(p_table.defs.size()));
	for (const WeaponSimDef &d : p_table.defs) {
		hash_def(h, d);
	}
	if (p_table.materials != nullptr) {
		for (size_t z = 0; z < kHitZoneCount; ++z) {
			h.add(p_table.materials->zone_property(HitZone(z)));
		}
	}
	hash_mechanics(h, p_table.mechanics);
	h.add(p_table.infinite_magazine);
	h.add(p_table.sprint_recover_time);
	return h.value();
}

double sim_shot_interval_seconds(const WeaponSimDef &p_def, uint8_t p_mode) {
	if (p_mode >= p_def.mode_count) {
		return 0;
	}
	const FireModeDef &m = p_def.modes[p_mode];
	SimTime t = m.interval;
	if (m.logic == FireLogic::SingleWithBoltAction) {
		t = std::max(t, p_def.bolt_cycle);
	}
	return units_to_seconds(t);
}

double sim_effective_rpm(const WeaponSimDef &p_def, uint8_t p_mode) {
	const double t = sim_shot_interval_seconds(p_def, p_mode);
	return t > 0 ? 60.0 / t : 0.0;
}

} // namespace bf3::sim::weapons
