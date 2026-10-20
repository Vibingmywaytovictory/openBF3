#include <doctest/doctest.h>

#include "test_support.h"
#include "weapons_support.h"

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/swf_layout.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/ui/text_db.h"
#include "bf3/host/hud_feed.h"
#include "bf3/sim/weapons/weapon_install.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace ui = bf3::core::ui;
namespace gp = bf3::core::gameplay;
namespace sw = bf3::sim::weapons;
namespace host = bf3::host;
namespace tw = bf3::test::weapons;

namespace {

void row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	std::printf("HUD ROW %s %s %s\n", p_name.c_str(), p_pass ? "PASS" : "FAIL", p_detail.c_str());
	std::fflush(stdout);
}

std::string fmt(const char *p_format, double p_a, double p_b = 0, double p_c = 0) {
	char buf[256];
	std::snprintf(buf, sizeof(buf), p_format, p_a, p_b, p_c);
	return buf;
}

struct SyntheticKit {
	std::vector<gp::WeaponDef> defs;
	std::shared_ptr<sw::WeaponTable> table;
};

SyntheticKit synthetic_kit() {
	struct Pick {
		const char *variant;
		uint32_t id;
		const char *weapon_class;
		const char *anim;
		const char *sid;
	};
	const Pick picks[] = {
		{ "weapons/m16a4/u_m16a4", 2580429694u, "wc556x45mmNATO", "wabsRif", "ID_P_WNAME_M16A4" },
		{ "weapons/an94/u_an94", 1001u, "wc545x45mmWP", "wabsRif", "ID_P_WNAME_AN94" },
		{ "weapons/remington870/u_870", 1002u, "wc12gauge", "wabsShg", "ID_P_WNAME_870" },
		{ "weapons/m9/u_m9", 287899934u, "wc9x19mm", "wabsPstl", "ID_P_WNAME_M9" },
		{ "weapons/rpg7/u_rpg7", 1003u, "wcRocket", "wabsRpg", "ID_P_WNAME_RPG7" },
		{ "weapons/m67/u_m67", 1049314494u, "wcHgr", "wabsHgr", "" },
		{ "weapons/knife/u_knife", 2235352958u, "wcKnife", "wabsHgr", "ID_P_WNAME_knife" },
	};
	SyntheticKit k;
	auto materials = std::make_shared<const sw::SurfaceMaterialTable>();
	k.table = std::make_shared<sw::WeaponTable>();
	k.table->materials = materials;
	for (const Pick &p : picks) {
		for (tw::Synthetic s : tw::synthetic_gate_weapons()) {
			if (s.def.variant != p.variant) {
				continue;
			}
			s.def.unlock_identifier = p.id;
			s.def.weapon_class = p.weapon_class;
			s.def.anim_base_set = p.anim;
			s.def.name_sid = p.sid;
			s.def.name_sid_hash = s.def.name_sid.empty() ? 0 : ui::text_hash(s.def.name_sid);
			k.defs.push_back(s.def);
			k.table->defs.push_back(tw::sim_def(s, *materials));
			break;
		}
	}
	REQUIRE(k.defs.size() == std::size(picks));
	k.table->hash = sw::weapon_table_hash(*k.table);
	return k;
}

bf3::sim::Pose sim_pose_of(int p_frame) {
	return bf3::sim::Pose((p_frame / 100) % 3);
}

uint16_t def_of(const SyntheticKit &p_k, const std::string &p_variant) {
	for (size_t i = 0; i < p_k.defs.size(); ++i) {
		if (p_k.defs[i].variant == p_variant) {
			return uint16_t(i);
		}
	}
	FAIL("no def " << p_variant);
	return 0;
}

host::HudStatic synthetic_static(const SyntheticKit &p_k) {
	bf3::core::Result<host::HudStatic> st = host::make_hud_static(*p_k.table, p_k.defs);
	REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
	return *st;
}

host::PresentEvent hit_event(uint8_t p_flags, int64_t p_time) {
	host::PresentEvent e;
	e.kind = host::PresentKind::Hit;
	e.timeline = host::PresentTimeline::Immediate;
	e.flags = p_flags;
	e.time = p_time;
	e.value = 25.0f;
	return e;
}

ui::SwfLayoutNode text_node(const char *p_instance, float p_x, float p_y, float p_size, uint8_t p_align, const char *p_initial,
		ui::SwfRect p_bounds) {
	ui::SwfLayoutNode n;
	n.instance = p_instance;
	n.kind = ui::SwfLayoutKind::EditText;
	n.stage.tx = p_x;
	n.stage.ty = p_y;
	n.text.font = "$HUD_medium";
	n.text.size = p_size;
	n.text.align = p_align;
	n.text.initial_text = p_initial;
	n.text.bounds = p_bounds;
	n.text.color = { 255, 255, 255, 255 };
	return n;
}

