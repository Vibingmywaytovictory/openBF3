#include "test_support.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/gameplay/weapon_resolver.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::gameplay;

namespace {

constexpr const char *kMetro = "Win32/Levels/MP_Subway/MP_Subway";

bool near(double p_a, double p_b, double p_eps = 1e-4) {
	return std::fabs(p_a - p_b) <= p_eps;
}

using bf3::test::Mounted;
using bf3::test::open_or_skip;

std::unique_ptr<EbxLibrary> open_library(const Mounted &p_m, const std::string &p_spec) {
	Result<std::unique_ptr<EbxLibrary>> lib = EbxLibrary::open(p_m.install, expand_superbundles(*p_m.install, p_spec));
	REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
	return std::move(*lib);
}

WeaponDef resolve(const EbxResolver &p_db, const char *p_variant, const EbxResolver *p_ui = nullptr, std::vector<std::string> p_unlocks = {}) {
	WeaponResolveOptions o;
	o.ui = p_ui;
	o.unlocks = std::move(p_unlocks);
	Result<WeaponDef> w = resolve_weapon(p_db, p_variant, o);
	REQUIRE_MESSAGE(w.ok(), (w.ok() ? std::string() : w.error().message));
	return std::move(*w);
}

const SwayState &stand(const WeaponDef &p_w, Zoom p_zoom) {
	return p_w.sway.state[size_t(Stance::Stand)][size_t(p_zoom)];
}

double ohk_range(const ProjectileDef &p_p, double p_multiplier) {
	double lo = 0, hi = 1000;
	if (damage_at(p_p, 0) * p_multiplier < 100.0 - 1e-6) {
		return -1;
	}
	for (int i = 0; i < 60; ++i) {
		const double mid = 0.5 * (lo + hi);
		(damage_at(p_p, mid) * p_multiplier >= 100.0 - 1e-6 ? lo : hi) = mid;
	}
	return lo;
}

int shots_to_kill(double p_damage) {
	return int(std::ceil(100.0 / p_damage - 1e-9));
}

std::string level_partition(const std::string &p_superbundle) {
	std::string p = to_lower_ascii(p_superbundle);
	return p.rfind("win32/", 0) == 0 ? p.substr(6) : p;
}

}

TEST_SUITE("gameplay-unit") {
	TEST_CASE("derived arithmetic") {
		FireFunction f;
		f.logic = FireLogic::Automatic;
		f.rate_of_fire = 800;
		f.rate_of_fire_for_burst = 1200;
		CHECK(near(shot_interval(f), 0.075));
		CHECK(near(burst_interval(f), 0.05));
		f.logic = FireLogic::SingleWithBoltAction;
		f.rate_of_fire = 360;
		f.bolt_action_delay = 0;
		f.bolt_action_time = 1.3f;
		CHECK(near(effective_rpm(f), 46.1538, 1e-3));
		f.bolt_action_time = 0.05f;
		CHECK(near(shot_interval(f), 60.0 / 360.0));

		ProjectileDef p;
		p.start_damage = 25;
		p.end_damage = 18.4f;
		p.damage_falloff_start = 8;
		p.damage_falloff_end = 50;
		CHECK(near(damage_at(p, 0), 25));
		CHECK(near(damage_at(p, 8), 25));
		CHECK(near(damage_at(p, 29), 21.7, 1e-5));
		CHECK(near(damage_at(p, 50), 18.4, 1e-5));
		CHECK(near(damage_at(p, 500), 18.4, 1e-5));
		p.damage_falloff_end = p.damage_falloff_start;
		CHECK(near(damage_at(p, 9), 18.4, 1e-5));

		CHECK(near(drop_at(650, -15, 100), 0.1775, 1e-4));
		CHECK(near(drop_at(650, -15, 300), 1.5976, 1e-4));
		CHECK(near(drop_at(650, -9.81, 1000), 11.609, 1e-3));
		CHECK(drop_at(0, -15, 100) == 0);

		CHECK(loc_sid_hash("ID_P_WNAME_M16A4") == 0x0fd83dfau);
		CHECK(loc_sid_hash("") == 0xFFFFFFFFu);
		CHECK(fire_logic_from(3) == FireLogic::Burst);
		CHECK(fire_logic_from(9) == FireLogic::Unknown);
		CHECK(fire_logic_from(-1) == FireLogic::Unknown);
	}

	TEST_CASE("sway modifier math") {
		std::array<std::array<SwayState, 2>, 3> s{};
		for (auto &stance : s) {
			for (SwayState &z : stance) {
				z.move[0] = Dispersion{ 0.2f, 3.0f, 0.1f, 15.0f };
				z.recoil = Recoil{ 0.26f, 10.0f, -0.4f, 0.1f, 10.0f, 18.0f };
			}
		}
		SwayGatedModifier m;
		for (auto &stance : m.mod) {
			stance[1].min_angle = 0.5f;
			stance[1].max_angle = 0.0f;
			stance[1].recoil_angle = 0.75f;
			stance[0].recoil_magnitude = 1.15f;
		}
		apply_sway_modifier(s, m);
		CHECK(near(s[0][1].move[0].min_angle, 0.1));
		CHECK(near(s[0][1].move[0].max_angle, 3.0));
		CHECK(near(s[0][1].recoil.horizontal_min, -0.3));
		CHECK(near(s[0][1].recoil.horizontal_max, 0.075));
		CHECK(near(s[0][1].recoil.up_per_shot, 0.26));
		CHECK(near(s[2][0].recoil.up_per_shot, 0.299));
		CHECK(near(s[2][0].move[0].min_angle, 0.2));
	}
}

