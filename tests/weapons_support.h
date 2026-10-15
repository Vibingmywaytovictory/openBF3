#pragma once
// Shared by the M2a weapons tests (weapons_tests.cpp, weapons_install_tests.cpp, weapons_range_tests.cpp): synthetic
// WeaponDefs carrying the §7.1 numbers of the gate weapons (bf3dump weapon on MP_Subway, docs/design/m2a-weapons.md
// §4.1), turned into a WeaponTable without data (an empty SurfaceMaterialTable plus the zone cells of §4.1 written
// in), and a synthetic wall world.

#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/weapons/shooting_range.h"
#include "bf3/sim/weapons/surface_materials.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <doctest/doctest.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace bf3::test::weapons {

namespace gp = bf3::core::gameplay;
namespace sw = bf3::sim::weapons;

struct Synthetic {
	gp::WeaponDef def;
	std::array<float, 4> zones{ 1, 1, 1, 1 }; // head, body, chest, legs (DamageProtectionMultiplier)
	std::array<float, 4> blast_zones{ 1, 1, 1, 1 };
};

inline void fill_sway(gp::WeaponDef &r_w, float p_hip_min, float p_hip_max, float p_ads_min, float p_ads_max, float p_inc, float p_dec, float p_up,
		float p_hmin, float p_hmax, float p_recoil_dec) {
	for (size_t s = 0; s < 3; ++s) {
		for (size_t z = 0; z < 2; ++z) {
			gp::SwayState &st = r_w.sway.state[s][z];
			const float lo = z == 0 ? p_hip_min : p_ads_min;
			const float hi = z == 0 ? p_hip_max : p_ads_max;
			for (size_t m = 0; m < size_t(gp::SwayMove::Count); ++m) {
				st.move[m] = { m == 1 ? lo + 0.5f : lo, hi, p_inc, p_dec };
				st.has_move[m] = s == 0 || m <= 1; // crouch and prone know Base and Moving only
			}
			st.recoil = { p_up, 10.0f, p_hmin, p_hmax, 10.0f, p_recoil_dec };
		}
	}
	r_w.sway.first_shot_recoil_multiplier = 2.5f;
	r_w.sway.shooting_recoil_decrease_scale = 0.1f;
	for (gp::SwayModifier &m : r_w.sway.suppression) {
		m = gp::SwayModifier{};
		m.min_angle = 1.5f;
		m.max_angle = 0.0f; // unset
		m.increase_per_shot = 0.1f;
		m.recoil_angle = 1.5f;
	}
}

inline gp::WeaponDef weapon(const std::string &p_variant, gp::FireLogic p_logic, float p_rof) {
	gp::WeaponDef w;
	w.variant = p_variant;
	w.weapon_class = "wcSynthetic";
	w.deploy_time = 0.67f;
	w.disable_zoom_on_deploy_time = 0.15f;
	gp::FireFunction &f = w.fire;
	f.logic = p_logic;
	f.rate_of_fire = p_rof;
	f.rate_of_fire_for_burst = p_rof;
	f.bullets_per_burst = 3;
	f.bolt_action_delay = 0.5f; // inert outside SingleWithBoltAction
	f.bolt_action_time = 1.0f;
	f.hold_bolt_action_until_fire_release = true;
	f.reload_type = gp::ReloadType::MagazineWithPossibleShorterReload;
	f.reload_time = 2.37f;
	f.reload_time_bullets_left = 1.8f;
	f.reload_threshold = 0.85f;
	f.always_auto_reload = true;
	f.reload_logic = "rlWeaponSwitchCancelsUnfinishedReload";
	f.magazine_capacity = 31;
	f.number_of_magazines = 5;
	f.trace_frequency = 1;
	f.initial_speed = { 0, 0, 650 };
	f.bullets_per_shell = 1;
	f.bullets_per_shot = 1;
	for (gp::Dispersion &d : f.weapon_dispersion) {
		d = { 0, 0, 0, 5 };
	}
	gp::ProjectileDef &p = w.projectile;
	p.kind = gp::ProjectileKind::Bullet;
	p.gravity = -15.0f;
	p.time_to_live = 1.5f;
	p.start_damage = 25.0f;
	p.end_damage = 18.4f;
	p.damage_falloff_start = 8.0f;
	p.damage_falloff_end = 50.0f;
	p.suppression = { true, 7, 7, 3, 500 };
	fill_sway(w, 2.5f, 7.0f, 0.2f, 3.0f, 0.1f, 15.0f, 0.26f, -0.4f, 0.1f, 18.0f);
	return w;
}

inline gp::Explosion explosion(float p_damage, float p_inner, float p_radius) {
	gp::Explosion e;
	e.present = true;
	e.blast_damage = p_damage;
	e.inner_blast_radius = p_inner;
	e.blast_radius = p_radius;
	e.max_occlusion_raycast_radius = 0.25f;
	e.disable_static_entity_occlusion = true;
	return e;
}