ui::SwfLayoutNode sprite(const char *p_instance, float p_x, float p_y, std::vector<ui::SwfLayoutNode> p_children = {}) {
	ui::SwfLayoutNode n;
	n.instance = p_instance;
	n.kind = ui::SwfLayoutKind::Sprite;
	n.stage.tx = p_x;
	n.stage.ty = p_y;
	n.children = std::move(p_children);
	return n;
}

ui::SwfLayoutNode synthetic_screen() {
	ui::SwfLayoutNode clip_txt = text_node("txtDisplay", 1094, 625, 43, 1, "000", { -2, 102, -2, 48 });
	ui::SwfLayoutNode res_txt = text_node("txtDisplay", 1199, 648, 21, 1, "000", { -2, 60, -2, 26 });
	ui::SwfLayoutNode slash = text_node("", 1194, 649, 18, 0, "/", { -2, 12, -2, 22 });
	ui::SwfLayoutNode ammo = sprite("Ammo_01", 1194, 669,
			{ sprite("Ammo", 1194, 669,
					{ sprite("", 1112, 647, { slash }), sprite("clip", 1196, 623, { sprite("cltxt", 1196, 623, { clip_txt }) }),
							sprite("amholder", 1197, 646, { sprite("amtxt", 1248, 646, { res_txt }) }) }) });
	ui::SwfLayoutNode hp_txt = text_node("txtDisplay", 1200.65f, 588, 21, 1, "100", { -2, 52, -2, 26 });
	ui::SwfLayoutNode pct = text_node("", 1253.66f, 593, 16, 0, "%", { -2, 14, -2, 20 });
	ui::SwfLayoutNode health = sprite("Health_01", 1193, 605, { sprite("Health", 1193, 605, { sprite("healthHolder", 1107, 583, { sprite("amtxt", 1198, 586, { pct, hp_txt }) }) }) });
	ui::SwfLayoutNode hit = sprite("Hitindicator_01", 640, 360, { sprite("hitindicator", 640, 360) });
	ui::SwfLayoutNode root = sprite("", 0, 0, { sprite("instance1", 0, 0, { sprite("LatencyIndicator_01", 1274, 3), hit, ammo, health }) });
	root.symbol = "_root";
	return root;
}

}