TEST_SUITE("gameplay") {
	TEST_CASE("M16A3, M16A4 burst and the §7.1 rifles on MP_Subway") {
		Mounted m;
		if (!open_or_skip(m)) {
			return;
		}
		std::unique_ptr<EbxLibrary> lib = open_library(m, kMetro);
		std::unique_ptr<EbxLibrary> ui = open_library(m, "Win32/UI");

		const WeaponDef a3 = resolve(*lib, "weapons/m16a4/u_m16a4", ui.get());
		CHECK(a3.blueprint == "weapons/m16a4/m16a4");
		CHECK(a3.unlock_identifier == 2580429694u);
		CHECK(a3.name_sid == "ID_P_WNAME_M16A4");
		CHECK(a3.name_sid_hash == 0x0fd83dfau);
		CHECK(a3.weapon_class == "wc556x45mmNATO");
		CHECK(a3.fire.logic == FireLogic::Automatic);
		REQUIRE(a3.fire.extra_modes.size() == 1);
		CHECK(a3.fire.extra_modes[0] == FireLogic::Single);
		CHECK(a3.fire.rate_of_fire == 800.0f);
		CHECK(near(shot_interval(a3.fire), 0.075));
		CHECK(a3.fire.magazine_capacity == 31);
		CHECK(a3.fire.number_of_magazines == 5);
		CHECK(a3.fire.reload_time_bullets_left == 1.8f);
		CHECK(a3.fire.reload_time == 2.37f);
		CHECK(a3.fire.reload_threshold == 0.85f);
		CHECK(a3.fire.reload_type == ReloadType::MagazineWithPossibleShorterReload);
		CHECK(a3.deploy_time == 0.67f);
		CHECK(muzzle_speed(a3) == 650.0);
		CHECK(a3.projectile.kind == ProjectileKind::Bullet);
		CHECK(a3.projectile.partition == "weapons/common/bullets/556x45mmnato");
		CHECK(a3.projectile.start_damage == 25.0f);
		CHECK(a3.projectile.end_damage == 18.4f);
		CHECK(a3.projectile.damage_falloff_start == 8.0f);
		CHECK(a3.projectile.damage_falloff_end == 50.0f);
		CHECK(a3.projectile.gravity == -15.0f);
		CHECK(a3.projectile.record_initial_speed == 350.0f);
		CHECK(a3.projectile.suppression.max_multiplier == 7.0f);
		CHECK(near(max_range(a3), 975.0));
		CHECK(near(drop_at(muzzle_speed(a3), a3.projectile.gravity, 300), 1.60, 0.005));
		CHECK(near(drop_at(muzzle_speed(a3), a3.projectile.gravity, 100), 0.178, 0.0005));
		const SwayState &hip = stand(a3, Zoom::NoZoom);
		const SwayState &ads = stand(a3, Zoom::Zoom);
		CHECK(hip.move[0].min_angle == 2.5f);
		CHECK(hip.move[0].max_angle == 7.0f);
		CHECK(hip.move[0].increase_per_shot == 0.1f);
		CHECK(ads.move[0].min_angle == 0.2f);
		CHECK(ads.move[0].max_angle == 3.0f);
		CHECK(ads.move[0].increase_per_shot == 0.1f);
		CHECK(ads.recoil.up_per_shot == 0.26f);
		CHECK(ads.recoil.horizontal_min == -0.4f);
		CHECK(ads.recoil.horizontal_max == 0.1f);
		CHECK(ads.recoil.decrease_factor == 18.0f);
		CHECK(a3.sway.first_shot_recoil_multiplier == 2.5f);
		CHECK(a3.sway.camera_spring_constant == 250.0f);
		CHECK(a3.sway.suppression[1].min_angle == 1.5f);
		CHECK(a3.sway.suppression[1].recoil_angle == 1.5f);
		CHECK(a3.sway.applied.empty());
		CHECK(a3.sway.modifiers_total == 7);
		CHECK(a3.fire.firing_dispersions == 0);
		CHECK_FALSE(a3.fire.fire_logic_recoil_nonzero);

		CHECK(shots_to_kill(damage_at(a3.projectile, 8)) == 4);
		CHECK(shots_to_kill(2.0 * damage_at(a3.projectile, 8)) == 2);
		CHECK(shots_to_kill(damage_at(a3.projectile, 50)) == 6);
		CHECK(shots_to_kill(2.0 * damage_at(a3.projectile, 50)) == 3);

		const WeaponDef a4 = resolve(*lib, "weapons/m16a4/u_m16_burst", ui.get());
		CHECK(a4.blueprint == a3.blueprint);
		REQUIRE(a4.active_unlocks.size() == 2);
		CHECK(a4.active_unlocks[1] == "weapons/m16a4/u_m16a4_burst");
		CHECK(a4.name_sid == "ID_P_WNAME_M16");
		CHECK(a4.fire.logic == FireLogic::Burst);
		REQUIRE(a4.fire.extra_modes.size() == 1);
		CHECK(a4.fire.extra_modes[0] == FireLogic::Single);
		CHECK(a4.fire.bullets_per_burst == 3);
		CHECK(a4.fire.rate_of_fire == 800.0f);
		CHECK(a4.fire.rate_of_fire_for_burst == 800.0f);
		REQUIRE(a4.sway.applied.size() == 1);
		CHECK(a4.sway.applied[0].unlock == "weapons/m16a4/u_m16a4_burst");
		CHECK(near(stand(a4, Zoom::Zoom).move[0].min_angle, 0.1));
		CHECK(near(stand(a4, Zoom::Zoom).move[1].min_angle, 0.5));
		CHECK(near(stand(a4, Zoom::Zoom).recoil.horizontal_max, 0.075));
		CHECK(near(-stand(a4, Zoom::Zoom).recoil.horizontal_min, 0.3));
		CHECK(stand(a4, Zoom::NoZoom).move[0].min_angle == 2.5f);
		CHECK(stand(a4, Zoom::Zoom).recoil.up_per_shot == 0.26f);
		CHECK(a4.fire.reload_time == a3.fire.reload_time);
		CHECK(a4.projectile.partition == a3.projectile.partition);
		CHECK(muzzle_speed(a4) == 650.0);

		const WeaponDef att = resolve(*lib, "weapons/m16a4/u_m16a4", nullptr, { "u_m16a4_sound_suppressor", "u_m16a4_bipod", "u_m16a4_foregrip" });
		CHECK(att.projectile.partition == "weapons/common/bullets/556x45mmnato_cold");
		CHECK(muzzle_speed(att) == 380.0);
		CHECK(att.silenced);
		CHECK(att.can_be_in_supported_shooting);
		CHECK(att.sway.applied.size() == 2);
		REQUIRE(att.sway.conditional.size() == 1);
		CHECK(att.sway.conditional[0].only_in_supported_shooting);
		CHECK(near(stand(att, Zoom::Zoom).move[0].min_angle, 0.2 * 1.5 * 0.75));
		CHECK(near(stand(att, Zoom::Zoom).recoil.up_per_shot, 0.26 * 0.9));

		const WeaponDef ak = resolve(*lib, "weapons/ak74m/u_ak74m");
		CHECK(ak.fire.rate_of_fire == 650.0f);
		CHECK(muzzle_speed(ak) == 600.0);
		CHECK(ak.projectile.start_damage == 25.0f);
		CHECK(ak.projectile.end_damage == 18.4f);
		CHECK(stand(ak, Zoom::Zoom).recoil.up_per_shot == 0.28f);
		CHECK(stand(ak, Zoom::Zoom).recoil.horizontal_min == -0.3f);
		CHECK(stand(ak, Zoom::Zoom).recoil.horizontal_max == 0.2f);
		CHECK(stand(ak, Zoom::Zoom).recoil.decrease_factor == 18.0f);
		CHECK(ak.sway.first_shot_recoil_multiplier == 1.5f);

		const WeaponDef saw = resolve(*lib, "weapons/m249/u_m249");
		CHECK(saw.fire.logic == FireLogic::Automatic);
		CHECK(saw.fire.rate_of_fire == 800.0f);
		CHECK(saw.fire.magazine_capacity == 100);
		CHECK(saw.fire.number_of_magazines == 3);
		CHECK(saw.fire.reload_time == 6.2f);
		CHECK(muzzle_speed(saw) == 620.0);
		CHECK(stand(saw, Zoom::NoZoom).move[0].min_angle == 5.0f);
		CHECK(stand(saw, Zoom::NoZoom).move[0].max_angle == 7.0f);
		CHECK(stand(saw, Zoom::Zoom).move[0].min_angle == 0.5f);
		CHECK(stand(saw, Zoom::Zoom).recoil.up_per_shot == 0.4f);
		CHECK(stand(saw, Zoom::Zoom).recoil.horizontal_min == -0.4f);
		CHECK(stand(saw, Zoom::Zoom).recoil.horizontal_max == 0.4f);
		CHECK(stand(saw, Zoom::Zoom).recoil.decrease_factor == 12.0f);
		CHECK(saw.sway.first_shot_recoil_multiplier == 2.2f);

		const WeaponDef an = resolve(*lib, "weapons/an94/u_an94");
		CHECK(an.fire.logic == FireLogic::Burst);
		CHECK(an.fire.bullets_per_burst == 2);
		CHECK(an.fire.rate_of_fire_for_burst == 1200.0f);
		CHECK(an.fire.rate_of_fire == 600.0f);
		CHECK(near(burst_interval(an.fire), 0.05));

		const WeaponDef m9 = resolve(*lib, "weapons/m9/u_m9");
		CHECK(m9.fire.rate_of_fire == 400.0f);
		CHECK(m9.projectile.start_damage == 25.0f);
		CHECK(m9.projectile.end_damage == 13.75f);
		CHECK(m9.projectile.damage_falloff_start == 12.0f);
		CHECK(m9.projectile.damage_falloff_end == 50.0f);
		CHECK(muzzle_speed(m9) == 380.0);
		CHECK(m9.fire.magazine_capacity == 16);
		CHECK(m9.fire.reload_time_bullets_left == 1.35f);
		CHECK(m9.fire.reload_time == 1.6f);
		CHECK(m9.fire.reload_threshold == 0.75f);
	}

	TEST_CASE("bolt actions, shotgun, launchers and grenades on MP_Subway") {
		Mounted m;
		if (!open_or_skip(m)) {
			return;
		}
		std::unique_ptr<EbxLibrary> lib = open_library(m, kMetro);

		const WeaponDef m98 = resolve(*lib, "weapons/model98b/u_m98b");
		CHECK(m98.fire.logic == FireLogic::SingleWithBoltAction);
		CHECK(m98.fire.rate_of_fire == 360.0f);
		CHECK(near(effective_rpm(m98.fire), 46.2, 0.05));
		CHECK(m98.projectile.start_damage == 95.0f);
		CHECK(m98.projectile.end_damage == 59.0f);
		CHECK(m98.projectile.damage_falloff_start == 15.0f);
		CHECK(m98.projectile.damage_falloff_end == 150.0f);
		CHECK(muzzle_speed(m98) == 650.0);
		CHECK(m98.projectile.gravity == -9.81f);
		CHECK(m98.fire.magazine_capacity == 6);
		CHECK(m98.fire.number_of_magazines == 7);
		CHECK(near(max_range(m98), 3250.0));

		CHECK(near(drop_at(650, m98.projectile.gravity, 300), 1.0447, 0.0005));
		CHECK(near(drop_at(650, m98.projectile.gravity, 1000), 11.6, 0.01));
		CHECK(m98.projectile.suppression.max_multiplier == 35.0f);

		const struct {
			const char *variant;
			float cycle;
			double rpm;
		} bolts[] = {
			{ "weapons/xp2_jng90/u_jng90", 1.3f, 46.2 },
			{ "weapons/xp1_l96/u_l96", 1.38f, 43.5 },
			{ "weapons/m40a5/u_m40a5", 1.1f, 54.5 },
			{ "weapons/sv98/u_sv98", 1.25f, 48.0 },
		};
		for (const auto &b : bolts) {
			CAPTURE(b.variant);
			const WeaponDef w = resolve(*lib, b.variant);
			CHECK(w.fire.logic == FireLogic::SingleWithBoltAction);
			CHECK(w.fire.rate_of_fire == 360.0f);
			CHECK(w.fire.bolt_action_delay == 0.0f);
			CHECK(w.fire.bolt_action_time == b.cycle);
			CHECK(near(effective_rpm(w.fire), b.rpm, 0.05));
		}

		const WeaponDef s870 = resolve(*lib, "weapons/remington870/u_870");
		CHECK(s870.fire.bullets_per_shell == 12);
		CHECK(s870.projectile.start_damage == 18.0f);
		CHECK(s870.projectile.end_damage == 6.0f);
		CHECK(s870.projectile.damage_falloff_start == 8.0f);
		CHECK(s870.projectile.damage_falloff_end == 25.0f);
		CHECK(muzzle_speed(s870) == 300.0);
		CHECK(s870.fire.bolt_action_time == 0.48f);
		CHECK(near(effective_rpm(s870.fire), 125.0, 1e-3));
		CHECK(s870.fire.weapon_dispersion[0].min_angle == 3.0f);
		CHECK(s870.fire.weapon_dispersion[0].max_angle == 3.0f);
		CHECK(s870.fire.reload_type == ReloadType::SingleBullet);
		CHECK(s870.fire.reload_delay == 1.15f);
		CHECK(s870.fire.reload_time == 0.67f);

		for (const char *v : { "weapons/rpg7/u_rpg7", "weapons/smaw/u_smaw" }) {
			CAPTURE(v);
			const WeaponDef w = resolve(*lib, v);
			CHECK(w.projectile.kind == ProjectileKind::Missile);
			CHECK(w.fire.initial_speed == Vec3{ 0, 0, 40 });
			CHECK(w.projectile.max_speed == 100.0f);
			CHECK(w.projectile.engine_strength == 10.0f);
			CHECK(w.projectile.gravity == -1.5f);
			CHECK(w.projectile.missile_damage == 225.0f);
			CHECK(w.projectile.time_to_arm == 0.5f);
			CHECK(w.projectile.drag == 0.0f);
			CHECK(w.projectile.explosion.blast_damage == 112.0f);
			CHECK(w.projectile.explosion.inner_blast_radius == 0.35f);
			CHECK(w.projectile.explosion.blast_radius == 4.0f);
			CHECK(w.projectile.dud_explosion.blast_damage == 56.0f);
			CHECK(w.projectile.suppression.max_multiplier == 50.0f);
		}

		const WeaponDef he = resolve(*lib, "weapons/gadgets/m320/u_m320_he");
		CHECK(he.fire.initial_speed == Vec3{ 0, 1.5f, 45 });
		CHECK(he.projectile.gravity == -7.2f);
		CHECK(he.projectile.start_damage == 100.0f);
		CHECK(he.projectile.time_to_arm_explosion == 0.5f);
		CHECK(he.projectile.explosion.blast_damage == 112.0f);
		CHECK(he.projectile.explosion.inner_blast_radius == 0.35f);
		CHECK(he.projectile.explosion.blast_radius == 5.0f);
		const WeaponDef m67 = resolve(*lib, "weapons/m67/u_m67");
		CHECK(m67.fire.logic == FireLogic::HoldAndRelease);
		CHECK(m67.projectile.kind == ProjectileKind::Grenade);
		CHECK(m67.fire.initial_speed == Vec3{ 0, 3, 18 });
		CHECK(m67.fire.spawn_delay == 0.17f);
		CHECK(m67.projectile.time_to_live == 3.6f);
		CHECK(m67.projectile.detonate_on_timeout);
		CHECK(m67.projectile.gravity == -9.8f);
		CHECK(m67.projectile.explosion.blast_damage == 112.0f);
		CHECK(m67.projectile.explosion.inner_blast_radius == 1.5f);
		CHECK(m67.projectile.explosion.blast_radius == 7.0f);
		CHECK(m67.fire.hold_delay == 0.5f);
		CHECK_FALSE(m67.allow_switching_out_of_ammo);
		CHECK(s870.allow_switching_out_of_ammo);

		CHECK(base_variant_of(*lib, "weapons/m16a4/m16a4") == "weapons/m16a4/u_m16a4");
		const WeaponDef ugl = resolve(*lib, "weapons/m16a4/u_m16a4_m320_he");
		CHECK(ugl.weapon_class == "wcUgl");
		CHECK(ugl.zoom_render_fov == 35.0f);
		CHECK(ugl.fire.initial_speed == Vec3{ 0, 1.5f, 45 });
		CHECK(ugl.projectile.partition == "weapons/gadgets/m320/40mmgrenade");
		CHECK(ugl.sway.partition == "weapons/gadgets/m320/40mm_gunsway");
	}

	TEST_CASE("body multipliers on Metro, MP_001, Caspian and XP2_Palace") {
		Mounted m;
		if (!open_or_skip(m)) {
			return;
		}
		std::unique_ptr<EbxLibrary> metro = open_library(m, kMetro);
		const WeaponDef a3 = resolve(*metro, "weapons/m16a4/u_m16a4");
		const WeaponDef m98 = resolve(*metro, "weapons/model98b/u_m98b");
		const WeaponDef m40 = resolve(*metro, "weapons/m40a5/u_m40a5");
		const WeaponDef sv98 = resolve(*metro, "weapons/sv98/u_sv98");
		const WeaponDef s870 = resolve(*metro, "weapons/remington870/u_870");
		const WeaponDef rpg = resolve(*metro, "weapons/rpg7/u_rpg7");
		const WeaponDef he = resolve(*metro, "weapons/gadgets/m320/u_m320_he");
		const WeaponDef m67 = resolve(*metro, "weapons/m67/u_m67");
		const WeaponDef knife = resolve(*metro, "weapons/knife/u_knife");
		const WeaponDef mk11 = resolve(*metro, "weapons/mk11/u_mk11");
		const WeaponDef sks = resolve(*metro, "weapons/sks/u_sks");

		for (const char *level : { kMetro, "Win32/Levels/MP_001/MP_001", "Win32/Levels/MP_007/MP_007", "Win32/Levels/XP2_Palace/XP2_Palace" }) {
			CAPTURE(level);
			std::unique_ptr<EbxLibrary> lib = open_library(m, level);
			const std::optional<std::string> name = MaterialGrid::grid_of_level(*lib, level_partition(level));
			REQUIRE(name.has_value());
			const EbxResolver *fallback[] = { metro.get() };
			Result<MaterialGrid> grid = MaterialGrid::load(*lib, *name, fallback);
			REQUIRE_MESSAGE(grid.ok(), (grid.ok() ? std::string() : grid.error().message));
			const MaterialGrid::LoadReport &rep = grid->report();
			CHECK(rep.grid_pairs == 32400);
			CHECK(rep.unresolved == 0);
			if (std::string(level).find("XP2_") != std::string::npos) {

				CHECK(rep.unresolved_in_level == 3839);
			} else {
				CHECK(rep.unresolved_in_level == 0);
			}
			Result<std::vector<HitCapsule>> caps = read_hit_capsules(*lib);
			REQUIRE_MESSAGE(caps.ok(), (caps.ok() ? std::string() : caps.error().message));
			CHECK(caps->size() == 13);
			size_t hi = 0;
			for (const HitCapsule &c : *caps) {
				hi += c.valid_hi_lod ? 1 : 0;
			}
			CHECK(hi == 9);

			auto zones = [&](const WeaponDef &p_w) {
				MaterialPairRef mat = p_w.projectile.material;
				REQUIRE(grid->complete(mat));
				return body_multipliers(*grid, *caps, mat.property());
			};
			auto check = [](const BodyMultipliers &p_b, float p_head, float p_body, float p_chest, float p_legs) {
				CHECK(p_b.head == doctest::Approx(p_head));
				CHECK(p_b.body == doctest::Approx(p_body));
				CHECK(p_b.chest == doctest::Approx(p_chest));
				CHECK(p_b.legs == doctest::Approx(p_legs));
			};
			const BodyMultipliers bullet = zones(a3);
			check(bullet, 2.0f, 1.0f, 1.0f, 0.91f);
			check(zones(m40), 2.35f, 1.0f, 1.25f, 0.91f);
			check(zones(m98), 2.35f, 1.0f, 1.06f, 0.91f);
			check(zones(s870), 1.0f, 1.0f, 1.0f, 0.91f);
			check(zones(rpg), 2.5f, 2.5f, 2.5f, 2.5f);
			check(zones(he), 2.5f, 2.5f, 2.5f, 2.5f);
			check(zones(m67), 0.0f, 0.0f, 0.0f, 0.0f);
			const BodyMultipliers melee = zones(knife);
			CHECK(melee.head == doctest::Approx(0.67f));
			CHECK(melee.body == doctest::Approx(0.5f));
			const std::optional<uint8_t> flechette = grid->property_by_name("Flechette");
			REQUIRE(flechette.has_value());
			check(body_multipliers(*grid, *caps, *flechette), 1.0f, 1.0f, 1.0f, 1.0f);
			CHECK(grid->material_name(m40.projectile.material.property()) == "Sniper");
			CHECK(grid->material_name(m98.projectile.material.property()) == "Magnum");

			CHECK(m98.projectile.end_damage * 2.35f > 100.0f);
			CHECK(m98.projectile.start_damage * 2.35f == doctest::Approx(223.25f));
			CHECK(ohk_range(m98.projectile, zones(m98).chest) == doctest::Approx(17.5).epsilon(0.01));
			CHECK(ohk_range(m40.projectile, zones(m40).chest) == doctest::Approx(15.0).epsilon(0.01));
			CHECK(ohk_range(sv98.projectile, zones(sv98).chest) == doctest::Approx(20.0).epsilon(0.01));
			CHECK(ohk_range(mk11.projectile, zones(mk11).head) == doctest::Approx(50.8).epsilon(0.01));
			CHECK(ohk_range(sks.projectile, zones(sks).head) == doctest::Approx(16.3).epsilon(0.01));
		}
	}

	TEST_CASE("every weapon variant of every MP level resolves") {
		Mounted m;
		if (!open_or_skip(m)) {
			return;
		}
		std::unique_ptr<EbxLibrary> ui = open_library(m, "Win32/UI");
		size_t levels = 0;
		size_t total = 0;
		size_t named = 0;
		for (const std::string &sb : expand_superbundles(*m.install, "@all-mp")) {
			if (sb.find("/Levels/") == std::string::npos) {
				continue;
			}
			CAPTURE(sb);
			std::unique_ptr<EbxLibrary> lib = open_library(m, sb);
			lib->load_all();
			const std::vector<std::string> variants = list_weapon_variants(*lib);
			CHECK(variants.size() >= 172);
			for (const std::string &v : variants) {
				CAPTURE(v);
				WeaponResolveOptions o;
				o.ui = ui.get();
				Result<WeaponDef> w = resolve_weapon(*lib, v, o);
				CHECK_MESSAGE(w.ok(), (w.ok() ? std::string() : w.error().message));
				named += (w.ok() && !w->name_sid.empty()) ? 1 : 0;
			}
			if (sb == kMetro) {
				CHECK(variants.size() == 172);
			}
			total += variants.size();
			++levels;
		}
		CHECK(levels == 29);
		std::printf("weapon variants resolved: %zu over %zu levels, %zu with a name SID\n", total, levels, named);
	}
}