// The §4.1 gate weapons with their MP_Subway numbers.
inline std::vector<Synthetic> synthetic_gate_weapons() {
	std::vector<Synthetic> out;
	auto rifle = [](const std::string &p_v, gp::FireLogic p_l, float p_rof) {
		Synthetic s;
		s.def = weapon(p_v, p_l, p_rof);
		s.zones = { 2.0f, 1.0f, 1.0f, 0.91f };
		return s;
	};
	{
		Synthetic s = rifle("weapons/m16a4/u_m16a4", gp::FireLogic::Automatic, 800);
		s.def.fire.extra_modes = { gp::FireLogic::Single };
		out.push_back(s);
		Synthetic b = rifle("weapons/m16a4/u_m16_burst", gp::FireLogic::Burst, 800);
		b.def.fire.extra_modes = { gp::FireLogic::Single };
		out.push_back(b);
	}
	auto bolt = [&](const std::string &p_v, float p_cycle, float p_v0, float p_start, float p_end, float p_fs, float p_fe, float p_chest) {
		Synthetic s = rifle(p_v, gp::FireLogic::SingleWithBoltAction, 360);
		gp::FireFunction &f = s.def.fire;
		f.bolt_action_delay = 0.0f;
		f.bolt_action_time = p_cycle;
		f.hold_bolt_action_until_fire_release = true;
		f.force_bolt_action_on_fire_trigger = true;
		f.hold_off_reload_until_fire_release = true;
		f.force_reload_action_on_fire_trigger = true;
		f.reload_time_bullets_left = 2.9f;
		f.reload_time = 4.7f;
		f.reload_threshold = 0.75f;
		f.magazine_capacity = 6;
		f.number_of_magazines = 7;
		f.initial_speed = { 0, 0, p_v0 };
		s.def.deploy_time = 0.95f;
		gp::ProjectileDef &p = s.def.projectile;
		p.gravity = -9.81f;
		p.time_to_live = 5.0f;
		p.start_damage = p_start;
		p.end_damage = p_end;
		p.damage_falloff_start = p_fs;
		p.damage_falloff_end = p_fe;
		s.zones = { 2.35f, 1.0f, p_chest, 0.91f };
		return s;
	};
	out.push_back(bolt("weapons/model98b/u_m98b", 1.3f, 650, 95, 59, 15, 150, 1.06f));
	out.push_back(bolt("weapons/xp2_jng90/u_jng90", 1.3f, 560, 80, 59, 15, 100, 1.25f));
	out.push_back(bolt("weapons/xp1_l96/u_l96", 1.38f, 540, 80, 59, 15, 100, 1.25f));
	out.push_back(bolt("weapons/m40a5/u_m40a5", 1.1f, 490, 80, 59, 15, 100, 1.25f));
	out.push_back(bolt("weapons/sv98/u_sv98", 1.25f, 520, 80, 50, 20, 80, 1.25f));
	auto dmr = [&](const std::string &p_v, float p_rof, float p_v0, float p_start, float p_end, float p_fe) {
		Synthetic s = rifle(p_v, gp::FireLogic::Single, p_rof);
		s.def.fire.initial_speed = { 0, 0, p_v0 };
		gp::ProjectileDef &p = s.def.projectile;
		p.gravity = -9.81f;
		p.time_to_live = 5.0f;
		p.start_damage = p_start;
		p.end_damage = p_end;
		p.damage_falloff_start = 15;
		p.damage_falloff_end = p_fe;
		s.zones = { 2.35f, 1.0f, 1.25f, 0.91f };
		return s;
	};
	out.push_back(dmr("weapons/m39ebr/u_m39ebr", 300, 570, 50, 37.5f, 75));
	out.push_back(dmr("weapons/sks/u_sks", 333, 440, 43, 27, 60));
	{
		Synthetic s = rifle("weapons/an94/u_an94", gp::FireLogic::Burst, 600);
		gp::FireFunction &f = s.def.fire;
		f.extra_modes = { gp::FireLogic::Automatic, gp::FireLogic::Single };
		f.rate_of_fire_for_burst = 1200;
		f.bullets_per_burst = 2;
		f.reload_time_bullets_left = 2.45f;
		f.reload_time = 3.3f;
		f.reload_threshold = 0.75f;
		f.initial_speed = { 0, 0, 600 };
		out.push_back(s);
	}
	{
		Synthetic s = rifle("weapons/remington870/u_870", gp::FireLogic::SingleWithBoltAction, 600);
		gp::FireFunction &f = s.def.fire;
		f.bolt_action_delay = 0.0f;
		f.bolt_action_time = 0.48f;
		f.hold_bolt_action_until_fire_release = false;
		f.reload_type = gp::ReloadType::SingleBullet;
		f.reload_time = 0.67f;
		f.reload_time_bullets_left = -1.0f; // unused by rtSingleBullet (as in data)
		f.reload_delay = 1.15f;
		f.reload_threshold = 1.0f;
		f.magazine_capacity = 5;
		f.number_of_magazines = 7;
		f.bullets_per_shell = 12;
		f.initial_speed = { 0, 0, 300 };
		for (gp::Dispersion &d : f.weapon_dispersion) {
			d = { 3, 3, 0, 5 };
		}
		s.def.deploy_time = 0.72f;
		gp::ProjectileDef &p = s.def.projectile;
		p.gravity = -9.81f;
		p.time_to_live = 1.0f;
		p.start_damage = 18;
		p.end_damage = 6;
		p.damage_falloff_start = 8;
		p.damage_falloff_end = 25;
		s.zones = { 1.0f, 1.0f, 1.0f, 0.91f };
		out.push_back(s);
	}
	{
		Synthetic s = rifle("weapons/m9/u_m9", gp::FireLogic::Single, 400);
		gp::FireFunction &f = s.def.fire;
		f.reload_time_bullets_left = 1.35f;
		f.reload_time = 1.6f;
		f.reload_threshold = 0.75f;
		f.magazine_capacity = 16;
		f.number_of_magazines = 4;
		f.initial_speed = { 0, 0, 380 };
		s.def.deploy_time = 0.54f;
		gp::ProjectileDef &p = s.def.projectile;
		p.end_damage = 13.75f;
		p.damage_falloff_start = 12;
		p.damage_falloff_end = 50;
		out.push_back(s);
	}
	{
		Synthetic s = rifle("weapons/gadgets/m320/u_m320_he", gp::FireLogic::Single, 250);
		gp::FireFunction &f = s.def.fire;
		f.reload_type = gp::ReloadType::Magazine;
		f.reload_time = 2.9f;
		f.reload_time_bullets_left = -1.0f;
		f.reload_threshold = 0.75f;
		f.magazine_capacity = 1;
		f.number_of_magazines = 4;
		f.initial_speed = { 0, 1.5f, 45 };
		f.initial_position = { -0.1f, -0.3f, 0.86f };
		s.def.deploy_time = 1.0f;
		gp::ProjectileDef &p = s.def.projectile;
		p.gravity = -7.2f;
		p.time_to_live = 0.0f;
		p.start_damage = 100;
		p.end_damage = 100;
		p.damage_falloff_start = 100;
		p.damage_falloff_end = 200;
		p.time_to_arm_explosion = 0.5f;
		p.explosion = explosion(112, 0.35f, 5);
		p.dud_explosion = explosion(56, 2.1f, 5);
		s.zones = { 2.5f, 2.5f, 2.5f, 2.5f };
		out.push_back(s);
	}
	auto launcher = [&](const std::string &p_v, float p_rof, float p_reload) {
		Synthetic s = rifle(p_v, gp::FireLogic::Single, p_rof);
		gp::FireFunction &f = s.def.fire;
		f.reload_type = gp::ReloadType::Magazine;
		f.reload_time = p_reload;
		f.reload_time_bullets_left = -1.0f;
		f.reload_threshold = 0.6f;
		f.magazine_capacity = 1;
		f.number_of_magazines = 4;
		f.initial_speed = { 0, 0, 40 };
		f.initial_position = { -0.3f, -0.1f, 2.0f };
		s.def.deploy_time = 1.0f;
		gp::ProjectileDef &p = s.def.projectile;
		p.kind = gp::ProjectileKind::Missile;
		p.gravity = -1.5f;
		p.time_to_live = 10.0f;
		p.start_damage = 0;
		p.end_damage = 0;
		p.missile_damage = 225;
		p.engine_strength = 10;
		p.max_speed = 100;
		p.engine_time_to_ignition = 0;
		p.engine_time_to_live = 40;
		p.time_to_arm = 0.5f;
		p.drag = 0;
		p.explosion = explosion(112, 0.35f, 4);
		p.dud_explosion = explosion(56, 2.1f, 4);
		s.zones = { 2.5f, 2.5f, 2.5f, 2.5f };
		return s;
	};
	out.push_back(launcher("weapons/rpg7/u_rpg7", 100, 4.8f));
	out.push_back(launcher("weapons/smaw/u_smaw", 350, 5.8f));
	{
		Synthetic s = rifle("weapons/m67/u_m67", gp::FireLogic::HoldAndRelease, 300);
		gp::FireFunction &f = s.def.fire;
		f.hold_delay = 0.5f;
		f.hold_max_time = 9999.0f; // saturates at 60 s (no limit)
		f.hold_min_power = 1;
		f.hold_max_power = 1;
		f.hold_power_per_second = 0.1f;
		f.hold_killed_power_modifier = 0.34f;
		f.hold_force_fire_when_killed = true;
		f.reload_type = gp::ReloadType::Magazine;
		f.reload_time = 1.0f;
		f.reload_time_bullets_left = -1.0f;
		f.reload_threshold = 0.75f;
		f.reload_logic = "rlReloadUnaffectedByWeaponSwitch";
		f.magazine_capacity = 1;
		f.number_of_magazines = 1;
		f.initial_speed = { 0, 3, 18 };
		f.spawn_delay = 0.17f;
		s.def.deploy_time = 0.2f;
		gp::ProjectileDef &p = s.def.projectile;
		p.kind = gp::ProjectileKind::Grenade;
		p.gravity = -9.8f;
		p.time_to_live = 3.6f;
		p.detonate_on_timeout = true;
		p.start_damage = 0;
		p.end_damage = 0;
		p.collision_speed_multiplier = 0.3f;
		p.min_bounce_speed = 0.5f;
		p.explosion = explosion(112, 1.5f, 7);
		s.zones = { 0, 0, 0, 0 };
		out.push_back(s);
	}
	{
		Synthetic s = rifle("weapons/knife/u_knife", gp::FireLogic::Single, 175);
		gp::FireFunction &f = s.def.fire;
		f.reload_type = gp::ReloadType::Magazine;
		f.reload_time = 0;
		f.reload_threshold = 0;
		f.magazine_capacity = 1;
		f.number_of_magazines = -1; // unlimited
		f.trace_frequency = 0; // no tracers (as in data)
		f.initial_speed = { 0, 0, 1.8f };
		f.spawn_delay = 0.3f;
		s.def.deploy_time = 0.5f;
		gp::ProjectileDef &p = s.def.projectile;
		p.gravity = 0;
		p.time_to_live = 1.0f;
		p.start_damage = 100;
		p.end_damage = 100;
		p.damage_falloff_start = 100;
		p.damage_falloff_end = 200;
		p.instant_hit = true;
		s.zones = { 0.67f, 0.5f, 0.5f, 0.5f };
		out.push_back(s);
	}
	return out;
}