TEST_SUITE("ui-hud-feed-unit") {

TEST_CASE("static rows from the table and its defs") {
	const SyntheticKit k = synthetic_kit();
	const host::HudStatic st = synthetic_static(k);
	REQUIRE(st.rows.size() == k.defs.size());
	const host::HudStaticRow &m16 = st.rows[def_of(k, "weapons/m16a4/u_m16a4")];
	CHECK(m16.item_id == 2580429694u);
	CHECK(m16.name_sid_hash == ui::text_hash("ID_P_WNAME_M16A4"));
	CHECK(m16.name_sid_hash == 0x0fd83dfau);
	CHECK(m16.clip_capacity == 31);
	CHECK(m16.magazines == 5);
	CHECK(m16.crosshair == host::HudCrosshairKind::Rifle);
	CHECK(m16.show_ammo);
	CHECK(m16.mode_kinds[0] == host::HudFireMode::Auto);
	CHECK(m16.mode_kinds[1] == host::HudFireMode::Single);
	CHECK(m16.mode_kinds[2] == host::HudFireMode::None);
	CHECK(m16.fire_modes == ((1u << 2) | (1u << 0)));
	CHECK(m16.weapon_class == host::hud_weapon_class("wc556x45mmNATO"));
	CHECK(m16.weapon_class == 1);
	CHECK(st.rows[def_of(k, "weapons/an94/u_an94")].mode_kinds[0] == host::HudFireMode::Burst);
	CHECK(st.rows[def_of(k, "weapons/remington870/u_870")].crosshair == host::HudCrosshairKind::Shotgun);
	CHECK(st.rows[def_of(k, "weapons/remington870/u_870")].mode_kinds[0] == host::HudFireMode::Single);
	CHECK(st.rows[def_of(k, "weapons/m9/u_m9")].crosshair == host::HudCrosshairKind::Pistol);
	CHECK(st.rows[def_of(k, "weapons/rpg7/u_rpg7")].crosshair == host::HudCrosshairKind::Launcher);
	const host::HudStaticRow &m67 = st.rows[def_of(k, "weapons/m67/u_m67")];
	CHECK(m67.crosshair == host::HudCrosshairKind::Grenade);
	CHECK(m67.fire_modes == 0);
	CHECK(m67.name_sid_hash == 0);
	const host::HudStaticRow &knife = st.rows[def_of(k, "weapons/knife/u_knife")];
	CHECK(knife.crosshair == host::HudCrosshairKind::Knife);
	CHECK_FALSE(knife.show_ammo);
	CHECK(knife.unlimited_ammo);
	CHECK(host::hud_weapon_class("wcSomethingNew") == 0);
	CHECK(st.row(uint16_t(st.rows.size())) == nullptr);
	CHECK(st.row(sw::kNoWeapon) == nullptr);

	std::vector<gp::WeaponDef> fewer(k.defs.begin(), k.defs.end() - 1);
	CHECK_FALSE(host::make_hud_static(*k.table, fewer).ok());
	std::vector<gp::WeaponDef> swapped = k.defs;
	std::swap(swapped[0], swapped[1]);
	bf3::core::Result<host::HudStatic> bad = host::make_hud_static(*k.table, swapped);
	CHECK_FALSE(bad.ok());
	if (!bad.ok()) {
		CHECK(bad.error().message.find("u_m16a4") != std::string::npos);
	}
	row("static.rows", true, std::to_string(st.rows.size()) + " synthetic rows: kinds, modes, capacities, refusals");
}

TEST_CASE("fill_hud over scripted frames: ammo every frame, reload, ADS, switch, death") {
	const SyntheticKit k = synthetic_kit();
	const host::HudStatic st = synthetic_static(k);
	const uint16_t m16 = def_of(k, "weapons/m16a4/u_m16a4");
	const uint16_t m9 = def_of(k, "weapons/m9/u_m9");
	const uint16_t knife = def_of(k, "weapons/knife/u_knife");
	host::HudFeedState state;
	ui::HudModel model;
	size_t frames = 0;
	size_t mismatches = 0;
	int32_t magazine = 31;
	int32_t reserve = 124;
	uint64_t serial_before = model.serial;
	for (int f = 0; f < 600; ++f) {
		host::HudFeedInput in;
		in.now_s = f / 60.0;
		in.alive = f < 560;
		in.health = 100.0f - float(f) * 0.1f;
		in.pose = sim_pose_of(f);
		host::PresentedWeapon &w = in.weapon;
		w.valid = in.alive;
		w.def = (f >= 300 && f < 400) ? m9 : (f >= 400 && f < 450 ? knife : m16);
		w.active_slot = w.def == m9 ? 1 : (w.def == knife ? 7 : 0);
		w.magazine_capacity = w.def == m9 ? 16 : (w.def == knife ? 1 : 31);

		if (w.def == m16 && f >= 20 && f < 200 && f % 4 == 0 && magazine > 0) {
			--magazine;
		}
		w.phase = (f >= 200 && f < 260) ? sw::WeaponPhase::Reloading : sw::WeaponPhase::Ready;
		w.phase_progress = w.phase == sw::WeaponPhase::Reloading ? float(f - 200) / 60.0f : 0.0f;
		if (f == 240) {
			const int32_t take = std::min(reserve, 31 - magazine);
			reserve -= take;
			magazine += take;
		}
		w.magazine = w.def == m9 ? 16 - (f - 300) / 10 : (w.def == knife ? 1 : magazine);
		w.reserve = w.def == m9 ? 64 : (w.def == knife ? 0 : reserve);
		w.zoomed = f >= 100 && f < 150;
		w.spread_deg = w.zoomed ? 0.2f : 2.5f + float(f % 40) * 0.1f;
		w.mode = f >= 500 ? 1 : 0;
		host::fill_hud(in, {}, st, state, model);
		++frames;
		const ui::HudWeapon &m = model.weapon;
		bool ok = model.serial == serial_before + 1;
		serial_before = model.serial;
		if (in.alive) {
			const host::HudStaticRow &r = *st.row(w.def);
			ok = ok && m.active_weapon_ammo == w.magazine && m.active_weapon_ammo_in_bag == w.reserve &&
					m.active_weapon_ammo_total == w.magazine + w.reserve && m.active_weapon_clip_capacity == w.magazine_capacity &&
					m.active_weapon_clips_left == (w.reserve + w.magazine_capacity - 1) / w.magazine_capacity &&
					m.active_weapon_name == r.name_sid_hash && m.active_weapon_data.item_id == r.item_id &&
					m.active_weapon_show_ammo == r.show_ammo && m.zoomed == w.zoomed && m.active_weapon_zoomed == w.zoomed &&
					m.show_crosshair == !w.zoomed && m.dispersion == w.spread_deg && m.active_weapon_index == w.active_slot &&
					m.active_weapon_is_reloading == (w.phase == sw::WeaponPhase::Reloading) &&
					std::fabs(m.active_weapon_reload_percent - (w.phase == sw::WeaponPhase::Reloading ? w.phase_progress * 100.0f : 0.0f)) < 1e-4f &&
					m.can_fire == (w.phase == sw::WeaponPhase::Ready) && m.firing_mode == int32_t(r.mode_kinds[w.mode]) &&
					m.firing_mode_types == r.fire_modes && m.active_weapon_critical_ammo == (r.show_ammo && w.magazine_capacity > 1 &&
																			   float(w.magazine) <= float(w.magazine_capacity) * host::kCriticalAmmoFraction) &&
					model.soldier.is_alive && model.soldier.health == in.health && model.soldier.pose == int32_t(in.pose) &&
					model.soldier.show_hud_ammo == r.show_ammo;
		} else {
			ok = ok && m.active_weapon_ammo == 0 && !m.active_weapon_show_ammo && !m.show_crosshair && m.active_weapon_name == 0 &&
					!model.soldier.is_alive;
		}
		mismatches += ok ? 0 : 1;
	}
	CHECK(mismatches == 0);
	CHECK(state.frames == frames);
	CHECK(magazine == 31);
	CHECK(reserve < 124);
	row("feed.ammo", mismatches == 0, std::to_string(frames) + " frames, " + std::to_string(mismatches) + " mismatches (magazine / reserve / name / ADS / reload)");
}

TEST_CASE("hit indicator: 0.3 s per own server hit, level 1, others ignored") {
	const SyntheticKit k = synthetic_kit();
	const host::HudStatic st = synthetic_static(k);
	host::HudFeedState state;
	ui::HudModel model;

	const auto events_at = [](int p_f) {
		std::vector<host::PresentEvent> out;
		if (p_f == 30 || p_f == 40 || p_f == 120) {
			out.push_back(hit_event(host::present_flags::kOwnShooter, p_f));
		}
		if (p_f == 80) {
			out.push_back(hit_event(host::present_flags::kOwnVictim, p_f));
		}
		if (p_f == 200) {
			out.push_back(hit_event(0, p_f));
		}
		if (p_f == 90) {
			host::PresentEvent d;
			d.kind = host::PresentKind::Death;
			d.flags = host::present_flags::kOwnShooter;
			out.push_back(d);
		}
		return out;
	};
	double last_own = -1e9;
	size_t mismatches = 0;
	size_t shown_frames = 0;
	size_t raised_on_frame = 0;

	for (int f = 0; f < 300; ++f) {
		host::HudFeedInput in;
		in.now_s = f / 64.0;
		in.alive = true;
		in.health = 100;
		in.weapon.valid = true;
		in.weapon.def = 0;
		in.weapon.magazine = 31;
		in.weapon.magazine_capacity = 31;
		const std::vector<host::PresentEvent> ev = events_at(f);
		for (const host::PresentEvent &e : ev) {
			if (e.kind == host::PresentKind::Hit && (e.flags & host::present_flags::kOwnShooter) != 0) {
				last_own = in.now_s;
			}
		}
		host::fill_hud(in, ev, st, state, model);
		const bool expect = in.now_s < last_own + host::kHitIndicatorSeconds;
		mismatches += model.weapon.show_hitindicator == expect ? 0 : 1;
		mismatches += model.weapon.hitindicator_level == (expect ? 1.0f : 0.0f) ? 0 : 1;
		shown_frames += expect ? 1 : 0;
		if (in.now_s == last_own && model.weapon.show_hitindicator) {
			++raised_on_frame;
		}
	}
	CHECK(mismatches == 0);
	CHECK(state.hits_shown == 3);
	CHECK(state.hits_ignored == 2);
	CHECK(raised_on_frame == 3);

	CHECK(shown_frames == 50);
	row("feed.hit_indicator", mismatches == 0 && state.hits_shown == 3,
			"3 own hits shown (" + std::to_string(shown_frames) + " frames at 64 Hz, 0.3 s each, restarted once), 2 others ignored, " +
					std::to_string(mismatches) + " mismatches");
}

TEST_CASE("hostile feed inputs are clamped") {
	const SyntheticKit k = synthetic_kit();
	const host::HudStatic st = synthetic_static(k);
	host::HudFeedState state;
	ui::HudModel model;
	host::HudFeedInput in;
	in.alive = true;
	in.health = std::nanf("");
	in.weapon.valid = true;
	in.weapon.def = 0;
	in.weapon.magazine = -5;
	in.weapon.reserve = -1;
	in.weapon.magazine_capacity = 0;
	in.weapon.spread_deg = std::nanf("");
	in.weapon.phase = sw::WeaponPhase::Reloading;
	in.weapon.phase_progress = 7.0f;
	in.weapon.mode = 200;
	host::fill_hud(in, {}, st, state, model);
	CHECK(model.weapon.active_weapon_ammo == 0);
	CHECK(model.weapon.active_weapon_ammo_in_bag == 0);
	CHECK(model.weapon.active_weapon_clip_capacity == 31);
	CHECK(model.weapon.dispersion == 0.0f);
	CHECK(model.weapon.active_weapon_reload_percent == 100.0f);
	CHECK(model.weapon.firing_mode == int32_t(host::HudFireMode::None));
	CHECK(model.soldier.health == 0.0f);
	in.weapon.def = 999;
	host::fill_hud(in, {}, st, state, model);
	CHECK_FALSE(model.weapon.active_weapon_show_ammo);
	CHECK_FALSE(model.weapon.show_crosshair);
	CHECK(model.weapon.active_weapon_name == 0);
	row("feed.hostile", true, "NaN spread / health, negative ammo, a mode index 200 and a def outside the table: clamped or cleared");
}

TEST_CASE("crosshair gap = (h/2) tan(spread) / tan(vfov/2)") {
	constexpr double kPi = 3.14159265358979323846;
	const double g = host::crosshair_gap_px(1080, 1.0, 55.0);
	CHECK(g == doctest::Approx(540.0 * std::tan(kPi / 180.0) / std::tan(27.5 * kPi / 180.0)).epsilon(1e-12));
	CHECK(g == doctest::Approx(18.1067).epsilon(1e-4));

	CHECK(host::crosshair_gap_px(1440, 1.0, 55.0) == doctest::Approx(g * 1440.0 / 1080.0));
	CHECK(host::crosshair_gap_px(1080, 1.0, 75.0) < g);
	CHECK(host::crosshair_gap_px(1080, 2.0, 55.0) > g);
	CHECK(host::crosshair_gap_px(720, 0.0, 55.0) == 0.0);

	CHECK(host::crosshair_gap_px(720, 27.5, 55.0) == doctest::Approx(360.0));

	CHECK(host::crosshair_gap_px(0, 1, 55) == 0.0);
	CHECK(host::crosshair_gap_px(720, -1, 55) == 0.0);
	CHECK(host::crosshair_gap_px(720, std::nan(""), 55) == 0.0);
	CHECK(host::crosshair_gap_px(720, 1, 0) == 0.0);
	CHECK(host::crosshair_gap_px(720, 1, 180) == 0.0);
	CHECK(std::isfinite(host::crosshair_gap_px(720, 1e9, 55)));
	row("crosshair.gap", true, fmt("h 1080, spread 1 deg, vfov 55: %.4f px; scaling and refusals", g));
}

TEST_CASE("stage to window: min(w/1280, h/720) around the widget's anchor") {
	using ui::HudAnchor;

	for (const auto &[w, h] : { std::pair{ 1280.0f, 720.0f }, std::pair{ 1920.0f, 1080.0f }, std::pair{ 2560.0f, 1440.0f } }) {
		const float s = host::hud_scale(w, h);
		CHECK(s == doctest::Approx(w / 1280.0f));
		for (int a = 0; a < 9; ++a) {
			const host::HudPoint p = host::hud_stage_to_window(HudAnchor(a), 1194, 669, w, h);
			CHECK(p.x == doctest::Approx(1194.0f * s));
			CHECK(p.y == doctest::Approx(669.0f * s));
		}
	}

	const float s43 = host::hud_scale(1024, 768);
	CHECK(s43 == doctest::Approx(0.8f));
	const host::HudPoint ammo = host::hud_stage_to_window(HudAnchor::BottomRight, 1194, 669, 1024, 768);
	CHECK(ammo.x == doctest::Approx(1024.0f - 86.0f * 0.8f));
	CHECK(ammo.y == doctest::Approx(768.0f - 51.0f * 0.8f));
	const host::HudPoint centre = host::hud_stage_to_window(HudAnchor::Centre, 640, 360, 1024, 768);
	CHECK(centre.x == doctest::Approx(512.0f));
	CHECK(centre.y == doctest::Approx(384.0f));

	const host::HudPoint tl = host::hud_stage_to_window(HudAnchor::TopLeft, 10, 3, 3440, 1440);
	CHECK(tl.x == doctest::Approx(20.0f));
	CHECK(tl.y == doctest::Approx(6.0f));
	CHECK(host::hud_scale(0, 720) == 0.0f);
	row("layout.scale", true, "16:9 at 1280x720 / 1920x1080 / 2560x1440 = stage x scale for every anchor; 4:3 and 21:9 keep the anchor offsets");
}

TEST_CASE("screen layout: the hudmpscreen slots of a synthetic tree") {
	const ui::SwfLayoutNode root = synthetic_screen();
	gp::WorkBudget budget;
	bf3::core::Result<host::HudScreenLayout> l = host::make_hud_screen_layout(root, budget);
	REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
	CHECK(l->missing.empty());
	CHECK(l->ammo.found);
	CHECK(l->ammo.x == 1194.0f);
	CHECK(l->ammo.y == 669.0f);
	CHECK(l->ammo.anchor == ui::HudAnchor::BottomRight);
	CHECK(l->hit_indicator.anchor == ui::HudAnchor::Centre);
	CHECK(l->health.anchor == ui::HudAnchor::BottomRight);
	CHECK(l->ammo_clip.found);
	CHECK(l->ammo_clip.size == 43.0f);
	CHECK(l->ammo_clip.align == 1);
	CHECK(l->ammo_clip.font == "$HUD_medium");
	CHECK(l->ammo_clip.path == "Ammo_01/Ammo/clip/cltxt/txtDisplay");
	CHECK(l->ammo_reserve.size == 21.0f);
	CHECK(l->ammo_reserve.x == 1199.0f);
	CHECK(l->ammo_slash.found);
	CHECK(l->ammo_slash.initial_text == "/");
	CHECK(l->health_value.found);
	CHECK(l->health_percent.initial_text == "%");
	CHECK(l->nodes > 0);

	const host::HudTextAnchor a = host::hud_text_anchor(l->ammo_clip, 0.8f);
	CHECK(a.x == doctest::Approx(1094.0f + 100.0f));
	CHECK(a.baseline == doctest::Approx(625.0f + 0.0f + 0.8f * 43.0f));
	CHECK(a.size == 43.0f);
	host::HudTextSlot left = l->ammo_slash;
	const host::HudTextAnchor b = host::hud_text_anchor(left, 1.0f);
	CHECK(b.x == doctest::Approx(1194.0f + 0.0f));
	left.align = 2;
	CHECK(host::hud_text_anchor(left, 1.0f).x == doctest::Approx(1194.0f + 5.0f));

	ui::SwfLayoutNode partial = root;
	partial.children[0].children.erase(partial.children[0].children.begin() + 3);
	gp::WorkBudget b2;
	bf3::core::Result<host::HudScreenLayout> p = host::make_hud_screen_layout(partial, b2);
	REQUIRE(p.ok());
	CHECK_FALSE(p->health.found);
	CHECK(std::find(p->missing.begin(), p->missing.end(), "Health_01") != p->missing.end());
	CHECK(p->ammo_clip.found);
	ui::SwfLayoutNode empty;
	gp::WorkBudget b3;
	CHECK_FALSE(host::make_hud_screen_layout(empty, b3).ok());
	row("layout.slots", l->missing.empty(), std::to_string(l->nodes) + " nodes; Ammo_01 (1194, 669) bottom right, clip 43 px right-aligned");
}

TEST_CASE("screen layout: hostile trees fail with an error") {

	ui::SwfLayoutNode root = synthetic_screen();
	ui::SwfLayoutNode &ammo = root.children[0].children[2];
	ammo.children[0].children.erase(ammo.children[0].children.begin());
	ui::SwfLayoutNode chain = sprite("deep", 0, 0);
	for (int i = 0; i < 200; ++i) {
		ui::SwfLayoutNode parent = sprite("deep", 0, 0);
		parent.children.push_back(std::move(chain));
		chain = std::move(parent);
	}
	ammo.children.push_back(std::move(chain));
	gp::WorkBudget budget;
	bf3::core::Result<host::HudScreenLayout> l = host::make_hud_screen_layout(root, budget);
	CHECK_FALSE(l.ok());

	ui::SwfLayoutNode wide = synthetic_screen();
	for (int i = 0; i < 5000; ++i) {
		wide.children[0].children.insert(wide.children[0].children.begin(), sprite("filler", 0, 0));
	}
	gp::WorkBudget small(1000);
	bf3::core::Result<host::HudScreenLayout> w = host::make_hud_screen_layout(wide, small);
	CHECK_FALSE(w.ok());
	gp::WorkBudget enough;
	CHECK(host::make_hud_screen_layout(wide, enough).ok());
	row("layout.hostile", !l.ok() && !w.ok(), std::string("depth 200 chain: ") + (l.ok() ? "accepted" : l.error().message) + "; 5000 siblings on 1000 units: " +
			(w.ok() ? "accepted" : w.error().message));
}

}

