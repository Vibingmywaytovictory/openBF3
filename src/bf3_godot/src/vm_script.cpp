#include "vm_script.h"

#include "bf3/sim/command.h"
#include "bf3/sim/weapons/sim_time.h"
#include "bf3/sim/weapons/weapon_input.h"
#include "bf3/sim/weapons/weapon_rules.h"

#include <algorithm>
#include <cmath>

namespace bf3::godot_side {

namespace sw = bf3::sim::weapons;

namespace vm_script {

namespace {

constexpr uint64_t kMatchSeed = 0x766d2d64656d6full;
constexpr sim::Tick kSpawnTick = 600;
constexpr double kTick = 1.0 / double(sim::kSimHz);
constexpr uint32_t kGuardTicks = 60 * 12;

int64_t display_units(double p_seconds) {
	return int64_t(std::llround(p_seconds * double(sw::kTimeUnitsPerSecond)));
}

struct Driver {
	const sw::WeaponTable &table;
	KitRun &run;
	sw::SoldierWeaponsState st;
	sim::Tick tick = 0;
	sw::WeaponTickOutput out;
	bool tap_phase = false;
	uint32_t tap_counter = 0;

	Driver(const sw::WeaponTable &p_table, KitRun &r_run) :
			table(p_table), run(r_run) {}

	double now() const { return sw::units_to_seconds(sw::tick_end(tick)); }
	const sw::WeaponSlotState &cur() const { return st.current(); }

	void step(sw::WeaponInput p_in, bool p_sprint = false, float p_speed = 0, float p_yaw_rate = 0, float p_pitch_rate = 0) {
		++tick;
		sim::Command c;
		c.tick = tick;
		c.sequence = tick;
		c.buttons = sw::with_weapon_input(0, p_in);
		sw::WeaponContext ctx;
		ctx.eye_start = { 0, 1.6f, 0 };
		ctx.eye_end = ctx.eye_start;
		ctx.sprinting = p_sprint;
		ctx.sway.move = p_sprint ? sw::SwayMove::Sprinting : (p_speed > 0.5f ? sw::SwayMove::Moving : sw::SwayMove::Base);
		ctx.sway.zoom = p_in.zoom ? sw::Zoom::Zoom : sw::Zoom::NoZoom;
		sw::step_weapons_with_rules(table, st, c, ctx, kMatchSeed, run.spawn, out);
		run.states.push_back(st);
		run.sprinting.push_back(p_sprint ? 1 : 0);
		run.zoom_held.push_back(p_in.zoom ? 1 : 0);
		run.speed.push_back(p_speed);
		run.yaw_rate.push_back(p_yaw_rate);
		run.pitch_rate.push_back(p_pitch_rate);
		run.yaw.push_back(run.yaw.back() + double(p_yaw_rate) * kTick);
		run.pitch.push_back(std::clamp(run.pitch.back() + double(p_pitch_rate) * kTick, -0.6, 0.6));
		for (uint8_t k = 0; k < out.timed_count; ++k) {
			run.timed.push_back(out.timed[k]);
		}
		for (uint8_t k = 0; k < out.shot_count; ++k) {
			run.shots.push_back(out.shots[k]);
		}
	}

	bool tap() {
		const bool down = (tap_counter % 6) < 2;
		++tap_counter;
		return down;
	}
	void idle(double p_seconds, bool p_zoom = false) {
		for (uint32_t i = 0; i < uint32_t(std::lround(p_seconds * sim::kSimHz)); ++i) {
			sw::WeaponInput in;
			in.zoom = p_zoom;
			step(in);
		}
	}
	void mark(const char *p_label) { run.marks.push_back({ now(), st.active, p_label }); }

	void wait_deployed(uint8_t p_slot, bool p_zoom) {
		for (uint32_t i = 0; i < kGuardTicks; ++i) {
			if (st.active == p_slot && st.pending == 0xFF && cur().phase != sw::WeaponPhase::Deploying && cur().phase != sw::WeaponPhase::None) {
				return;
			}
			sw::WeaponInput in;
			in.zoom = p_zoom;
			step(in);
		}
	}
	void select(uint8_t p_slot) {
		sw::WeaponInput in;
		in.select_slot = p_slot;
		step(in);
	}

