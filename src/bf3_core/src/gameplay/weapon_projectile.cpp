#include "weapon_readers.h"

#include <array>
#include <string_view>
#include <utility>

namespace bf3::core::gameplay::detail {

void read_explosion(Fields &r_f, const EbxInstance &p_e, Explosion &r_out) {
	const EbxStruct e = p_e.data();
	r_out.present = true;
	r_out.partition = r_f.partition_name(p_e.partition());
	r_out.blast_damage = r_f.f32(e, "BlastDamage");
	r_out.blast_radius = r_f.f32(e, "BlastRadius");
	r_out.inner_blast_radius = r_f.f32(e, "InnerBlastRadius");
	r_out.blast_impulse = r_f.f32_or(e, "BlastImpulse", 0);
	r_out.shockwave_damage = r_f.f32_or(e, "ShockwaveDamage", 0);
	r_out.shockwave_radius = r_f.f32_or(e, "ShockwaveRadius", 0);
	r_out.shockwave_impulse = r_f.f32_or(e, "ShockwaveImpulse", 0);
	r_out.shockwave_time = r_f.f32_or(e, "ShockwaveTime", 0);
	r_out.spawn_delay = r_f.f32_or(e, "SpawnDelay", 0);
	r_out.max_occlusion_raycast_radius = r_f.f32_or(e, "MaxOcclusionRaycastRadius", 0);
	r_out.disable_occlusion = r_f.flag_or(e, "DisableOcclusion", false);
	r_out.disable_static_entity_occlusion = r_f.flag_or(e, "DisableStaticEntityOcclusion", false);
	r_out.is_causing_suppression = r_f.flag_or(e, "IsCausingSuppression", false);

	if (p_e.get("MaterialPair")) {
		r_out.material = r_f.material_pair(e, "MaterialPair");
	}
}

LockType lock_type_from(std::string_view p_name) {
	static constexpr std::array<std::pair<std::string_view, LockType>, 6> kNames{ {
			{ "LockAlways", LockType::Always },
			{ "LockOnRadar", LockType::Radar },
			{ "LockOnHeat", LockType::Heat },
			{ "LockOnLaserPainted", LockType::LaserPainted },
			{ "LockNever", LockType::Never },
			{ "LockTypeCount", LockType::Count },
	} };
	for (const auto &[name, type] : kNames) {
		if (name == p_name) {
			return type;
		}
	}
	return LockType::Count;
}

void read_locking_controller(Fields &r_f, const EbxInstance &p_controller, LockingController &r_out) {
	const EbxStruct c = p_controller.data();
	r_out.present = true;
	r_out.lock_time = r_f.f32(c, "LockTime");
	r_out.release_time = r_f.f32(c, "ReleaseTime");
	r_out.release_on_new_target_time = r_f.f32(c, "ReleaseOnNewTargetTime");
	r_out.sample_rate = r_f.f32(c, "SampleRate");
	r_out.minimum_lock_time = r_f.f32(c, "MinimumLockTime");
	r_out.ray_length = r_f.f32(c, "RayLength");
	r_out.acceptance_angle = r_f.f32(c, "AcceptanceAngle");
	r_out.angle_constant = r_f.f32(c, "AngleConstant");
	r_out.distance_constant = r_f.f32(c, "DistanceConstant");
	r_out.sensitivity = r_f.f32(c, "Sensitivity");
	r_out.hold_still_threshold = r_f.f32(c, "HoldStillThreshold");
	r_out.lock_on_empty_vehicles = r_f.flag(c, "LockOnEmptyVehicles");
	r_out.lock_on_visible_targets_only = r_f.flag(c, "LockOnVisibleTargetsOnly");
	r_out.ignore_height_lock_distance = r_f.flag(c, "IgnoreHeigthLockDistance");
	r_out.position_only = r_f.flag(c, "PositionOnly");
	r_out.zoom_locks.clear();
	const OptionalEbxArray levels = r_f.array(c, "ZoomLevelLock");
	if (!levels) {
		return;
	}
	for (const EbxValue element : *levels) {
		const std::optional<EbxStruct> level = element.as_struct();
		if (!level) {
			r_f.fail(r_f.where(c, "ZoomLevelLock") + ": element is not a struct");
			return;
		}
		r_out.zoom_locks.push_back(lock_type_from(r_f.enum_text(*level, "LockType")));
	}
}

void read_projectile(Fields &r_f, const EbxInstance &p_projectile, ProjectileDef &r_projectile) {
	const EbxStruct p = p_projectile.data();
	r_projectile.type = std::string(p_projectile.type_name());
	r_projectile.partition = r_f.partition_name(p_projectile.partition());
	if (p_projectile.is_a("BulletEntityData")) {
		r_projectile.kind = ProjectileKind::Bullet;
	} else if (p_projectile.is_a("MissileEntityData")) {
		r_projectile.kind = ProjectileKind::Missile;
	} else if (p_projectile.is_a("GrenadeEntityData")) {
		r_projectile.kind = ProjectileKind::Grenade;
	} else {
		r_projectile.kind = ProjectileKind::Other;
	}

	r_projectile.time_to_live = r_f.f32(p, "TimeToLive");
	r_projectile.record_initial_speed = r_f.f32(p, "InitialSpeed");
	r_projectile.visual_converge_distance = r_f.f32_or(p, "VisualConvergeDistance", 0);
	r_projectile.detonate_on_timeout = r_f.flag_or(p, "DetonateOnTimeout", false);
	r_projectile.server_projectile_disabled = r_f.flag_or(p, "ServerProjectileDisabled", false);
	r_projectile.ammunition_type = r_f.text_or(p, "AmmunitionType");
	r_projectile.material = r_f.material_pair(p, "MaterialPair");
	if (p.get("FlyBySound")) {
		if (const std::optional<EbxRef> s = r_f.ref(p, "FlyBySound"); s && !s->is_null()) {
			r_projectile.fly_by_sound = r_f.target_partition(p_projectile.partition(), *s);
		}
	}
	r_projectile.fly_by_radius = r_f.f32_or(p, "FlyBySoundRadius", 0);
	r_projectile.fly_by_speed = r_f.f32_or(p, "FlyBySoundSpeed", 0);
	if (const std::optional<EbxInstance> e = r_f.follow(p, "Explosion", "ExplosionEntityData", false)) {
		read_explosion(r_f, *e, r_projectile.explosion);
	}
	if (const std::optional<EbxInstance> s = r_f.follow(p, "SuppressionData", "WeaponSuppressionData", false)) {
		const EbxStruct sd = s->data();
		r_projectile.suppression.present = true;
		r_projectile.suppression.min_multiplier = r_f.f32(sd, "MinMultiplier");
		r_projectile.suppression.max_multiplier = r_f.f32(sd, "MaxMultiplier");
		r_projectile.suppression.min_distance = r_f.f32(sd, "MinDistance");
		r_projectile.suppression.max_distance = r_f.f32(sd, "MaxDistance");
	}

	switch (r_projectile.kind) {
		case ProjectileKind::Bullet:
			r_projectile.gravity = r_f.f32(p, "Gravity");
			r_projectile.impact_impulse = r_f.f32(p, "ImpactImpulse");
			r_projectile.start_damage = r_f.f32(p, "StartDamage");
			r_projectile.end_damage = r_f.f32(p, "EndDamage");
			r_projectile.damage_falloff_start = r_f.f32(p, "DamageFalloffStartDistance");
			r_projectile.damage_falloff_end = r_f.f32(p, "DamageFalloffEndDistance");
			r_projectile.time_to_arm_explosion = r_f.f32(p, "TimeToArmExplosion");
			r_projectile.distribute_damage_over_time = r_f.f32_or(p, "DistributeDamageOverTime", 0);
			r_projectile.instant_hit = r_f.flag(p, "InstantHit");
			if (const std::optional<EbxInstance> e = r_f.follow(p, "DudExplosion", "ExplosionEntityData", false)) {
				read_explosion(r_f, *e, r_projectile.dud_explosion);
			}
			break;
		case ProjectileKind::Missile:
			r_projectile.gravity = r_f.f32(p, "Gravity");
			r_projectile.impact_impulse = r_f.f32(p, "ImpactImpulse");
			r_projectile.missile_damage = r_f.f32(p, "Damage");
			r_projectile.engine_strength = r_f.f32(p, "EngineStrength");
			r_projectile.max_speed = r_f.f32(p, "MaxSpeed");
			r_projectile.engine_time_to_ignition = r_f.f32(p, "EngineTimeToIgnition");
			r_projectile.engine_time_to_live = r_f.f32(p, "EngineTimeToLive");
			r_projectile.time_to_arm = r_f.f32(p, "TimeToArm");
			r_projectile.drag = r_f.f32(p, "Drag");
			r_projectile.max_turn_angle = r_f.f32_or(p, "MaxTurnAngle", 0);
			r_projectile.time_to_activate_guiding = r_f.f32_or(p, "TimeToActivateGuidingSystem", 0);
			r_projectile.apply_gravity_when_guided = r_f.flag(p, "ApplyGravityWhenGuided");
			if (const std::optional<EbxStruct> u = r_f.sub(p, "UnguidedData")) {
				MissileGuidance &g = r_projectile.guidance;
				if (const std::optional<EbxStruct> at = r_f.sub(*u, "StaticPosition")) {
					g.static_position = { r_f.f32(*at, "x"), r_f.f32(*at, "y") };
				}
				if (const std::optional<EbxStruct> offset = r_f.sub(*u, "TargetPositionOffset")) {
					g.target_position_offset = { r_f.f32(*offset, "x"), r_f.f32(*offset, "y") };
				}
				g.use_static_position = r_f.flag(*u, "UseStaticPosition");
				g.use_target_position = r_f.flag(*u, "UseTargetPosition");
			}
			if (const std::optional<EbxStruct> n = r_f.sub(p, "NearTargetDetonation")) {
				MissileGuidance &g = r_projectile.guidance;
				g.detonate_near_target = r_f.flag(*n, "DetonateNearTarget");
				g.detonation_radius = r_f.f32(*n, "DetonationRadius");
				g.min_detonation_delay = r_f.f32(*n, "MinDetonationDelay");
				g.max_detonation_delay = r_f.f32(*n, "MaxDetonationDelay");
			}
			if (const std::optional<EbxInstance> c =
							r_f.follow(p, "LockingController", "LockingControllerData", false)) {
				read_locking_controller(r_f, *c, r_projectile.guidance.controller);
			}
			if (const std::optional<EbxInstance> e = r_f.follow(p, "DudExplosion", "ExplosionEntityData", false)) {
				read_explosion(r_f, *e, r_projectile.dud_explosion);
			}
			break;
		case ProjectileKind::Grenade:
			r_projectile.gravity = r_f.f32(p, "Gravity");
			r_projectile.collision_speed_multiplier = r_f.f32(p, "CollisionSpeedMultiplier");
			r_projectile.min_bounce_speed = r_f.f32(p, "MinBounceSpeed");
			r_projectile.grenade_type = r_f.enum_text(p, "GrenadeType");
			break;
		default:

			r_projectile.gravity = r_f.f32_or(p, "Gravity", 0);
			break;
	}
}

}