namespace {

struct KitItem {
	const char *variant;
	const char *name;
};

constexpr KitItem kKitItems[] = {
	{ "weapons/m16a4/u_m16a4", "M16A3" },
	{ "weapons/m4a1/u_m4a1", "M4A1" },
	{ "weapons/m27iar/u_m27iar", "M27 IAR" },
	{ "weapons/mk11/u_mk11", "MK11 MOD 0" },
	{ "weapons/ak74m/u_ak74m", "AK-74M" },
	{ "weapons/aks74u/u_aks74u", "AKS-74u" },
	{ "weapons/rpk/u_rpk-74m", "RPK-74M" },
	{ "weapons/svd/u_svd", "SVD" },
	{ "weapons/m9/u_m9", "M9" },
	{ "weapons/mp443/u_mp443", "MP443" },
	{ "weapons/knife/u_knife", "KNIFE" },
	{ "weapons/m67/u_m67", "M67 GRENADE" },
};

struct HudInstall {
	bf3::core::InstallReader reader;
	bf3::core::InstallSnapshot before;
	std::shared_ptr<bf3::core::Install> install;
};

HudInstall *hud_install() {
	static HudInstall *s = [] {
		HudInstall *h = new HudInstall();
		if (!h->reader.locate({})) {
			return h;
		}
		h->before = h->reader.snapshot();
		bf3::core::Result<std::shared_ptr<bf3::core::Install>> opened = bf3::core::Install::open(h->reader);
		if (opened) {
			h->install = *opened;
		}
		return h;
	}();
	if (s->install == nullptr) {
		bf3::test::skip_install_case(s->reader.describe().c_str());
		return nullptr;
	}
	return s;
}

}

