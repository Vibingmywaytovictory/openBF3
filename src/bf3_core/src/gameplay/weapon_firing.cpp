#include "weapon_readers.h"

#include <cmath>

namespace bf3::core::gameplay::detail {

namespace {

ReloadType reload_type_from(std::string_view p_name) {
	if (p_name == "rtSingleBullet" || p_name == "0") {
		return ReloadType::SingleBullet;
	}
	if (p_name == "rtMagazine" || p_name == "1") {
		return ReloadType::Magazine;
	}
	if (p_name == "rtMagazineWithPossibleShorterReload" || p_name == "2") {
		return ReloadType::MagazineWithPossibleShorterReload;
	}
	return ReloadType::Unknown;
}

Dispersion read_firing_dispersion(Fields &r_f, const EbxStruct &p_s) {
	Dispersion d;
	d.min_angle = r_f.f32(p_s, "MinAngle");
	d.max_angle = r_f.f32(p_s, "MaxAngle");
	d.increase_per_shot = r_f.f32(p_s, "IncreasePerShot");
	d.decrease_per_second = r_f.f32(p_s, "DecreasePerSecond");
	return d;
}

}

void read_fire_function(Fields &r_f, const EbxInstance &p_ffd, FireFunction &r_fire) {
	const EbxStruct ffd = p_ffd.data();
	if (const std::optional<EbxStruct> fl = r_f.sub(ffd, "FireLogic")) {
		const std::optional<EbxValue> type = r_f.value(*fl, "FireLogicType");
		r_fire.logic = (type && type->as_i32()) ? fire_logic_from(*type->as_i32()) : FireLogic::Unknown;
		if (const OptionalEbxArray modes = r_f.array(*fl, "FireLogicTypeArray")) {
			for (const EbxValue m : *modes) {
				const std::optional<int32_t> v = m.as_i32();
				if (!v) {
					r_f.fail(r_f.where(*fl, "FireLogicTypeArray") + ": element is not an integer");
					break;
				}
				r_fire.extra_modes.push_back(fire_logic_from(*v));
			}
		}
		r_fire.rate_of_fire = r_f.f32(*fl, "RateOfFire");
		r_fire.rate_of_fire_for_burst = r_f.f32(*fl, "RateOfFireForBurst");
		r_fire.client_fire_rate_multiplier = r_f.f32(*fl, "ClientFireRateMultiplier");
		r_fire.pre_fire_delay = r_f.f32(*fl, "PreFireDelay");
		r_fire.automatic_delay = r_f.f32(*fl, "AutomaticDelay");
		r_fire.trigger_pull_weight = r_f.f32_or(*fl, "TriggerPullWeight", 0);
		if (const std::optional<EbxStruct> b = r_f.sub(*fl, "BoltAction")) {
			r_fire.bolt_action_delay = r_f.f32(*b, "BoltActionDelay");
			r_fire.bolt_action_time = r_f.f32(*b, "BoltActionTime");
			r_fire.unzoom_on_bolt_action = r_f.flag(*b, "UnZoomOnBoltAction");
			r_fire.return_to_zoom_after_bolt_action = r_f.flag_or(*b, "ReturnToZoomAfterBoltAction", false);
			r_fire.hold_bolt_action_until_fire_release = r_f.flag(*b, "HoldBoltActionUntilFireRelease");
			r_fire.hold_bolt_action_until_zoom_release = r_f.flag(*b, "HoldBoltActionUntilZoomRelease");
			r_fire.force_bolt_action_on_fire_trigger = r_f.flag_or(*b, "ForceBoltActionOnFireTrigger", false);
		}
		if (const std::optional<EbxStruct> h = r_f.sub(*fl, "HoldAndRelease")) {
			r_fire.hold_delay = r_f.f32(*h, "Delay");
			r_fire.hold_max_time = r_f.f32_or(*h, "MaxHoldTime", 0);
			r_fire.hold_min_power = r_f.f32_or(*h, "MinPowerModifier", 0);
			r_fire.hold_max_power = r_f.f32_or(*h, "MaxPowerModifier", 0);
			r_fire.hold_power_per_second = r_f.f32_or(*h, "PowerIncreasePerSecond", 0);
			r_fire.hold_killed_power_modifier = r_f.f32_or(*h, "KilledHoldingPowerModifier", 0);
			r_fire.hold_force_fire_when_killed = r_f.flag_or(*h, "ForceFireWhenKilledHolding", false);
		}
		if (const std::optional<EbxStruct> rc = r_f.sub(*fl, "Recoil")) {
			for (const char *field : { "MaxRecoilAngleX", "MaxRecoilAngleY", "MaxRecoilAngleZ", "MinRecoilAngleX", "MinRecoilAngleY", "MinRecoilAngleZ" }) {
				if (r_f.f32_or(*rc, field, 0) != 0) {
					r_fire.fire_logic_recoil_nonzero = true;
				}
			}
		}
		r_fire.reload_type = reload_type_from(r_f.enum_text(*fl, "ReloadType"));
		r_fire.reload_time = r_f.f32(*fl, "ReloadTime");
		r_fire.reload_time_bullets_left = r_f.f32(*fl, "ReloadTimeBulletsLeft");
		r_fire.reload_delay = r_f.f32(*fl, "ReloadDelay");
		r_fire.reload_threshold = r_f.f32(*fl, "ReloadThreshold");
		r_fire.always_auto_reload = r_f.flag_or(*fl, "AlwaysAutoReload", false);
		r_fire.reload_logic = r_f.enum_text(*fl, "ReloadLogic");

		r_fire.hold_off_reload_until_fire_release = r_f.flag_or(*fl, "HoldOffReloadUntilFireRelease", false);
		r_fire.force_reload_action_on_fire_trigger = r_f.flag_or(*fl, "ForceReloadActionOnFireTrigger", false);
	}
	if (const std::optional<EbxStruct> ammo = r_f.sub(ffd, "Ammo")) {
		r_fire.magazine_capacity = r_f.i32(*ammo, "MagazineCapacity");
		r_fire.number_of_magazines = r_f.i32(*ammo, "NumberOfMagazines");
		r_fire.trace_frequency = r_f.i32_or(*ammo, "TraceFrequency", 0);
		r_fire.auto_replenish_delay = r_f.f32_or(*ammo, "AutoReplenishDelay", 0);
	}
	if (const std::optional<EbxStruct> shot = r_f.sub(ffd, "Shot")) {
		r_fire.initial_position = r_f.vec3(*shot, "InitialPosition");
		r_fire.initial_direction = r_f.vec3(*shot, "InitialDirection");
		r_fire.initial_speed = r_f.vec3(*shot, "InitialSpeed");
		r_fire.bullets_per_shell = r_f.i32(*shot, "NumberOfBulletsPerShell");
		r_fire.bullets_per_shot = r_f.i32(*shot, "NumberOfBulletsPerShot");
		r_fire.bullets_per_burst = r_f.i32(*shot, "NumberOfBulletsPerBurst");
		r_fire.spawn_delay = r_f.f32(*shot, "SpawnDelay");
		r_fire.inherit_weapon_speed = r_f.f32_or(*shot, "InheritWeaponSpeedAmount", 0);
		r_fire.active_force_spawn_to_camera = r_f.flag_or(*shot, "ActiveForceSpawnToCamera", false);
		r_fire.force_spawn_to_camera = r_f.flag_or(*shot, "ForceSpawnToCamera", false);
	}
	if (const std::optional<EbxStruct> heat = r_f.sub(ffd, "OverHeat")) {
		r_fire.heat_per_bullet = r_f.f32(*heat, "HeatPerBullet");
		r_fire.heat_drop_per_second = r_f.f32(*heat, "HeatDropPerSecond");
		r_fire.overheat_penalty_time = r_f.f32(*heat, "OverHeatPenaltyTime");
		r_fire.overheat_threshold = r_f.f32(*heat, "OverHeatThreshold");
	}
	if (const std::optional<EbxStruct> wd = r_f.sub(ffd, "WeaponDispersion")) {
		constexpr const char *kStances[3] = { "StandDispersion", "CrouchDispersion", "ProneDispersion" };
		for (size_t s = 0; s < 3; ++s) {
			if (const std::optional<EbxStruct> d = r_f.sub(*wd, kStances[s])) {
				r_fire.weapon_dispersion[s] = read_firing_dispersion(r_f, *d);
			}
		}
		r_fire.jump_dispersion_angle = r_f.f32(*wd, "JumpDispersionAngle");
		r_fire.move_dispersion_angle = r_f.f32(*wd, "MoveDispersionAngle");
		r_fire.move_zoomed_dispersion_angle = r_f.f32(*wd, "MoveZoomedDispersionAngle");
		r_fire.weapon_dispersion_decrease_per_second = r_f.f32(*wd, "DecreasePerSecond");
	}
	if (const OptionalEbxArray disp = r_f.array(ffd, "Dispersion")) {
		r_fire.firing_dispersions = disp->size();
	}
}

}
