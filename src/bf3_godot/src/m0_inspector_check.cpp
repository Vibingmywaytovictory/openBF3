#include "m0_inspector_check.h"

#include "bf3/core/gameplay/weapon_format.h"

#include <cmath>

namespace bf3::godot_side {

using namespace bf3::core::gameplay;

namespace {

class Rows {
public:
	explicit Rows(std::vector<M0CheckRow> &r_rows) :
			rows_(r_rows) {}

	// Data values are f32: compared with a tolerance far below the printed precision.
	void num(const std::string &p_check, double p_actual, double p_expected, double p_eps = 1e-4) {
		rows_.push_back({ p_check, std::fabs(p_actual - p_expected) <= p_eps, format_number(p_actual), format_number(p_expected) });
	}
	void text(const std::string &p_check, const std::string &p_actual, const std::string &p_expected) {
		rows_.push_back(m0_text_row(p_check, p_actual, p_expected));
	}

private:
	std::vector<M0CheckRow> &rows_;
};

const SwayState &stand(const WeaponDef &p_w, Zoom p_zoom) {
	return p_w.sway.state[size_t(Stance::Stand)][size_t(p_zoom)];
}

} // namespace

M0CheckRow m0_text_row(const std::string &p_check, const std::string &p_actual, const std::string &p_expected) {
	return { p_check, p_actual == p_expected, p_actual.empty() ? std::string("\"\"") : p_actual, p_expected };
}

std::vector<M0CheckRow> check_m0_weapons(const WeaponDef &p_a3, const std::string &p_a3_name, const WeaponDef &p_a4, const std::string &p_a4_name) {
	std::vector<M0CheckRow> out;
	Rows r(out);

	// M16A3 (§7.1 row 1): auto + single, 800 RPM (75 ms), mag 31 x 5, reload 1.8/2.37 s, v0 650,
	// 25 -> 18.4 over 8-50 m, g -15, hip 2.5-7 +0.1, ADS 0.2-3 +0.1, recoil 0.26 up, horizontal
	// -0.4..0.1, dec 18, FSRM 2.5, deploy 0.67.
	const FireFunction &f = p_a3.fire;
	const ProjectileDef &p = p_a3.projectile;
	r.text("m16a3.blueprint", p_a3.blueprint, "weapons/m16a4/m16a4");
	r.text("m16a3.variant", p_a3.variant, "weapons/m16a4/u_m16a4");
	r.text("m16a3.name_sid", p_a3.name_sid, "ID_P_WNAME_M16A4");
	r.text("m16a3.name", p_a3_name, "M16A3");
	r.text("m16a3.fire_modes", format_fire_modes(f), "fltAutomaticFire + [fltSingleFire]");
	r.num("m16a3.rate_of_fire", f.rate_of_fire, 800);
	r.num("m16a3.shot_interval_ms", shot_interval(f) * 1000.0, 75);
	r.num("m16a3.magazine", f.magazine_capacity, 31);
	r.num("m16a3.magazines", f.number_of_magazines, 5);
	r.num("m16a3.reload_tactical", f.reload_time_bullets_left, 1.8);
	r.num("m16a3.reload_empty", f.reload_time, 2.37);
	r.num("m16a3.v0", muzzle_speed(p_a3), 650);
	r.num("m16a3.damage_start", p.start_damage, 25);
	r.num("m16a3.damage_end", p.end_damage, 18.4);
	r.num("m16a3.falloff_start", p.damage_falloff_start, 8);
	r.num("m16a3.falloff_end", p.damage_falloff_end, 50);
	r.num("m16a3.gravity", p.gravity, -15);
	const SwayState &hip = stand(p_a3, Zoom::NoZoom);
	const SwayState &ads = stand(p_a3, Zoom::Zoom);
	r.num("m16a3.hip_min", hip.move[0].min_angle, 2.5);
	r.num("m16a3.hip_max", hip.move[0].max_angle, 7);
	r.num("m16a3.hip_increase", hip.move[0].increase_per_shot, 0.1);
	r.num("m16a3.ads_min", ads.move[0].min_angle, 0.2);
	r.num("m16a3.ads_max", ads.move[0].max_angle, 3);
	r.num("m16a3.ads_increase", ads.move[0].increase_per_shot, 0.1);
	r.num("m16a3.recoil_up", ads.recoil.up_per_shot, 0.26);
	r.num("m16a3.recoil_horizontal_min", ads.recoil.horizontal_min, -0.4);
	r.num("m16a3.recoil_horizontal_max", ads.recoil.horizontal_max, 0.1);
	r.num("m16a3.recoil_decrease", ads.recoil.decrease_factor, 18);
	r.num("m16a3.first_shot_recoil_multiplier", p_a3.sway.first_shot_recoil_multiplier, 2.5);
	r.num("m16a3.deploy", p_a3.deploy_time, 0.67);

	// M16A4 burst (§7.1 row 2): 3 per burst at 800 RPM; = M16A3 x the burst modifier (ADS x0.5 -> 0.1,
	// horizontal x0.75 -> 0.075 left / 0.3 right).
	const FireFunction &b = p_a4.fire;
	const SwayState &bads = stand(p_a4, Zoom::Zoom);
	r.text("m16a4_burst.blueprint", p_a4.blueprint, "weapons/m16a4/m16a4");
	r.text("m16a4_burst.variant", p_a4.variant, "weapons/m16a4/u_m16_burst");
	r.text("m16a4_burst.name_sid", p_a4.name_sid, "ID_P_WNAME_M16");
	r.text("m16a4_burst.name", p_a4_name, "M16A4");
	r.text("m16a4_burst.fire_modes", format_fire_modes(b), "fltBurstFire + [fltSingleFire]");
	r.num("m16a4_burst.bullets_per_burst", b.bullets_per_burst, 3);
	r.num("m16a4_burst.rate_of_fire_for_burst", b.rate_of_fire_for_burst, 800);
	r.num("m16a4_burst.rate_of_fire", b.rate_of_fire, 800);
	r.num("m16a4_burst.ads_min", bads.move[0].min_angle, 0.1);
	r.num("m16a4_burst.recoil_left", bads.recoil.horizontal_max, 0.075);
	r.num("m16a4_burst.recoil_right", -bads.recoil.horizontal_min, 0.3);
	r.num("m16a4_burst.recoil_up", bads.recoil.up_per_shot, 0.26);
	r.num("m16a4_burst.hip_min", stand(p_a4, Zoom::NoZoom).move[0].min_angle, 2.5);
	r.num("m16a4_burst.v0", muzzle_speed(p_a4), 650);
	r.num("m16a4_burst.reload_empty", b.reload_time, 2.37);
	r.num("m16a4_burst.damage_start", p_a4.projectile.start_damage, 25);
	return out;
}

} // namespace bf3::godot_side