TEST_SUITE("ui-hud-feed-install") {

TEST_CASE("the 12 kit items: names, icons, static rows") {
	HudInstall *hi = hud_install();
	if (hi == nullptr) {
		return;
	}
	bf3::core::Result<sw::InstallWeapons> weapons = sw::open_install_weapons(hi->install);
	REQUIRE_MESSAGE(weapons.ok(), (weapons.ok() ? std::string() : weapons.error().message));
	std::vector<gp::WeaponDef> defs;
	for (const KitItem &k : kKitItems) {
		bf3::core::Result<gp::WeaponDef> d = weapons->catalog->resolve(k.variant);
		REQUIRE_MESSAGE(d.ok(), (std::string(k.variant) + ": " + (d.ok() ? std::string() : d.error().message)));
		defs.push_back(std::move(*d));
	}
	bf3::core::Result<std::shared_ptr<const sw::WeaponTable>> table = sw::make_weapon_table(defs, weapons->materials, sw::install_table_options(*weapons));
	REQUIRE_MESSAGE(table.ok(), (table.ok() ? std::string() : table.error().message));
	bf3::core::Result<host::HudStatic> st = host::make_hud_static(**table, defs);
	REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
	bf3::core::Result<std::unique_ptr<bf3::core::EbxLibrary>> ebx = bf3::core::EbxLibrary::open(hi->install, { "Win32/UI" });
	REQUIRE_MESSAGE(ebx.ok(), (ebx.ok() ? std::string() : ebx.error().message));
	bf3::core::Result<ui::ItemIconIndex> items = ui::ItemIconIndex::read(**ebx);
	REQUIRE_MESSAGE(items.ok(), (items.ok() ? std::string() : items.error().message));
	const size_t filled = host::apply_item_names(*st, *items);
	CHECK(filled == 1);
	bf3::core::Result<ui::TextDatabase> text = ui::open_text_database(hi->install);
	REQUIRE_MESSAGE(text.ok(), (text.ok() ? std::string() : text.error().message));
	size_t names = 0;
	size_t icons = 0;
	for (size_t i = 0; i < std::size(kKitItems); ++i) {
		const host::HudStaticRow &r = st->rows[i];
		const std::optional<std::string_view> name = text->find(r.name_sid_hash);
		const bool name_ok = name && *name == kKitItems[i].name;
		names += name_ok ? 1 : 0;
		const ui::ItemIcon *icon = items->find(r.item_id);
		bool icon_ok = icon != nullptr && icon->res_name.size() > 6 && icon->res_name.ends_with("_small");
		std::string icon_detail = icon != nullptr ? icon->res_name : std::string("no description");
		if (icon_ok) {
			bf3::core::Result<ui::ItemIconImage> img = ui::load_item_icon((*ebx)->mount(0), *icon);
			icon_ok = img.ok() && img->width > 0 && img->height > 0 && !img->data.empty();
			icon_detail += img.ok() ? " " + std::to_string(img->width) + "x" + std::to_string(img->height) : ": " + img.error().message;
		}
		icons += icon_ok ? 1 : 0;
		row(std::string("kit.") + kKitItems[i].variant, name_ok && icon_ok,
				"name '" + (name ? std::string(*name) : std::string("?")) + "' (want '" + kKitItems[i].name + "'), icon " + icon_detail + ", crosshair " +
						std::to_string(int(r.crosshair)) + ", clip " + std::to_string(r.clip_capacity) + " x " + std::to_string(r.magazines));
		CHECK_MESSAGE(name_ok, kKitItems[i].variant);
		CHECK_MESSAGE(icon_ok, kKitItems[i].variant);
	}
	CHECK(st->rows[11].name_sid_hash == 0x26436abdu);
	CHECK(st->rows[10].crosshair == host::HudCrosshairKind::Knife);
	CHECK(st->rows[11].crosshair == host::HudCrosshairKind::Grenade);
	CHECK(st->rows[8].crosshair == host::HudCrosshairKind::Pistol);
	CHECK(st->rows[0].clip_capacity == 31);
	row("kit.names", names == std::size(kKitItems), std::to_string(names) + "/12 localised names (M67 through its UI description)");
	row("kit.icons", icons == std::size(kKitItems), std::to_string(icons) + "/12 _small icons decode");
	CHECK(names == 12);
	CHECK(icons == 12);
}

TEST_CASE("the hudmpscreen slots") {
	HudInstall *hi = hud_install();
	if (hi == nullptr) {
		return;
	}
	bf3::core::Result<ui::SwfLibrary> lib = ui::SwfLibrary::open(hi->install);
	REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
	bf3::core::level::WorkBudget budget(ui::kSwfLayoutBudgetUnits);
	bf3::core::Result<ui::SwfLayoutNode> tree = ui::read_layout(*lib, host::kHudScreenMovie, {}, budget);
	REQUIRE_MESSAGE(tree.ok(), (tree.ok() ? std::string() : tree.error().message));
	gp::WorkBudget lb;
	bf3::core::Result<host::HudScreenLayout> l = host::make_hud_screen_layout(*tree, lb);
	REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
	std::string missing;
	for (const std::string &m : l->missing) {
		missing += " " + m;
	}
	CHECK_MESSAGE(l->missing.empty(), missing);
	CHECK(l->ammo.x == doctest::Approx(1194.0f));
	CHECK(l->ammo.y == doctest::Approx(669.0f));
	CHECK(l->health.x == doctest::Approx(1193.0f));
	CHECK(l->health.y == doctest::Approx(605.0f));
	CHECK(l->hit_indicator.x == doctest::Approx(640.0f));
	CHECK(l->hit_indicator.y == doctest::Approx(360.0f));
	CHECK(l->ammo.anchor == ui::HudAnchor::BottomRight);
	CHECK(l->ammo_clip.font == host::kHudNumberFont);
	CHECK(l->ammo_clip.size == 43.0f);
	CHECK(l->ammo_clip.align == 1);
	CHECK(l->ammo_reserve.size == 21.0f);
	CHECK(l->health_value.size == 21.0f);
	const auto slot = [](const host::HudTextSlot &p_s) {
		return p_s.path + fmt(" at (%.2f, %.2f) %.0f px", p_s.x, p_s.y, p_s.size) + " " + p_s.font + " align " + std::to_string(p_s.align) +
				fmt(" bounds x %.2f..%.2f", p_s.bounds.x_min, p_s.bounds.x_max) + fmt(" y %.2f..%.2f", p_s.bounds.y_min, p_s.bounds.y_max);
	};
	row("screen.ammo", l->ammo.found, fmt("Ammo_01 (%.2f, %.2f) anchor ", l->ammo.x, l->ammo.y) + ui::hud_anchor_name(l->ammo.anchor));
	row("screen.clip", l->ammo_clip.found, slot(l->ammo_clip));
	row("screen.reserve", l->ammo_reserve.found, slot(l->ammo_reserve));
	row("screen.slash", l->ammo_slash.found, slot(l->ammo_slash));
	row("screen.health", l->health_value.found, slot(l->health_value));
	row("screen.percent", l->health_percent.found, slot(l->health_percent));
	row("screen.nodes", l->missing.empty(), std::to_string(l->nodes) + " nodes visited, " + std::to_string(l->missing.size()) + " missing" + missing);
}

TEST_CASE("install.unchanged") {
	HudInstall *hi = hud_install();
	if (hi == nullptr) {
		return;
	}
	const bf3::core::SnapshotDiff d = bf3::core::diff_snapshots(hi->before, hi->reader.snapshot());
	row("install.unchanged", hi->before.valid && d.clean(), std::to_string(hi->before.files.size()) + " index files" + (d.clean() ? "" : ": " + d.summary()));
	CHECK(hi->before.valid);
	CHECK(d.clean());
}

}