TEST_SUITE("gameplay-fuzz") {
	TEST_CASE("mutated M16A4 partitions never crash the resolver") {
		Mounted m;
		if (!open_or_skip(m)) {
			return;
		}
		std::unique_ptr<EbxLibrary> lib = open_library(m, kMetro);
		const char *names[] = {
			"weapons/m16a4/u_m16_burst",
			"weapons/m16a4/u_m16a4_burst",
			"weapons/m16a4/m16a4",
			"weapons/m16a4/m16a4_gunsway",
			"weapons/m16a4/m16a4_burst",
			"weapons/common/bullets/556x45mmnato",
		};
		struct Source {
			std::vector<uint8_t> bytes;
			std::vector<std::pair<uint32_t, uint32_t>> instances;
		};
		std::vector<Source> sources;
		for (const char *n : names) {
			const EbxPartition *p = lib->partition_by_name(n);
			REQUIRE(p != nullptr);
			Source s{ std::vector<uint8_t>(p->bytes().begin(), p->bytes().end()), {} };
			for (const EbxPartition::InstanceEntry &e : p->instance_entries()) {
				s.instances.emplace_back(e.offset, p->complex_descs()[e.complex].size);
			}
			sources.push_back(std::move(s));
		}
		auto build = [&](size_t p_target, const std::vector<uint8_t> *p_bytes, EbxDb &r_db) {
			for (size_t i = 0; i < sources.size(); ++i) {
				Result<bool> r = r_db.parse_insert(names[i], i == p_target ? *p_bytes : sources[i].bytes);
				if (!r.ok()) {
					return false;
				}
			}
			return true;
		};
		{
			EbxDb db;
			REQUIRE(build(sources.size(), nullptr, db));
			Result<WeaponDef> w = resolve_weapon(db, names[0]);
			REQUIRE_MESSAGE(w.ok(), (w.ok() ? std::string() : w.error().message));
			CHECK(w->fire.logic == FireLogic::Burst);
		}

		std::mt19937 rng(0xB3F3u);
		size_t parsed = 0;
		size_t resolved = 0;
		size_t refused = 0;
		for (int iter = 0; iter < 600; ++iter) {
			const size_t target = size_t(iter) % sources.size();
			const Source &src = sources[target];
			std::vector<uint8_t> bytes = src.bytes;
			const int flips = 1 + int(rng() % 6);
			for (int k = 0; k < flips; ++k) {
				const auto &[at, size] = src.instances[rng() % src.instances.size()];
				if (size == 0 || uint64_t(at) + size > bytes.size()) {
					continue;
				}
				bytes[at + rng() % size] ^= uint8_t(1u << (rng() % 8));
			}
			EbxDb db;
			if (!build(target, &bytes, db)) {
				continue;
			}
			++parsed;
			Result<WeaponDef> w = resolve_weapon(db, names[0]);
			(w.ok() ? resolved : refused) += 1;
		}
		std::printf("fuzz: %zu mutations parsed, %zu resolved, %zu refused with an error\n", parsed, resolved, refused);
		CHECK(parsed > 0);
	}

	TEST_CASE("mutated material grid never crashes the grid reader") {
		Mounted m;
		if (!open_or_skip(m)) {
			return;
		}
		std::unique_ptr<EbxLibrary> lib = open_library(m, kMetro);
		const std::string grid_name = "levels/mp_subway/mp_subway/materialgrid_win32/grid";
		const EbxPartition *grid = lib->partition_by_name(grid_name);
		const EbxPartition *container = lib->partition_by_name("materials/materialcontainer");
		REQUIRE(grid != nullptr);
		REQUIRE(container != nullptr);
		const std::vector<uint8_t> container_bytes(container->bytes().begin(), container->bytes().end());
		const std::vector<uint8_t> grid_bytes(grid->bytes().begin(), grid->bytes().end());
		std::vector<std::pair<uint32_t, uint32_t>> instances;
		for (const EbxPartition::InstanceEntry &e : grid->instance_entries()) {
			instances.emplace_back(e.offset, grid->complex_descs()[e.complex].size);
		}
		std::mt19937 rng(0x6121Du);
		size_t parsed = 0;
		size_t loaded = 0;
		for (int iter = 0; iter < 60; ++iter) {
			std::vector<uint8_t> bytes = grid_bytes;
			const int flips = 1 + int(rng() % 6);
			for (int k = 0; k < flips; ++k) {

				if (rng() % 2 == 0) {
					bytes[rng() % bytes.size()] ^= uint8_t(1u << (rng() % 8));
					continue;
				}
				const auto &[at, size] = instances[rng() % instances.size()];
				if (size != 0 && uint64_t(at) + size <= bytes.size()) {
					bytes[at + rng() % size] ^= uint8_t(1u << (rng() % 8));
				}
			}
			EbxDb db;
			if (!db.parse_insert(grid_name, std::move(bytes)).ok() || !db.parse_insert("materials/materialcontainer", container_bytes).ok()) {
				continue;
			}
			++parsed;
			Result<MaterialGrid> g = MaterialGrid::load(db, grid_name);
			if (g.ok()) {
				++loaded;
				for (int q = 0; q < 64; ++q) {
					(void)g->damage(uint8_t(rng()), uint8_t(rng()));
				}
			}
		}
		std::printf("grid fuzz: %zu mutations parsed, %zu loaded\n", parsed, loaded);
		CHECK(parsed > 0);
	}
}
