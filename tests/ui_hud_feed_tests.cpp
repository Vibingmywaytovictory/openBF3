#include <doctest/doctest.h>

#include "gunplay_fixture.h"
#include "test_support.h"
#include "weapons_support.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/ui/text_db.h"
#include "bf3/core/util/format.h"
#include "bf3/host/hud_feed.h"
#include "bf3/host/presentation_weapon.h"
#include "bf3/net/weapons_wire.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/weapons/weapon_install.h"
#include "bf3/sim/weapons/weapon_rules.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ui = bf3::core::ui;
namespace gp = bf3::core::gameplay;
namespace sw = bf3::sim::weapons;
namespace host = bf3::host;
namespace tw = bf3::test::weapons;
namespace net = bf3::net;

namespace {

void row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	std::printf("HUD ROW %s %s %s\n", p_name.c_str(), p_pass ? "PASS" : "FAIL", p_detail.c_str());
	std::fflush(stdout);
}

using bf3::core::fmt;

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

struct XhairWindow {
	bf3::sim::Tick from = 0;
	bf3::sim::Tick to = 0;

	bool has(bf3::sim::Tick p_tick) const { return p_tick >= from && p_tick < to; }
};

struct XhairScript {
	XhairWindow walk;
	XhairWindow sprint;
	XhairWindow air;
	XhairWindow fire;
	bf3::sim::Tick last = 0;
};

struct XhairFrame {
	bf3::sim::Tick tick = 0;
	double arm = 0.0;
	uint32_t shots = 0;
	sw::SwayMove move = sw::SwayMove::Base;
	float speed = 0.0f;
};

constexpr bf3::sim::Tick kXhairSpawn = 100;

std::vector<XhairFrame> crosshair_frames(const sw::WeaponTable &p_table, std::span<const uint16_t> p_loadout, const host::HudStatic &p_static, double p_base_offset,
		const XhairScript &p_script, const bf3::sim::SoldierMotor *p_motor = nullptr) {
	namespace sim = bf3::sim;
	sw::SoldierWeaponsState w;
	w.player_slot = 3;
	REQUIRE(sw::spawn_soldier_weapons(w, p_table, p_loadout, 0, kXhairSpawn).ok());
	std::vector<net::WeaponsWire> wires{ net::weapons_wire_view(w, kXhairSpawn) };
	sim::SoldierState before;
	before.flags = sim::soldier_flags::kAlive;
	if (p_motor != nullptr) {
		REQUIRE(p_script.sprint.from == p_script.sprint.to);
		REQUIRE(p_script.air.from == p_script.air.to);
		before = p_motor->spawn_state({ 0.0f, 0.0f, 0.0f }, 0, 1, kXhairSpawn, false);
	}
	std::vector<sim::SoldierState> soldiers{ before };
	std::vector<sim::Command> commands{ sim::Command{} };
	std::vector<uint32_t> shots{ 0 };
	const sim::MotorParams scripted_motor;
	const sim::MotorParams &motor = p_motor != nullptr ? p_motor->params() : scripted_motor;
	for (sim::Tick tick = kXhairSpawn + 1; tick <= p_script.last; ++tick) {
		sim::Command c;
		c.tick = tick;
		c.buttons = p_script.fire.has(tick) ? sim::buttons::kFire : 0u;
		sim::SoldierState after = before;
		if (p_motor != nullptr) {
			c.move_forward = p_script.walk.has(tick) ? sim::kAxisMax : int8_t(0);
			p_motor->step(after, c);
		} else {
			const bool sprinting = p_script.sprint.has(tick);
			after.flags = uint16_t(sim::soldier_flags::kAlive | (sprinting ? sim::soldier_flags::kSprinting : 0));
			after.velocity = { 0.0f, 0.0f, sprinting ? 6.0f : (p_script.walk.has(tick) ? 1.5f : 0.0f) };
			after.mode = p_script.air.has(tick) ? sim::MoveMode::InAir : sim::MoveMode::OnGround;
		}
		sw::WeaponTickOutput out;
		sw::step_weapons_with_rules(p_table, w, c, sw::make_weapon_context(motor, before, after, c), 1, kXhairSpawn, out);
		wires.push_back(net::weapons_wire_view(w, tick));
		soldiers.push_back(after);
		commands.push_back(c);
		shots.push_back(shots.back() + out.shot_count);
		before = after;
	}
	host::HudFeedState state;
	ui::HudModel model;
	std::vector<XhairFrame> frames;
	for (sim::Tick tick = kXhairSpawn + 2; tick <= p_script.last; tick += 2) {
		const size_t i = size_t(tick - kXhairSpawn);
		const sw::SwayContext sway = sw::suppressed_sway(sw::weapon_sway_context(motor, soldiers[i], commands[i]), 0.0f, p_table.mechanics);
		host::HudFeedInput in;
		in.alive = true;
		in.health = 100.0f;
		in.now_s = sw::units_to_seconds(sw::tick_start(tick));
		in.weapon = host::present_weapon(wires[i - 1], wires[i], tick, 0.0, p_table, sway);
		host::fill_hud(in, {}, p_static, state, model);
		frames.push_back({ tick, ui::hud_crosshair_arm(model.weapon.dispersion, p_base_offset), shots[i], sway.move,
				std::hypot(soldiers[i].velocity[0], soldiers[i].velocity[2]) });
	}
	return frames;
}