inline sw::WeaponSimDef sim_def(const Synthetic &p_s, const sw::SurfaceMaterialTable &p_materials) {
	bf3::core::Result<sw::WeaponSimDef> d = sw::make_weapon_sim_def(p_s.def, p_materials);
	REQUIRE_MESSAGE(d.ok(), (d.ok() ? std::string() : d.error().message));
	sw::WeaponSimDef out = std::move(*d);
	for (size_t z = 0; z < 4; ++z) {
		out.projectile.zone[z].present = true;
		out.projectile.zone[z].protection_multiplier = p_s.zones[z];
		out.projectile.explosion.zone_multiplier[z] = p_s.blast_zones[z];
		out.projectile.dud.zone_multiplier[z] = p_s.blast_zones[z];
	}
	return out;
}

// A table of the synthetic gate weapons (ids in synthetic_gate_weapons() order).
inline std::shared_ptr<const sw::WeaponTable> synthetic_table(bool p_infinite = false, const sw::Mechanics &p_mechanics = {}) {
	auto materials = std::make_shared<const sw::SurfaceMaterialTable>();
	auto t = std::make_shared<sw::WeaponTable>();
	for (const Synthetic &s : synthetic_gate_weapons()) {
		t->defs.push_back(sim_def(s, *materials));
	}
	t->materials = materials;
	t->mechanics = p_mechanics;
	t->infinite_magazine = p_infinite;
	t->hash = sw::weapon_table_hash(*t);
	return t;
}

inline uint16_t id_of(const sw::WeaponTable &p_table, const std::string &p_variant) {
	for (size_t i = 0; i < p_table.defs.size(); ++i) {
		if (p_table.defs[i].variant == p_variant) {
			return uint16_t(i);
		}
	}
	FAIL("no def " << p_variant);
	return 0;
}

inline std::unique_ptr<bf3::sim::CollisionWorld> wall_world(std::vector<float> p_walls, float p_eye = 1.6f) {
	const std::vector<bf3::sim::StaticTriangle> tris = sw::range_wall_triangles(p_walls, p_eye);
	bf3::core::Result<std::shared_ptr<const bf3::sim::StaticWorldShapes>> shapes = bf3::sim::build_static_world(std::span<const bf3::sim::StaticTriangle>(tris));
	REQUIRE_MESSAGE(shapes.ok(), (shapes.ok() ? std::string() : shapes.error().message));
	bf3::core::Result<std::unique_ptr<bf3::sim::CollisionWorld>> w = bf3::sim::CollisionWorld::create(*shapes);
	REQUIRE_MESSAGE(w.ok(), (w.ok() ? std::string() : w.error().message));
	return std::move(*w);
}

} // namespace bf3::test::weapons