	void reload_then_fire(const char *p_label) {
		if (cur().phase != sw::WeaponPhase::Reloading && cur().phase != sw::WeaponPhase::ReloadShell) {
			sw::WeaponInput press;
			press.reload = true;
			step(press);
		}
		bool started = false;
		for (uint32_t i = 0; i < 6; ++i) {
			if (cur().phase == sw::WeaponPhase::Reloading || cur().phase == sw::WeaponPhase::ReloadShell) {
				started = true;
				break;
			}
			step(sw::WeaponInput{});
		}
		if (!started) {
			return;
		}
		run.marks.push_back({ sw::units_to_seconds(cur().phase_start), st.active, p_label });
		bool pressed = false;
		for (uint32_t i = 0; i < kGuardTicks; ++i) {
			const bool reloading = cur().phase == sw::WeaponPhase::Reloading || cur().phase == sw::WeaponPhase::ReloadShell;

			const double end = sw::units_to_seconds(cur().phase_end);
			const bool in_end_tick = reloading && cur().phase_end > 0 && sw::units_to_seconds(sw::tick_end(tick + 1)) > end;
			sw::WeaponInput in;
			in.fire = in_end_tick || (!reloading && pressed);
			pressed = pressed || in_end_tick;
			step(in);
			if (!reloading && pressed) {
				break;
			}
		}

		for (uint32_t i = 0; i < uint32_t(0.2 * sim::kSimHz); ++i) {
			sw::WeaponInput in;
			in.fire = true;
			step(in);
		}
	}
};

}

double KitRun::start_s() const {
	return sw::units_to_seconds(sw::tick_end(spawn));
}

double KitRun::end_s() const {
	return sw::units_to_seconds(sw::tick_end(spawn + sim::Tick(states.empty() ? 0 : states.size() - 1)));
}

const Mark *KitRun::mark(uint8_t p_slot, const std::string &p_label) const {
	for (const Mark &m : marks) {
		if (m.slot == p_slot && m.label == p_label) {
			return &m;
		}
	}
	return nullptr;
}

KitRun run_kit(const sw::WeaponTable &p_table, size_t p_kit, const std::array<uint16_t, kKitItems> &p_defs) {
	KitRun run;
	run.kit = p_kit;
	run.spawn = kSpawnTick;
	run.defs = p_defs;
	Driver d(p_table, run);
	const core::Status spawned = sw::spawn_soldier_weapons(d.st, p_table, std::span<const uint16_t>(p_defs.data(), p_defs.size()), 0, run.spawn);
	if (!spawned.ok()) {
		run.error = spawned.error().message;
		return run;
	}
	d.tick = run.spawn;
	run.states.push_back(d.st);
	run.sprinting.push_back(0);
	run.zoom_held.push_back(0);
	run.speed.push_back(0);
	run.yaw_rate.push_back(0);
	run.pitch_rate.push_back(0);
	run.yaw.push_back(0.0);
	run.pitch.push_back(0.0);

	d.wait_deployed(0, true);
	d.idle(0.3);
	d.mark("hip");

	for (uint32_t i = 0; i < uint32_t(0.6 * sim::kSimHz); ++i) {
		d.step(sw::WeaponInput{}, false, 4.0f, 1.2f * float(std::sin(double(i) * 0.15)), 0.4f);
	}
	d.mark("settle");
	d.idle(1.3);

	d.mark("zoom_press");
	for (uint32_t i = 0; i < uint32_t(1.0 * sim::kSimHz); ++i) {
		sw::WeaponInput in;
		in.zoom = true;
		in.fire = i >= 33 && i < 48 && d.tap();
		if (i == 30) {
			d.mark("ads");
		}
		d.step(in);
	}
	d.mark("zoom_release");
	d.idle(0.6);

	for (uint32_t i = 0; i < uint32_t(0.3 * sim::kSimHz); ++i) {
		sw::WeaponInput in;
		in.fire = d.tap();
		d.step(in);
	}
	d.idle(0.3);

	d.reload_then_fire("reload_start");
	d.idle(0.4);

	d.mark("sprint");
	for (uint32_t i = 0; i < uint32_t(0.8 * sim::kSimHz); ++i) {
		sw::WeaponInput in;
		in.zoom = true;
		d.step(in, true, 6.5f);
	}
	d.idle(0.4);

	for (uint32_t i = 0; i < kGuardTicks && d.cur().magazine > 0; ++i) {
		sw::WeaponInput in;
		in.fire = d.tap();
		d.step(in);
	}
	d.idle(0.2);
	d.reload_then_fire("reload_empty_start");
	d.idle(0.4);

	d.select(1);
	d.wait_deployed(1, true);
	d.idle(0.3);
	d.mark("hip");
	d.idle(0.1);
	d.mark("zoom_press");
	for (uint32_t i = 0; i < uint32_t(0.8 * sim::kSimHz); ++i) {
		sw::WeaponInput in;
		in.zoom = true;
		in.fire = i >= 27 && i < 45 && d.tap();
		if (i == 24) {
			d.mark("ads");
		}
		d.step(in);
	}
	d.mark("zoom_release");
	d.idle(0.4);
	d.reload_then_fire("reload_start");
	d.idle(0.4);

	d.select(2);
	d.wait_deployed(2, false);
	d.idle(0.3);
	d.mark("hip");
	for (uint32_t i = 0; i < uint32_t(0.4 * sim::kSimHz); ++i) {
		sw::WeaponInput in;
		in.fire = true;
		d.step(in);
	}
	d.idle(0.15);
	d.mark("throw");
	d.idle(1.6);

	d.select(3);
	d.wait_deployed(3, false);
	d.idle(0.3);
	d.mark("hip");
	for (uint32_t i = 0; i < 2; ++i) {
		sw::WeaponInput in;
		in.fire = true;
		d.step(in);
	}
	d.idle(0.2);
	d.mark("knife");
	d.idle(1.4);
	return run;
}

View view_at(const KitRun &p_run, const sw::WeaponTable &p_table, double p_t) {
	View v;
	if (p_run.states.empty()) {
		return v;
	}
	const int64_t u0 = sw::tick_end(p_run.spawn);
	double rel = double(display_units(p_t) - u0) / double(sw::kTimeUnitsPerTick);
	rel = std::clamp(rel, 0.0, double(p_run.states.size() - 1) - 1e-9);
	const size_t i = size_t(std::floor(rel));
	const size_t j = std::min(i + 1, p_run.states.size() - 1);
	const double f = rel - double(i);
	v.weapon = scripted::present_weapon(p_run.states[i], p_run.states[j], f, p_t, p_table);
	v.sprinting = p_run.sprinting[j] != 0;
	v.zoom_held = p_run.zoom_held[j] != 0;
	v.speed = p_run.speed[j];
	v.yaw_rate = p_run.yaw_rate[j];
	v.pitch_rate = p_run.pitch_rate[j];
	v.yaw = p_run.yaw[i] + (p_run.yaw[j] - p_run.yaw[i]) * f;
	v.pitch = p_run.pitch[i] + (p_run.pitch[j] - p_run.pitch[i]) * f;
	return v;
}

void events_between(const KitRun &p_run, double p_from, double p_to, std::vector<bf3::host::PresentEvent> &r_events) {
	r_events.clear();
	const int64_t from = display_units(p_from), to = display_units(p_to);
	for (const sw::TimedWeaponEvent &e : p_run.timed) {
		if (e.time > from && e.time <= to) {
			bf3::host::PresentEvent pe;
			pe.kind = bf3::host::PresentKind::OwnTimed;
			pe.timeline = bf3::host::PresentTimeline::Predicted;
			pe.aux = e.weapon_slot;
			pe.id = e.event;
			pe.time = e.time;
			r_events.push_back(pe);
		}
	}
	for (const sw::ShotEvent &s : p_run.shots) {
		if (s.time > from && s.time <= to) {
			bf3::host::PresentEvent pe;
			pe.kind = bf3::host::PresentKind::OwnShot;
			pe.timeline = bf3::host::PresentTimeline::Predicted;
			pe.aux = s.weapon_slot;
			pe.def = s.def;
			pe.id = s.shot_index;
			pe.time = s.time;
			r_events.push_back(pe);
		}
	}
	std::stable_sort(r_events.begin(), r_events.end(), [](const bf3::host::PresentEvent &p_a, const bf3::host::PresentEvent &p_b) { return p_a.time < p_b.time; });
}

}

namespace scripted {

bf3::host::PresentedWeapon present_weapon(const sw::SoldierWeaponsState &p_prev, const sw::SoldierWeaponsState &p_curr, double p_fraction, double p_time_s,
		const sw::WeaponTable &p_table) {
	bf3::host::PresentedWeapon p;
	const sw::WeaponSlotState &a = p_prev.current();
	const sw::WeaponSlotState &b = p_curr.current();
	const float f = float(std::clamp(p_fraction, 0.0, 1.0));
	p.valid = b.def != sw::kNoWeapon;
	p.def = b.def;
	p.active_slot = p_curr.active;
	p.pending_slot = p_curr.pending;
	p.phase = b.phase;
	p.mode = b.mode;
	p.flags = b.flags;
	p.magazine = b.magazine;
	p.reserve = b.reserve;
	p.magazine_capacity = b.def < p_table.defs.size() ? p_table.defs[b.def].magazine_capacity : 0;
	p.time_s = p_time_s;
	p.phase_start_s = sw::units_to_seconds(b.phase_start);
	p.phase_end_s = sw::units_to_seconds(b.phase_end);
	const double len = p.phase_end_s - p.phase_start_s;
	p.phase_progress = b.phase_end > 0 && len > 0 ? float(std::clamp((p_time_s - p.phase_start_s) / len, 0.0, 1.0)) : 0.0f;

	const bool same = p_prev.active == p_curr.active && a.def == b.def;
	p.spread_deg = same ? a.spread + (b.spread - a.spread) * f : b.spread;
	p.recoil.pitch = same ? a.recoil.pitch + (b.recoil.pitch - a.recoil.pitch) * f : b.recoil.pitch;
	p.recoil.yaw = same ? a.recoil.yaw + (b.recoil.yaw - a.recoil.yaw) * f : b.recoil.yaw;
	p.zoomed = (b.flags & sw::slot_flags::kZoomWasDown) != 0;
	p.events = p_curr.events;
	return p;
}

}

}