double speed_fraction_arm(const sw::WeaponSimDef &p_def, float p_speed, float p_walk_speed, double p_base_offset) {
	sw::SwayContext moving;
	moving.move = sw::SwayMove::Moving;
	const float standing = sw::effective_dispersion(p_def, sw::SwayContext{}).min_angle;
	const float walking = sw::effective_dispersion(p_def, moving).min_angle;
	return ui::hud_crosshair_arm(standing + (walking - standing) * std::min(1.0f, p_speed / p_walk_speed), p_base_offset);
}

struct RampMatch {
	size_t frame = 0;
	double max_error = 1e9;
};

RampMatch match_ramp(std::span<const double> p_footage, std::span<const double> p_arms) {
	RampMatch best;
	for (size_t n = 0; n + p_footage.size() <= p_arms.size(); ++n) {
		double e = 0.0;
		for (size_t i = 0; i < p_footage.size(); ++i) {
			e = std::max(e, std::fabs(p_footage[i] - p_arms[n + i]));
		}
		if (e < best.max_error) {
			best = { n, e };
		}
	}
	return best;
}

std::string arms_text(std::span<const double> p_arms) {
	std::string s;
	for (const double a : p_arms) {
		s += (s.empty() ? "" : " ") + fmt("%.0f", a);
	}
	return s;
}

std::vector<XhairFrame> frames_in(const std::vector<XhairFrame> &p_frames, XhairWindow p_window) {
	std::vector<XhairFrame> out;
	for (const XhairFrame &f : p_frames) {
		if (p_window.has(f.tick)) {
			out.push_back(f);
		}
	}
	return out;
}

bool arms_all(const std::vector<XhairFrame> &p_frames, double p_arm) {
	return !p_frames.empty() && std::all_of(p_frames.begin(), p_frames.end(), [p_arm](const XhairFrame &f) { return f.arm == p_arm; });
}

double peak_arm(const std::vector<XhairFrame> &p_frames) {
	double peak = 0.0;
	for (const XhairFrame &f : p_frames) {
		peak = std::max(peak, f.arm);
	}
	return peak;
}

double arm_at(const std::vector<XhairFrame> &p_frames, size_t p_index) {
	return p_index < p_frames.size() ? p_frames[p_index].arm : -1.0;
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
	CHECK(st.rows[def_of(k, "weapons/remington870/u_870")].mode_kinds[0] == host::HudFireMode::SingleBolt);
	CHECK(st.rows[def_of(k, "weapons/m9/u_m9")].crosshair == host::HudCrosshairKind::Pistol);
	CHECK(st.rows[def_of(k, "weapons/rpg7/u_rpg7")].crosshair == host::HudCrosshairKind::Launcher);
	const host::HudStaticRow &m67 = st.rows[def_of(k, "weapons/m67/u_m67")];
	CHECK(m67.crosshair == host::HudCrosshairKind::Grenade);
	CHECK(m67.mode_kinds[0] == host::HudFireMode::HoldAndRelease);
	CHECK(m67.fire_modes == (1u << 4));
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

TEST_CASE("walking opens the crosshair without a shot: the presented spread follows the predicted move state every frame") {
	SyntheticKit k = synthetic_kit();
	const uint16_t m16 = def_of(k, "weapons/m16a4/u_m16a4");
	const uint16_t m9 = def_of(k, "weapons/m9/u_m9");
	sw::SwayState &hip = k.table->defs[m16].sway[0][0];
	hip.move[size_t(sw::SwayMove::Moving)] = hip.move[size_t(sw::SwayMove::Base)];
	hip.move[size_t(sw::SwayMove::Base)].min_angle = 2.5f;
	hip.move[size_t(sw::SwayMove::Moving)].min_angle = 3.0f;
	hip.has_move[size_t(sw::SwayMove::Moving)] = true;
	const host::HudStatic st = synthetic_static(k);
	XhairScript script;
	script.walk = { kXhairSpawn + 80, kXhairSpawn + 140 };
	script.last = kXhairSpawn + 200;
	const std::array<uint16_t, 2> loadout{ m16, m9 };
	const std::vector<XhairFrame> frames = crosshair_frames(*k.table, loadout, st, 0.151, script);
	const std::vector<XhairFrame> before = frames_in(frames, { kXhairSpawn + 40, script.walk.from });
	const std::vector<XhairFrame> walking = frames_in(frames, script.walk);
	const std::vector<XhairFrame> after = frames_in(frames, { script.walk.to, script.last + 1 });
	const bool ok = arms_all(before, 65.0) && arms_all(walking, 75.0) && arms_all(after, 65.0) && frames.back().shots == 0;
	CHECK(arms_all(before, 65.0));
	CHECK(arms_all(walking, 75.0));
	CHECK(arms_all(after, 65.0));
	CHECK(frames.back().shots == 0);
	row("feed.crosshair_walk", ok,
			fmt("no shot fired: arms %.0f standing, %.0f on every scripted 1.5 m/s frame (the sim's Moving minimum above 0.1 m/s), %.0f after stopping (2.5 / 3.0 deg, offset 0.151)", arm_at(before, 0),
					arm_at(walking, 0), arm_at(after, 0)));
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

TEST_CASE("damage direction: own-victim server hits carry the bearing to the presented shooter") {
	const SyntheticKit k = synthetic_kit();
	const host::HudStatic st = synthetic_static(k);
	host::HudFeedState state;
	ui::HudModel model;
	CHECK(host::hud_bearing_deg({ 0, 0, 0 }, { 0, 0, 5 }) == doctest::Approx(0.0f));
	CHECK(host::hud_bearing_deg({ 0, 0, 0 }, { 5, 0, 0 }) == doctest::Approx(270.0f));
	CHECK(host::hud_bearing_deg({ 0, 0, 0 }, { 0, 0, -5 }) == doctest::Approx(180.0f));
	CHECK(host::hud_bearing_deg({ 0, 0, 0 }, { -5, 0, 0 }) == doctest::Approx(90.0f));
	CHECK(host::hud_bearing_deg({ 1, 0, 1 }, { 1, 3, 1 }) == 0.0f);
	std::array<bf3::present::PresentedSoldier, 2> others{};
	others[0].slot = 4;
	others[0].position = { 10.0f, 0.0f, 20.0f };
	others[1].slot = 6;
	others[1].position = { 0.0f, 0.0f, 10.0f };
	host::HudFeedInput in;
	in.alive = true;
	in.health = 70;
	in.damage_own_slot = 2;
	in.own_position = { 10.0f, 0.0f, 10.0f };
	in.others = others;
	std::vector<host::PresentEvent> ev;
	host::PresentEvent a = hit_event(host::present_flags::kOwnVictim, 1);
	a.slot = 4;
	a.value = 30.0f;
	ev.push_back(a);
	host::PresentEvent b = hit_event(host::present_flags::kOwnVictim, 2);
	b.slot = 6;
	b.value = 12.0f;
	ev.push_back(b);
	host::PresentEvent self = hit_event(host::present_flags::kOwnVictim | host::present_flags::kOwnShooter, 3);
	self.slot = 2;
	ev.push_back(self);
	host::PresentEvent unseen = hit_event(host::present_flags::kOwnVictim, 4);
	unseen.slot = 9;
	ev.push_back(unseen);
	host::PresentEvent other = hit_event(0, 5);
	other.slot = 4;
	ev.push_back(other);
	host::fill_hud(in, ev, st, state, model);
	REQUIRE(model.damage.newest_serial == 2);
	const ui::HudDamageHit &h1 = model.damage.hits[1 % ui::kHudDamageFeedSize];
	const ui::HudDamageHit &h2 = model.damage.hits[2 % ui::kHudDamageFeedSize];
	CHECK(h1.serial == 1);
	CHECK(h1.shooter == 4);
	CHECK(h1.bearing_deg == doctest::Approx(0.0f));
	CHECK(h1.damage == 30.0f);
	CHECK(h2.shooter == 6);
	CHECK(h2.bearing_deg == doctest::Approx(90.0f));
	CHECK(h2.damage == 12.0f);
	CHECK(state.damage_shown == 2);
	CHECK(state.damage_unplaced == 1);
	host::fill_hud(in, {}, st, state, model);
	CHECK(model.damage.newest_serial == 2);
	row("feed.damage_direction", state.damage_shown == 2 && state.damage_unplaced == 1,
			"2 own-victim hits fed with the bearing to the presented shooter (0 and 90 deg), self damage skipped, 1 unpresented shooter counted");
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

TEST_CASE("compass heading from the view yaw and the grenade count from the wcHgr slot") {
	constexpr float kPi = 3.14159265358979323846f;
	CHECK(host::hud_compass_heading_deg(0.0f) == 0.0f);
	CHECK(host::hud_compass_heading_deg(kPi / 2.0f) == doctest::Approx(270.0f));
	CHECK(host::hud_compass_heading_deg(-kPi / 2.0f) == doctest::Approx(90.0f));
	CHECK(host::hud_compass_heading_deg(-295.0f * kPi / 180.0f + 4.0f * kPi) == doctest::Approx(295.0f).epsilon(1e-4));
	CHECK(host::hud_compass_heading_deg(std::nanf("")) == 0.0f);
	const SyntheticKit k = synthetic_kit();
	const host::HudStatic st = synthetic_static(k);
	net::WeaponsWire wire;
	wire.slots[0].def = def_of(k, "weapons/m16a4/u_m16a4");
	wire.slots[0].magazine = 30;
	wire.slots[0].reserve = 120;
	CHECK(host::hud_grenade_count(wire, st) == 0);
	wire.slots[3].def = def_of(k, "weapons/m67/u_m67");
	wire.slots[3].magazine = 1;
	wire.slots[3].reserve = 1;
	CHECK(host::hud_grenade_count(wire, st) == 2);
	wire.slots[3].reserve = -4;
	CHECK(host::hud_grenade_count(wire, st) == 1);
	host::HudFeedState state;
	ui::HudModel model;
	host::HudFeedInput in;
	in.alive = true;
	in.health = 100.0f;
	in.view_yaw_rad = -212.0f * kPi / 180.0f;
	in.weapon.valid = true;
	in.weapon.def = wire.slots[0].def;
	in.weapons = &wire;
	host::fill_hud(in, {}, st, state, model);
	CHECK(model.weapon.secondary_weapon_ammo == 1);
	CHECK(model.soldier.compass_angle == doctest::Approx(212.0f).epsilon(1e-4));
	in.alive = false;
	host::fill_hud(in, {}, st, state, model);
	CHECK(model.weapon.secondary_weapon_ammo == 0);
	row("feed.compass_grenades", true, "yaw 212 deg -> heading 212; wcHgr slot 1 + 1 -> 2 grenades, cleared when dead");
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

TEST_CASE("crosshair footage: the M16A3 builds of the 18.57 recording through the sim, the HUD feed and crosshairdefault's binding") {
	HudInstall *hi = hud_install();
	if (hi == nullptr) {
		return;
	}
	bf3::core::Result<sw::InstallWeapons> weapons = sw::open_install_weapons(hi->install);
	REQUIRE_MESSAGE(weapons.ok(), (weapons.ok() ? std::string() : weapons.error().message));
	bf3::core::Result<std::unique_ptr<bf3::core::EbxLibrary>> level = bf3::core::EbxLibrary::open(hi->install, { "Win32/Levels/MP_Subway/MP_Subway" });
	REQUIRE_MESSAGE(level.ok(), (level.ok() ? std::string() : level.error().message));
	bf3::core::Result<ui::HudScreenAsset> screen = ui::read_hud_screen_asset(**level, ui::kCrosshairDefaultPartition);
	REQUIRE_MESSAGE(screen.ok(), (screen.ok() ? std::string() : screen.error().message));
	const ui::HudWidgetNode *node = screen->find("Crosshair_01");
	REQUIRE(node != nullptr);
	REQUIRE(node->dispersion_base_offset.has_value());
	const double offset = *node->dispersion_base_offset;

	const std::vector<std::string> suppressed_unlocks{ "weapons/m16a4/u_m16a4_sound_suppressor", "weapons/m16a4/u_m16a4_eotech", "weapons/m16a4/u_m16a4_bipod" };
	const std::vector<std::string> heavy_unlocks{ "weapons/m16a4/u_m16a4_heavybarrel" };
	std::vector<gp::WeaponDef> defs;
	for (const std::vector<std::string> *u : { &suppressed_unlocks, &heavy_unlocks }) {
		bf3::core::Result<gp::WeaponDef> d = weapons->catalog->resolve("weapons/m16a4/u_m16a4", *u);
		REQUIRE_MESSAGE(d.ok(), (d.ok() ? std::string() : d.error().message));
		defs.push_back(std::move(*d));
	}
	bf3::core::Result<gp::WeaponDef> pistol = weapons->catalog->resolve("weapons/m9/u_m9");
	REQUIRE_MESSAGE(pistol.ok(), (pistol.ok() ? std::string() : pistol.error().message));
	defs.push_back(std::move(*pistol));
	bf3::core::Result<std::shared_ptr<const sw::WeaponTable>> table = sw::make_weapon_table(defs, weapons->materials, sw::install_table_options(*weapons));
	REQUIRE_MESSAGE(table.ok(), (table.ok() ? std::string() : table.error().message));
	bf3::core::Result<host::HudStatic> st = host::make_hud_static(**table, defs);
	REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));

	bf3::core::Result<gp::SoldierDef> soldier = gp::read_soldier_def(**level);
	REQUIRE_MESSAGE(soldier.ok(), (soldier.ok() ? std::string() : soldier.error().message));
	bf3::core::Result<bf3::sim::MotorParams> motor_params = bf3::sim::make_motor_params(*soldier, bf3::test::gunplay::shipped_motor_tuning());
	REQUIRE_MESSAGE(motor_params.ok(), (motor_params.ok() ? std::string() : motor_params.error().message));
	const std::unique_ptr<bf3::sim::CollisionWorld> flat = tw::wall_world({}, 150.0f);
	bf3::sim::SoldierMotor motor;
	const bf3::core::Status motor_ok = motor.init(*flat, *motor_params);
	REQUIRE_MESSAGE(motor_ok.ok(), (motor_ok.ok() ? std::string() : motor_ok.error().message));
	const bf3::sim::StateSpeed &stand_speed = motor_params->poses[size_t(bf3::sim::Pose::Stand)].on_ground;
	const float walk_speed = stand_speed.velocity * stand_speed.forward;

	constexpr double kFootageSuppressedStand = 85.0;
	constexpr double kFootageSuppressedMoving = 99.0;
	constexpr double kFootageSuppressedSprint = 115.0;
	constexpr double kFootageSuppressedJump = 155.0;
	constexpr double kFootageHeavyStand = 75.0;
	constexpr double kFootageHeavyMoving = 87.0;
	constexpr double kFootageHeavyBurstPeak = 117.0;
	constexpr double kFootageDecayPerFrame = 10.0;
	constexpr std::array<double, 7> kFootageHeavyOnset{ 82.0, 84.0, 85.0, 86.0, 86.0, 86.0, 87.0 };
	constexpr std::array<double, 8> kFootageSuppressedStop{ 98.0, 97.0, 95.0, 93.0, 91.0, 90.0, 88.0, 85.0 };
	constexpr double kFootageArmRounding = 1.0;

	const bf3::sim::Tick t0 = kXhairSpawn + 60;
	XhairScript walk;
	walk.walk = { t0 + 60, t0 + 150 };
	walk.last = t0 + 240;
	struct Walked {
		std::vector<XhairFrame> stand;
		std::vector<XhairFrame> settled;
		std::vector<double> onset_sim, onset_blend, stop_sim, stop_blend;
	};
	const auto walked = [&](const std::array<uint16_t, 2> &p_loadout) {
		const std::vector<XhairFrame> f = crosshair_frames(**table, p_loadout, *st, offset, walk, &motor);
		const sw::WeaponSimDef &def = (*table)->defs[p_loadout[0]];
		Walked w;
		w.stand = frames_in(f, { t0, walk.walk.from });
		w.settled = frames_in(f, { walk.walk.from + 30, walk.walk.to });
		for (const XhairFrame &x : frames_in(f, { walk.walk.from - 4, walk.walk.from + 30 })) {
			w.onset_sim.push_back(x.arm);
			w.onset_blend.push_back(speed_fraction_arm(def, x.speed, walk_speed, offset));
		}
		for (const XhairFrame &x : frames_in(f, { walk.walk.to - 4, walk.walk.to + 30 })) {
			w.stop_sim.push_back(x.arm);
			w.stop_blend.push_back(speed_fraction_arm(def, x.speed, walk_speed, offset));
		}
		return w;
	};
	const std::array<uint16_t, 2> suppressed{ 0, 2 };
	const std::array<uint16_t, 2> heavy{ 1, 2 };
	const Walked ws = walked(suppressed);
	const Walked wh = walked(heavy);
	CHECK(arms_all(ws.stand, kFootageSuppressedStand));
	CHECK(arms_all(ws.settled, kFootageSuppressedMoving));
	CHECK(arms_all(wh.stand, kFootageHeavyStand));
	CHECK(arms_all(wh.settled, kFootageHeavyMoving));
	row("footage.suppressed.stand_move", arms_all(ws.stand, kFootageSuppressedStand) && arms_all(ws.settled, kFootageSuppressedMoving),
			fmt("M16A3 + suppressor: arms %.0f standing and %.0f from 0.5 s into a walk (footage %.0f / %.0f at FOV 100, 1280x720)", arm_at(ws.stand, 0), arm_at(ws.settled, 0),
					kFootageSuppressedStand, kFootageSuppressedMoving));
	row("footage.heavy.stand_move", arms_all(wh.stand, kFootageHeavyStand) && arms_all(wh.settled, kFootageHeavyMoving),
			fmt("M16A3 + heavy barrel: arms %.0f standing and %.0f from 0.5 s into a walk (footage %.0f / %.0f)", arm_at(wh.stand, 0), arm_at(wh.settled, 0), kFootageHeavyStand,
					kFootageHeavyMoving));

	const RampMatch onset = match_ramp(kFootageHeavyOnset, wh.onset_sim);
	const RampMatch stop = match_ramp(kFootageSuppressedStop, ws.stop_sim);
	REQUIRE(onset.frame + kFootageHeavyOnset.size() <= wh.onset_sim.size());
	REQUIRE(stop.frame + kFootageSuppressedStop.size() <= ws.stop_sim.size());
	const bool onset_is_blend = wh.onset_sim == wh.onset_blend;
	const bool stop_is_blend = ws.stop_sim == ws.stop_blend;
	CHECK(onset.max_error <= kFootageArmRounding);
	CHECK(stop.max_error <= kFootageArmRounding);
	CHECK(onset_is_blend);
	CHECK(stop_is_blend);
	row("footage.heavy.move_onset", onset.max_error <= kFootageArmRounding && onset_is_blend,
			fmt("footage arms per 30 fps frame from 104.87 s: %s; the sim's presented arms as the motor accelerates to its walk speed (%.1f m/s): %s, max %.0f px, %s the speed / walk "
				"speed blend of the moving minimum",
					arms_text(kFootageHeavyOnset).c_str(), double(walk_speed),
					arms_text(std::span<const double>(wh.onset_sim).subspan(onset.frame, kFootageHeavyOnset.size())).c_str(), onset.max_error,
					onset_is_blend ? "equal to" : "not equal to"));
	row("footage.suppressed.move_stop", stop.max_error <= kFootageArmRounding && stop_is_blend,
			fmt("footage arms per 30 fps frame from 301.88 s: %s; the sim's presented arms as the motor decelerates: %s, max %.0f px, %s the blend",
					arms_text(kFootageSuppressedStop).c_str(), arms_text(std::span<const double>(ws.stop_sim).subspan(stop.frame, kFootageSuppressedStop.size())).c_str(),
					stop.max_error, stop_is_blend ? "equal to" : "not equal to"));

	XhairScript s;
	s.sprint = { t0 + 150, t0 + 210 };
	s.air = { t0 + 240, t0 + 270 };
	s.fire = { t0 + 268, t0 + 270 };
	s.last = t0 + 360;
	const std::vector<XhairFrame> fs = crosshair_frames(**table, suppressed, *st, offset, s);
	const std::vector<XhairFrame> sprint = frames_in(fs, s.sprint);
	const std::vector<XhairFrame> air = frames_in(fs, { s.air.from, s.fire.from });
	const std::vector<XhairFrame> landed = frames_in(fs, { s.air.to, s.last + 1 });
	const std::vector<XhairFrame> before_shot = frames_in(fs, { t0, s.fire.from });
	REQUIRE(!before_shot.empty());
	REQUIRE(!landed.empty());
	CHECK(before_shot.back().shots == 0);
	CHECK(landed.back().shots == 1);
	size_t full_steps = 0;
	bool steps_ok = true;
	for (size_t i = 1; i < landed.size(); ++i) {
		const double step = landed[i - 1].arm - landed[i].arm;
		steps_ok = steps_ok && step >= 0.0 && step <= kFootageDecayPerFrame;
		full_steps += step == kFootageDecayPerFrame ? 1 : 0;
	}
	CHECK(steps_ok);
	CHECK(full_steps >= 5);
	CHECK(landed.back().arm == kFootageSuppressedStand);
	row("footage.suppressed.decay", steps_ok && full_steps >= 5 && landed.back().arm == kFootageSuppressedStand,
			fmt("after a shot in the air and the landing the arms fall %zu frames by %.0f px per 30 fps frame (footage %.0f px per frame, 15 deg/s) to %.0f", full_steps,
					kFootageDecayPerFrame, kFootageDecayPerFrame, landed.back().arm));
	std::printf("INFO footage.suppressed.sprint sim arm %.0f, footage %.0f (the sim applies the suppressor's x1.4 to the Sprinting minimum)\n", peak_arm(sprint),
			kFootageSuppressedSprint);
	std::printf("INFO footage.suppressed.jump sim arm %.0f, footage %.0f (the sim's Jumping minimum is above the state's maximum, the footage clamps at 7 deg)\n", peak_arm(air),
			kFootageSuppressedJump);

	XhairScript h;
	h.fire = { t0 + 150, t0 + 246 };
	h.last = t0 + 300;
	const std::vector<XhairFrame> fh = crosshair_frames(**table, heavy, *st, offset, h);
	const std::vector<XhairFrame> burst = frames_in(fh, h.fire);
	REQUIRE(!burst.empty());
	std::printf("INFO footage.heavy.burst sim peak arm %.0f over %u shots in 1.6 s, footage %.0f (the footage holds the bloom while firing, the sim decays 15 deg/s between shots)\n",
			peak_arm(burst), burst.back().shots - burst.front().shots, kFootageHeavyBurstPeak);
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
