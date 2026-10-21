#include <doctest/doctest.h>

#include "gunplay_fixture.h"

#include "bf3/sim/weapons/spread_recoil.h"
#include "bf3/sim_harness/shooting_range.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdarg>
#include <cstring>
#include <map>
#include <random>
#include <set>

using namespace bf3;
using namespace bf3::test::gunplay;
namespace sw = bf3::sim::weapons;

namespace {

constexpr sw::SimTime kUnit = sw::kTimeUnitsPerTick;

std::string lower(std::string p_s) {
	for (char &c : p_s) {
		c = char(std::tolower(static_cast<unsigned char>(c)));
	}
	return p_s;
}

std::string short_name(const std::string &p_variant) {
	const size_t at = p_variant.rfind('/');
	return at == std::string::npos ? p_variant : p_variant.substr(at + 1);
}

bool ends_with(const std::string &p_s, const std::string &p_tail) {
	return p_s.size() >= p_tail.size() && p_s.compare(p_s.size() - p_tail.size(), p_tail.size(), p_tail) == 0;
}

bool alive_flag(const sim::SoldierState &p_state) {
	return (p_state.flags & sim::soldier_flags::kAlive) != 0 && p_state.mode != sim::MoveMode::Dead;
}

double hdist(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	return std::hypot(double(p_a[0]) - double(p_b[0]), double(p_a[2]) - double(p_b[2]));
}

std::string mechanics_cfg_path() {
	return OPENBF3_MECHANICS_CFG;
}

std::shared_ptr<const sw::WeaponTable> local_table(const host::SharedLevel &p_level, const std::string &p_cfg, bool p_exact_aim = false,
		bool p_infinite = false) {
	core::Result<sw::Mechanics> m = host::load_mechanics_cfg(p_cfg);
	REQUIRE_MESSAGE(m.ok(), (m.ok() ? std::string() : m.error().message));
	if (!m.ok()) {
		return nullptr;
	}
	core::Result<std::shared_ptr<const sw::WeaponTable>> t = host::make_match_table(*p_level.gunplay, *m, p_exact_aim, p_infinite);
	REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
	return t.ok() ? *t : nullptr;
}

struct Run {
	std::unique_ptr<Session> s;
	std::shared_ptr<GunplayRecorder> rec;
	uint8_t slot = 0;
};

Run start_run(std::shared_ptr<const host::SharedLevel> p_level, host::GunplayConfig p_gunplay, SessionOptions p_options = {}) {
	Run r;
	r.rec = std::make_shared<GunplayRecorder>();
	p_gunplay.observer = r.rec;
	if (p_gunplay.mechanics_cfg == std::string(sw::kMechanicsTuningPath)) {
		p_gunplay.mechanics_cfg = mechanics_cfg_path();
	}
	p_options.configure_server = [p_gunplay](host::GameServerConfig &r_game) { r_game.gunplay = p_gunplay; };
	if (p_options.client_mechanics_cfg.empty()) {
		p_options.client_mechanics_cfg = p_gunplay.mechanics_cfg;
	}
	r.s = start_session(p_level, p_options);
	if (r.s == nullptr) {
		return r;
	}
	const bool spawned = wait_spawned(*r.s, 15.0);
	REQUIRE_MESSAGE(spawned, "no spawn within 15 s");
	if (!spawned) {
		r.s.reset();
		return r;
	}
	r.slot = r.s->client.stats().slot;
	r.rec->set_watch(r.slot);
	const bool ready = poll_until(*r.s, [&] { return weapon_ready(*r.rec, r.slot); }, 8.0);
	CHECK_MESSAGE(ready, "the active weapon did not deploy within 8 s");
	return r;
}

void set_buttons(Run &r_run, uint32_t p_buttons) {
	r_run.s->input.held_buttons = p_buttons;
	poll(*r_run.s);
}

size_t hold_fire(Run &r_run, size_t p_count, double p_timeout_s, uint32_t p_extra = 0, const std::function<bool()> &p_stop = {}) {
	const size_t before = r_run.rec->shot_count(r_run.slot);
	set_buttons(r_run, p_extra | sw::weapon_buttons::kFire);
	poll_until(
			*r_run.s, [&] { return r_run.rec->shot_count(r_run.slot) >= before + p_count || (p_stop && p_stop()); }, p_timeout_s);
	set_buttons(r_run, p_extra);
	return r_run.rec->shot_count(r_run.slot) - before;
}

size_t tap_fixed(Run &r_run, int p_hold_ms, int p_after_ms, uint32_t p_extra = 0) {
	const size_t before = r_run.rec->shot_count(r_run.slot);
	set_buttons(r_run, p_extra | sw::weapon_buttons::kFire);
	run_for(*r_run.s, double(p_hold_ms) / 1000.0, {}, 2);
	set_buttons(r_run, p_extra);
	run_for(*r_run.s, double(p_after_ms) / 1000.0, {}, 2);
	return r_run.rec->shot_count(r_run.slot) - before;
}

std::optional<sim::Tick> press_tick(const std::vector<WeaponTickRecord> &p_ticks, uint8_t p_slot) {
	for (const WeaponTickRecord &r : p_ticks) {
		if (r.slot == p_slot && (r.command.buttons & sw::weapon_buttons::kFire) != 0) {
			return r.command.tick;
		}
	}
	return std::nullopt;
}

std::vector<sw::TimedWeaponEvent> timed_events(const std::vector<WeaponTickRecord> &p_ticks, uint8_t p_slot, uint32_t p_mask) {
	std::vector<sw::TimedWeaponEvent> out;
	for (const WeaponTickRecord &r : p_ticks) {
		if (r.slot != p_slot) {
			continue;
		}
		for (const sw::TimedWeaponEvent &e : r.timed) {
			if ((e.event & p_mask) != 0) {
				out.push_back(e);
			}
		}
	}
	return out;
}

bool shots_equal(const sw::ShotEvent &p_a, const sw::ShotEvent &p_b) {
	return p_a.time == p_b.time && p_a.spawn_time == p_b.spawn_time && p_a.seed == p_b.seed && p_a.player_slot == p_b.player_slot &&
			p_a.shot_index == p_b.shot_index && p_a.def == p_b.def && p_a.weapon_slot == p_b.weapon_slot && p_a.pellets == p_b.pellets &&
			std::memcmp(&p_a.origin, &p_b.origin, sizeof(p_a.origin)) == 0 && std::memcmp(&p_a.aim, &p_b.aim, sizeof(p_a.aim)) == 0 &&
			std::memcmp(&p_a.direction, &p_b.direction, sizeof(p_a.direction)) == 0 && std::memcmp(&p_a.spread, &p_b.spread, sizeof(float)) == 0 &&
			std::memcmp(&p_a.pellet_cone, &p_b.pellet_cone, sizeof(float)) == 0 && p_a.recoil == p_b.recoil && p_a.tracer == p_b.tracer &&
			std::memcmp(&p_a.power, &p_b.power, sizeof(float)) == 0;
}

void row(const char *p_gate, const std::string &p_name, bool p_pass, const std::string &p_measured, const std::string &p_threshold) {
	std::printf("%-4s %-58s %s | %s | %s\n", p_gate, p_name.c_str(), p_pass ? "PASS" : "FAIL", p_measured.c_str(), p_threshold.c_str());
	CHECK_MESSAGE(p_pass, p_gate << " " << p_name << ": " << p_measured << " (" << p_threshold << ")");
}

std::string fmt(const char *p_format, ...) {
	char buf[512];
	va_list a;
	va_start(a, p_format);
	std::vsnprintf(buf, sizeof(buf), p_format, a);
	va_end(a);
	return buf;
}

void check_fire_timing(Run &r_run, const sw::WeaponTable &p_table, uint8_t p_weapon_slot) {
	const std::optional<WeaponTickRecord> last = r_run.rec->last(r_run.slot);
	REQUIRE(last.has_value());
	if (!last) {
		return;
	}
	const sw::WeaponSlotState &ws = last->after.slots[p_weapon_slot];
	REQUIRE(ws.def < p_table.defs.size());
	if (ws.def >= p_table.defs.size()) {
		return;
	}
	const sw::WeaponSimDef &d = p_table.defs[ws.def];
	const sw::FireModeDef &mode = d.modes[ws.mode < d.mode_count ? ws.mode : 0];
	const std::string name = short_name(d.variant);
	r_run.rec->clear();
	if (mode.logic == sw::FireLogic::Automatic) {
		const size_t n = hold_fire(r_run, 8, 4.0);
		run_for(*r_run.s, 0.2);
		const std::vector<WeaponTickRecord> ticks = r_run.rec->ticks();
		const std::vector<sw::ShotEvent> shots = r_run.rec->shots(r_run.slot);
		const std::optional<sim::Tick> press = press_tick(ticks, r_run.slot);
		bool gaps = shots.size() >= 8;
		for (size_t i = 1; i < shots.size(); ++i) {
			gaps = gaps && shots[i].time - shots[i - 1].time == mode.interval;
		}
		const bool first = press && !shots.empty() && shots.front().time == sw::tick_start(*press);
		row("E1", name + " automatic: gaps = the table interval", gaps && first,
				fmt("%zu shots (%zu while held), first at the press tick's start: %s, gaps %s", shots.size(), n, first ? "yes" : "no",
						gaps ? "all equal" : "differ"),
				fmt("every gap %" PRId64 ", first at tick_start(press)", mode.interval));
		return;
	}

	const int gap_ms = int(std::max<double>(300.0, sw::units_to_seconds(mode.interval) * 1000.0 + 150.0));
	size_t one_each = 0;
	for (int i = 0; i < 5; ++i) {
		one_each += tap_fixed(r_run, 50, gap_ms) == 1 ? 1 : 0;
	}
	const std::vector<sw::ShotEvent> shots = r_run.rec->shots(r_run.slot);
	bool gaps = shots.size() == 5;
	for (size_t i = 1; i < shots.size(); ++i) {
		gaps = gaps && shots[i].time - shots[i - 1].time >= mode.interval;
	}
	row("E1", name + " single: one shot per tap, gaps >= the interval", one_each == 5 && gaps,
			fmt("%zu / 5 taps with exactly one shot, %zu shots, gaps %s", one_each, shots.size(), gaps ? ">= interval" : "too short"),
			fmt("5 / 5, every gap >= %" PRId64, mode.interval));
}

struct Flight {
	bool ok = false;
	sw::ShotEvent shot;
	std::vector<std::pair<sim::Tick, sim::Vec3>> path;
	std::optional<sim::Vec3> expired;
	int ticks_after_teleport = -1;
	float below_teleport = 0;
	size_t report_bytes = 0;
};

std::optional<double> drop_at(const Flight &p_f, double p_distance) {
	const sim::Vec3 &o = p_f.shot.origin;
	const sim::Vec3 &d = p_f.shot.direction;
	for (size_t i = 1; i < p_f.path.size(); ++i) {
		const sim::Vec3 &a = p_f.path[i - 1].second;
		const sim::Vec3 &b = p_f.path[i].second;
		const double ha = hdist(a, o), hb = hdist(b, o);
		if (ha <= p_distance && hb >= p_distance && hb > ha) {
			const double t = (p_distance - ha) / (hb - ha);
			double p[3], v[3];
			for (int k = 0; k < 3; ++k) {
				p[k] = double(a[k]) + (double(b[k]) - double(a[k])) * t;
				v[k] = p[k] - double(o[k]);
			}
			const double along = v[0] * double(d[0]) + v[1] * double(d[1]) + v[2] * double(d[2]);
			double perp2 = 0;
			for (int k = 0; k < 3; ++k) {
				const double c = v[k] - along * double(d[k]);
				perp2 += c * c;
			}
			return std::sqrt(perp2);
		}
	}
	return std::nullopt;
}

std::string variant_cfg(const std::string &p_base, const char *p_disc, const char *p_recovery) {
	std::string out;
	size_t at = 0;
	while (at < p_base.size()) {
		size_t end = p_base.find('\n', at);
		if (end == std::string::npos) {
			end = p_base.size();
		}
		std::string line = p_base.substr(at, end - at);
		if (line.rfind("disc", 0) == 0) {
			line = std::string("disc = ") + p_disc;
		} else if (line.rfind("recovery", 0) == 0) {
			line = std::string("recovery = ") + p_recovery;
		}
		out += line + "\n";
		at = end + 1;
	}
	return out;
}

class FireSource final : public host::CommandSource {
public:
	explicit FireSource(uint32_t p_phase) :
			phase_(p_phase) {}
	sim::Command command(sim::Tick p_tick, const sim::SoldierState &p_state) override {
		sim::Command c = sim::idle_command(p_tick, p_state.yaw, sim::quantize_pitch(0.35f), sim::Pose::Stand);
		if (((p_tick + phase_) / 6u) % 2u == 0) {
			c.buttons |= sw::weapon_buttons::kFire;
		}
		return c;
	}

private:
	uint32_t phase_ = 0;
};

double angle_between_deg(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	const double ax = p_a[0], ay = p_a[1], az = p_a[2], bx = p_b[0], by = p_b[1], bz = p_b[2];
	const double cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
	return std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), ax * bx + ay * by + az * bz) * 57.29577951308232;
}

double ks_d(std::vector<double> &r_u, const std::function<double(double)> &p_cdf) {
	std::sort(r_u.begin(), r_u.end());
	const double n = double(r_u.size());
	double d = 0;
	for (size_t i = 0; i < r_u.size(); ++i) {
		const double f = p_cdf(std::clamp(r_u[i], 0.0, 1.0));
		d = std::max({ d, f - double(i) / n, double(i + 1) / n - f });
	}
	return d;
}

struct DirectServer {
	host::GameServer server;
	std::vector<host::OutDatagram> out;
	std::optional<net::MatchInfo> match;
	std::vector<double> tick_ms;
	uint64_t out_bytes = 0;

	void tick() {
		out.clear();
		const auto start = std::chrono::steady_clock::now();
		server.tick(out);
		tick_ms.push_back(seconds_since(start) * 1000.0);
		for (const host::OutDatagram &d : out) {
			out_bytes += d.bytes.size();
			const std::optional<net::MessageType> t = net::peek_type(d.bytes);
			if (t && *t == net::MessageType::MatchInfo) {
				core::Result<net::MatchInfo> m = net::decode_match_info(d.bytes);
				if (m.ok()) {
					match = *m;
				}
			}
		}
	}
	void receive(const std::vector<uint8_t> &p_bytes, uint8_t p_channel) { server.on_receive(1, p_channel, p_bytes); }
};

double percentile(std::vector<double> p_v, double p_q) {
	if (p_v.empty()) {
		return 0;
	}
	std::sort(p_v.begin(), p_v.end());
	return p_v[std::min(p_v.size() - 1, size_t(p_q * double(p_v.size() - 1) + 0.5))];
}

}

TEST_SUITE("host-gunplay-server") {

	TEST_CASE("timing") {
		Metro *m = metro_gunplay();
		if (m == nullptr) {
			return;
		}
		const std::shared_ptr<const sw::WeaponTable> table = local_table(*m->level, mechanics_cfg_path());
		REQUIRE(table != nullptr);
		if (table == nullptr) {
			return;
		}
		for (uint8_t team = 1; team <= 2; ++team) {
			for (uint8_t kit = 0; kit < host::kKitCount; ++kit) {
				host::GunplayConfig g;
				g.player_team = team;
				g.player_kit = kit;
				Run r = start_run(m->level, g);
				if (r.s == nullptr) {
					return;
				}
				const host::GameServerStats gs = r.s->server.game_stats();
				CHECK(gs.gunplay_active);
				CHECK(gs.weapon_hash == host::match_weapon_hash(*table, m->level->gunplay->capsule_table));
				const std::optional<WeaponTickRecord> last = r.rec->last(r.slot);
				REQUIRE(last.has_value());
				if (!last) {
					return;
				}
				const sw::WeaponSlotState &primary = last->after.slots[0];
				const std::string variant = primary.def < table->defs.size() ? lower(table->defs[primary.def].variant) : std::string("?");
				std::printf("E1 team %u kit %s: slot %u, primary %s\n", unsigned(team), host::kit_role_name(host::KitRole(kit)), unsigned(r.slot),
						short_name(variant).c_str());
				if (team == 1 && kit == 0) {

					CHECK(ends_with(variant, "u_m16a4"));
					net::DebugRequest q;
					q.kind = net::DebugKind::Trace;
					q.ticks = 200;
					const uint32_t id = send_request(*r.s, q);
					run_for(*r.s, 0.15);
					r.rec->clear();
					hold_fire(r, 40, 4.0, 0, [&] {
						const std::optional<WeaponTickRecord> l = r.rec->last(r.slot);
						return l && (l->after.current().phase == sw::WeaponPhase::Reloading || l->after.current().magazine == 0);
					});
					const std::optional<net::DebugReport> rep = wait_report(*r.s, id, 10.0);
					REQUIRE(rep.has_value());
					const std::vector<WeaponTickRecord> ticks = r.rec->ticks();
					const std::vector<sw::ShotEvent> shots = r.rec->shots(r.slot);
					const std::optional<sim::Tick> press = press_tick(ticks, r.slot);
					size_t gaps_ok = 0;
					for (size_t i = 1; i < shots.size(); ++i) {
						gaps_ok += shots[i].time - shots[i - 1].time == 4'500'000 ? 1u : 0u;
					}
					const bool first = press && !shots.empty() && shots.front().time == sw::tick_start(*press);
					row("E1", "M16A3 one magazine held: 31 shots, 30 gaps of 4500000", shots.size() == 31 && gaps_ok == 30 && first,
							fmt("%zu shots, %zu gaps of 4500000, first %s the press tick %u's start", shots.size(), gaps_ok, first ? "at" : "NOT at",
									press ? unsigned(*press) : 0u),
							"31 shots; first at the press tick's start; all 30 gaps exactly 4500000");
					if (rep) {
						std::vector<sw::ShotEvent> traced;
						for (const net::TraceGunplayTick &t : rep->trace_gunplay) {
							traced.insert(traced.end(), t.shots.begin(), t.shots.end());
						}
						size_t equal = 0;
						for (size_t i = 0; i < std::min(traced.size(), shots.size()); ++i) {
							equal += shots_equal(traced[i], shots[i]) ? 1u : 0u;
						}
						const size_t bytes = net::encode_debug_report(*rep).size();
						row("E1", "M16A3: the server trace's shots = the server's ShotEvents", equal == shots.size() && traced.size() == shots.size(),
								fmt("%zu / %zu equal field for field (trace %zu ticks, %zu shots, %zu bytes, %u dropped)", equal, shots.size(),
										rep->trace_gunplay.size(), traced.size(), bytes, rep->trace_dropped),
								"every shot (time, index, seed, origin, aim, direction bits)");
					}
					run_for(*r.s, 2.8);
				} else {
					check_fire_timing(r, *table, 0);
				}
				if (kit == 0) {

					r.s->input.held_buttons = select_slot_field(1);
					const bool switched = poll_until(
							*r.s,
							[&] {
								const std::optional<WeaponTickRecord> l = r.rec->last(r.slot);
								return l && l->after.active == 1 && l->after.current().phase == sw::WeaponPhase::Ready;
							},
							6.0);
					CHECK_MESSAGE(switched, "no switch to the pistol");
					if (switched) {
						const uint32_t keep = select_slot_field(1);

						const std::optional<WeaponTickRecord> l = r.rec->last(r.slot);
						const sw::WeaponSimDef &d = table->defs[l->after.slots[1].def];
						const sw::FireModeDef &mode = d.modes[0];
						r.rec->clear();
						size_t one_each = 0;
						const int gap_ms = int(std::max<double>(300.0, sw::units_to_seconds(mode.interval) * 1000.0 + 150.0));
						for (int i = 0; i < 5; ++i) {
							one_each += tap_fixed(r, 50, gap_ms, keep) == 1 ? 1 : 0;
						}
						const std::vector<sw::ShotEvent> shots = r.rec->shots(r.slot);
						bool gaps = shots.size() == 5;
						for (size_t i = 1; i < shots.size(); ++i) {
							gaps = gaps && shots[i].time - shots[i - 1].time >= mode.interval;
						}
						row("E1", short_name(d.variant) + " single (pistol): one shot per tap", one_each == 5 && gaps && mode.logic == sw::FireLogic::Single,
								fmt("%zu / 5 taps with exactly one shot, %zu shots, gaps %s, logic %u", one_each, shots.size(), gaps ? ">= interval" : "too short",
										unsigned(mode.logic)),
								fmt("5 / 5, every gap >= %" PRId64, mode.interval));
					}
				}
				const host::GameServerStats end = r.s->server.game_stats();
				CHECK(end.gunplay.weapon_precondition_failures == 0);
				CHECK(end.step_precondition_failures == 0);
			}
		}
	}

	TEST_CASE("drop") {
		Metro *m = metro_gunplay();
		if (m == nullptr) {
			return;
		}
		const std::shared_ptr<const sw::WeaponTable> table = local_table(*m->level, mechanics_cfg_path());
		core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(m->level->static_world);
		REQUIRE(world.ok());
		if (table == nullptr || !world.ok()) {
			return;
		}
		host::GunplayConfig g;
		g.player_team = 1;
		g.player_kit = 0;
		Run r = start_run(m->level, g);
		if (r.s == nullptr) {
			return;
		}
		const std::optional<WeaponTickRecord> first = r.rec->last(r.slot);
		REQUIRE(first.has_value());
		const sw::WeaponSimDef &d = table->defs[first->after.slots[0].def];
		CHECK(ends_with(lower(d.variant), "u_m16a4"));
		const double flight = double(d.projectile.muzzle_speed) * sw::units_to_seconds(d.projectile.time_to_live);
		core::Result<host::SkyLane> lane = host::find_sky_lane(**world, float(flight), sw::BallisticsLimits{}.world_margin);
		REQUIRE_MESSAGE(lane.ok(), (lane.ok() ? std::string() : lane.error().message));
		if (!lane.ok()) {
			return;
		}
		std::printf("E2 sky lane: eye (%.1f, %.1f, %.1f) yaw %u, clear %.1f m, in-bounds %.1f m (flight %.1f m + 75 m spare)\n", double(lane->eye[0]),
				double(lane->eye[1]), double(lane->eye[2]), unsigned(lane->yaw), double(lane->clear_length), double(lane->in_bounds_length), flight);
		std::vector<Flight> flights;
		for (int k = 0; k < 3; ++k) {

			const bool ready = poll_until(
					*r.s,
					[&] {
						return r.s->frame->valid && alive_flag(r.s->frame->own_curr) && weapon_ready(*r.rec, r.slot) && r.rec->last(r.slot)->after.active == 0;
					},
					12.0);
			REQUIRE_MESSAGE(ready, "shot " << k << ": the shooter is not alive and ready");
			if (!ready) {
				break;
			}
			r.s->input.yaw = sim::yaw_radians(lane->yaw);
			r.s->input.pitch = 0.0f;
			r.s->input.held_buttons = 0;
			run_for(*r.s, 0.1);
			net::DebugRequest q;
			q.kind = net::DebugKind::Trace;
			q.ticks = 160;
			const uint32_t tid = send_request(*r.s, q);
			run_for(*r.s, 0.05);
			const std::optional<net::DebugReport> tp = teleport(*r.s, lane->feet, lane->yaw);
			REQUIRE(tp.has_value());
			if (!tp) {
				break;
			}
			unsigned tp_tick = 0;
			const size_t at = tp->text.find("at tick ");
			if (at != std::string::npos) {
				tp_tick = unsigned(std::strtoul(tp->text.c_str() + at + 8, nullptr, 10));
			}
			const size_t before = r.rec->shot_count(r.slot);
			hold_fire(r, 1, 1.0);
			const std::vector<sw::ShotEvent> shots = r.rec->shots(r.slot);
			const std::optional<net::DebugReport> rep = wait_report(*r.s, tid, 15.0);
			REQUIRE(rep.has_value());
			if (!rep || shots.size() <= before) {
				break;
			}
			Flight f;
			f.shot = shots[before];
			f.ticks_after_teleport = int(sw::tick_containing(f.shot.time)) - int(tp_tick);
			f.below_teleport = lane->eye[1] - f.shot.origin[1];
			f.report_bytes = net::encode_debug_report(*rep).size();
			for (const net::TraceGunplayTick &t : rep->trace_gunplay) {
				for (size_t i = 0; i < t.projectiles.size() && i < t.projectile_shots.size(); ++i) {
					if (t.projectile_shots[i] == f.shot.shot_index) {
						f.path.push_back({ t.tick, t.projectiles[i] });
					}
				}
			}
			run_for(*r.s, 0.3);
			for (const ImpactRecord &i : r.rec->impacts()) {
				if (i.impact.player_slot == r.slot && i.impact.shot_index == f.shot.shot_index && i.impact.kind == sw::ImpactKind::Expired) {
					f.expired = i.impact.position;
				}
			}
			f.ok = true;
			flights.push_back(f);
		}
		REQUIRE(flights.size() == 3);
		for (size_t k = 0; k < flights.size(); ++k) {
			const Flight &f = flights[k];
			const std::optional<double> d100 = drop_at(f, 100.0);
			const std::optional<double> d300 = drop_at(f, 300.0);
			std::printf("E2 shot %zu: index %u, %d ticks after the teleport, origin %.3f m below the teleport eye, %zu traced positions, report %zu bytes\n", k,
					f.shot.shot_index, f.ticks_after_teleport, double(f.below_teleport), f.path.size(), f.report_bytes);
			row("E2", fmt("M16A3 drop at 100 m (shot %zu)", k), d100 && std::fabs(*d100 - 0.1779) <= 0.01, d100 ? fmt("%.4f m", *d100) : "not reached",
					"0.1779 +- 0.01 m");
			row("E2", fmt("M16A3 drop at 300 m (shot %zu)", k), d300 && std::fabs(*d300 - 1.5981) <= 0.01, d300 ? fmt("%.4f m", *d300) : "not reached",
					"1.5981 +- 0.01 m");
			double along = -1;
			if (f.expired) {
				const double dh = std::hypot(double(f.shot.direction[0]), double(f.shot.direction[2]));
				along = hdist(*f.expired, f.shot.origin) / std::max(dh, 1e-9);
			}
			row("E2", fmt("M16A3 expiry (ImpactKind::Expired) along the launch line (shot %zu)", k), f.expired && std::fabs(along - 975.0) <= 0.01,
					f.expired ? fmt("%.4f m (horizontal %.4f m)", along, hdist(*f.expired, f.shot.origin)) : "no Expired impact", "975.0 +- 0.01 m");
			row("E2", fmt("shot %zu origin <= 0.1 m below the teleport point", k), f.below_teleport <= 0.1f && f.below_teleport >= -0.05f,
					fmt("%.4f m, %d ticks after the teleport", double(f.below_teleport), f.ticks_after_teleport), "<= 0.1 m");
		}
	}

	TEST_CASE("stk") {
		Metro *m = metro_gunplay();
		if (m == nullptr) {
			return;
		}
		core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(m->level->static_world);
		REQUIRE(world.ok());
		if (!world.ok()) {
			return;
		}
		const float eye = m->level->gunplay->soldier.poses.empty() ? 1.6f : m->level->gunplay->soldier.poses[0].eye_height;
		const std::optional<sw::RangeLane> lane = sw::find_range_lane(**world, m->level->spawns, eye, 65.0f);
		REQUIRE_MESSAGE(lane.has_value(), "no 65 m range lane on Metro");
		if (!lane) {
			return;
		}
		std::printf("E3 range lane: spawn %u, %.1f m clear, yaw %u\n", lane->spawn_index, double(lane->length), unsigned(lane->yaw));
		struct Row {
			float distance;
			sw::HitZone zone;
			int kills_in;
			double per_hit;
			const char *gate;
		};
		struct Weapon {
			uint8_t kit;
			const char *tail;
			std::vector<Row> rows;
		};
		const std::vector<Weapon> weapons = {
			{ 0, "u_m16a4",
					{ { 5, sw::HitZone::Chest, 4, 25.0, "E3" }, { 5, sw::HitZone::Head, 2, 50.0, "E4" }, { 5, sw::HitZone::Legs, -1, 22.75, "E3" },
							{ 60, sw::HitZone::Chest, 6, -1, "E3" }, { 60, sw::HitZone::Head, 3, -1, "E3" } } },
			{ 3, "u_mk11", { { 5, sw::HitZone::Head, 1, 117.5, "E4" }, { 60, sw::HitZone::Head, 2, -1, "E4" } } },
		};
		for (const Weapon &w : weapons) {
			host::GunplayConfig g;
			g.exact_aim = true;
			g.target_bots = 1;
			g.target_team = 2;
			g.player_team = 1;
			g.player_kit = w.kit;
			Run r = start_run(m->level, g);
			if (r.s == nullptr) {
				return;
			}
			const std::shared_ptr<const sw::WeaponTable> table = local_table(*m->level, mechanics_cfg_path(), true);
			REQUIRE(table != nullptr);
			const std::optional<WeaponTickRecord> l0 = r.rec->last(r.slot);
			REQUIRE(l0.has_value());
			const std::string variant = lower(table->defs[l0->after.slots[0].def].variant);
			CHECK_MESSAGE(ends_with(variant, w.tail), variant);
			CHECK((r.s->server.game_stats().weapon_flags & net::weapon_flags::kExactAim) != 0);
			REQUIRE(teleport(*r.s, lane->feet, lane->yaw).has_value());
			r.s->input.yaw = sim::yaw_radians(lane->yaw);
			r.s->input.pitch = 0;
			run_for(*r.s, 0.3);

			sim::Vec3 origin = own_eye(*r.s->frame);
			const float s = std::sin(sim::yaw_radians(lane->yaw)), c = std::cos(sim::yaw_radians(lane->yaw));
			for (const Row &rw : w.rows) {
				sim::Vec3 at = { lane->feet[0] + s * rw.distance, lane->feet[1], lane->feet[2] + c * rw.distance };
				sim::StaticHit floor;
				if ((*world)->ray({ at[0], at[1] + 1.5f, at[2] }, { 0.0f, -4.0f, 0.0f }, sim::query_mask::kMovement, &floor)) {
					at[1] = at[1] + 1.5f - 4.0f * floor.fraction;
				}
				const std::optional<net::DebugReport> placed = spawn_target(*r.s, 0, at, uint16_t(lane->yaw + 32768u), sim::Pose::Stand);
				REQUIRE(placed.has_value());
				CHECK_MESSAGE(placed->ok, placed->text);
				run_for(*r.s, 0.3);
				const std::optional<net::DebugReport> info = target_info(*r.s, 0);
				REQUIRE(info.has_value());
				if (!info || info->targets.empty()) {
					CHECK_MESSAGE(false, "no TargetInfo row");
					continue;
				}
				sim::Vec3 point{};
				REQUIRE(sw::zone_aim_point(info->targets[0].target, rw.zone, point));
				const uint64_t deaths_before = r.s->server.game_stats().gunplay.deaths;
				int hits = 0, misses = 0, wrong_zone = 0;
				bool killed = false;
				double first_damage = -1;
				for (int shot = 0; shot < 12 && !killed; ++shot) {

					const sw::WeaponSimDef &wd = table->defs[l0->after.slots[0].def];
					const double t_flight = hdist(point, origin) / std::max(double(wd.projectile.muzzle_speed), 1.0);
					const sim::Vec3 aim_point = { point[0], point[1] + float(0.5 * std::fabs(double(wd.projectile.gravity)) * t_flight * t_flight), point[2] };
					aim_at(origin, aim_point, r.s->input.yaw, r.s->input.pitch);
					run_for(*r.s, 0.08);
					const size_t before = r.rec->shot_count(r.slot);
					hold_fire(r, 1, 1.0);
					const std::vector<sw::ShotEvent> shots = r.rec->shots(r.slot);
					if (shots.size() <= before) {
						++misses;
						continue;
					}
					const sw::ShotEvent e = shots[before];
					origin = e.origin;
					bool hit = false;
					poll_until(
							*r.s,
							[&] {
								for (const DamageRecord &dr : r.rec->damage()) {
									if (dr.event.attacker_slot == r.slot && dr.event.shot_index == e.shot_index) {
										return true;
									}
								}
								return false;
							},
							0.5);
					for (const DamageRecord &dr : r.rec->damage()) {
						if (dr.event.attacker_slot == r.slot && dr.event.shot_index == e.shot_index && dr.applied && dr.event.target == 0) {
							hit = true;
							++hits;
							wrong_zone += dr.event.zone != rw.zone ? 1 : 0;
							if (first_damage < 0) {
								first_damage = dr.event.amount;
							}
							killed = dr.health_after <= 0.0f;
						}
					}
					misses += hit ? 0 : 1;
					run_for(*r.s, 0.3);
				}
				run_for(*r.s, 0.2);
				const uint64_t deaths = r.s->server.game_stats().gunplay.deaths - deaths_before;
				const std::string name = fmt("%s %s at %.0f m", short_name(variant).c_str(), sw::hit_zone_name(rw.zone), double(rw.distance));
				const bool count_ok = rw.kills_in < 0 || (killed && hits == rw.kills_in);
				const bool damage_ok = rw.per_hit < 0 || std::fabs(first_damage - rw.per_hit) <= 1e-3;
				row(rw.gate, name, count_ok && damage_ok && misses == 0 && wrong_zone == 0 && (!killed || deaths == 1),
						fmt("%d hits to %s (%d misses, %d other zone), damage per hit %.4f, %" PRIu64 " death event(s)", hits, killed ? "kill" : "no kill", misses,
								wrong_zone, first_damage, deaths),
						fmt("%s; %s", rw.kills_in < 0 ? "kills reported" : fmt("kill in %d", rw.kills_in).c_str(),
								rw.per_hit < 0 ? "damage reported" : fmt("%.3f +- 1e-3", rw.per_hit).c_str()));
			}
			CHECK(r.s->server.game_stats().gunplay.weapon_precondition_failures == 0);
		}
	}

	TEST_CASE("reload") {
		Metro *m = metro_gunplay();
		if (m == nullptr) {
			return;
		}
		const std::shared_ptr<const sw::WeaponTable> table = local_table(*m->level, mechanics_cfg_path());
		REQUIRE(table != nullptr);
		if (table == nullptr) {
			return;
		}

		for (uint8_t team = 1; team <= 2; ++team) {
			for (uint8_t kit = 0; kit < host::kKitCount; ++kit) {
				const host::KitLoadout &l = m->level->gunplay->kits.loadout(team, kit);
				for (uint8_t slot : { uint8_t(0), uint8_t(1) }) {
					if (l.defs[slot] < table->defs.size()) {
						const sw::WeaponSimDef &d = table->defs[l.defs[slot]];
						std::printf("E5 team %u %s slot %u %-12s ReloadTimeBulletsLeft %.3f s, ReloadTime %.3f s, threshold %.2f, capacity %d\n", unsigned(team),
								host::kit_role_name(host::KitRole(kit)), unsigned(slot), short_name(d.variant).c_str(), sw::units_to_seconds(d.reload_tactical),
								sw::units_to_seconds(d.reload_empty), double(d.reload_threshold), d.magazine_capacity);
					}
				}
			}
		}
		host::GunplayConfig g;
		g.player_team = 1;
		g.player_kit = 0;
		Run r = start_run(m->level, g);
		if (r.s == nullptr) {
			return;
		}
		struct Case {
			const char *name;
			uint8_t weapon_slot;
			bool empty;
			sw::SimTime duration;
			sw::SimTime commit;
			int32_t magazine_after;
		};
		const Case cases[] = {
			{ "M16A3 tactical reload", 0, false, 108'000'000, 91'800'000, 31 },
			{ "M16A3 empty reload", 0, true, 142'200'000, 0, 30 },
			{ "M9 tactical reload", 1, false, 81'000'000, 0, -1 },
			{ "M9 empty reload", 1, true, 96'000'000, 0, -1 },
		};
		for (const Case &c : cases) {
			const uint32_t keep = c.weapon_slot == 0 ? select_slot_field(0) : select_slot_field(1);
			r.s->input.held_buttons = keep;
			const bool ready = poll_until(
					*r.s,
					[&] {
						const std::optional<WeaponTickRecord> l = r.rec->last(r.slot);
						return l && l->after.active == c.weapon_slot && l->after.current().phase == sw::WeaponPhase::Ready;
					},
					8.0);
			REQUIRE_MESSAGE(ready, c.name << ": the weapon is not ready");
			if (!ready) {
				return;
			}
			const sw::WeaponSimDef &d = table->defs[r.rec->last(r.slot)->after.current().def];
			const bool automatic = d.modes[0].logic == sw::FireLogic::Automatic;
			r.rec->clear();
			if (!c.empty) {
				if (automatic) {
					hold_fire(r, 5, 2.0, keep);
				} else {
					tap_fixed(r, 50, 250, keep);
					tap_fixed(r, 50, 250, keep);
				}
				run_for(*r.s, 0.3);
				set_buttons(r, keep | sw::weapon_buttons::kReload);
				run_for(*r.s, 0.1);
				set_buttons(r, keep);
			} else {

				const auto is_empty = [&] {
					const std::optional<WeaponTickRecord> l = r.rec->last(r.slot);
					return l && (l->after.current().magazine == 0 || l->after.current().phase == sw::WeaponPhase::Reloading);
				};
				if (automatic) {
					hold_fire(r, 64, 6.0, keep, is_empty);
				} else {
					for (int i = 0; i < 40 && !is_empty(); ++i) {
						tap_fixed(r, 50, int(sw::units_to_seconds(d.modes[0].interval) * 1000.0) + 80, keep);
					}
				}
				run_for(*r.s, 0.2);
				if (timed_events(r.rec->ticks(), r.slot, sw::weapon_events::kReloadStarted).empty()) {
					set_buttons(r, keep | sw::weapon_buttons::kReload);
					run_for(*r.s, 0.1);
					set_buttons(r, keep);
				}
			}
			poll_until(*r.s, [&] { return !timed_events(r.rec->ticks(), r.slot, sw::weapon_events::kReloadFinished).empty(); }, 5.0);
			const std::vector<WeaponTickRecord> ticks = r.rec->ticks();
			const std::vector<sw::TimedWeaponEvent> started = timed_events(ticks, r.slot, sw::weapon_events::kReloadStarted);
			const std::vector<sw::TimedWeaponEvent> committed = timed_events(ticks, r.slot, sw::weapon_events::kReloadCommitted);
			const std::vector<sw::TimedWeaponEvent> finished = timed_events(ticks, r.slot, sw::weapon_events::kReloadFinished);
			int32_t magazine = -1;
			int32_t magazine_before = -1;
			for (const WeaponTickRecord &t : ticks) {
				if (!started.empty() && t.command.tick < sw::tick_containing(started.front().time)) {
					magazine_before = t.after.slots[c.weapon_slot].magazine;
				}
				for (const sw::TimedWeaponEvent &e : t.timed) {
					if (e.event == sw::weapon_events::kReloadFinished) {
						magazine = t.after.slots[c.weapon_slot].magazine;
					}
				}
			}
			const bool have = !started.empty() && !committed.empty() && !finished.empty();
			const sw::SimTime duration = have ? finished.front().time - started.front().time : -1;
			const sw::SimTime commit = have ? committed.front().time - started.front().time : -1;
			const sw::SimTime want_commit = c.commit != 0 ? c.commit : (c.empty ? d.reload_commit_empty : d.reload_commit_tactical);
			const int32_t want_mag = c.magazine_after >= 0 ? c.magazine_after : (c.empty && table->mechanics.empty_reload_unchambered ? d.magazine_capacity - 1 : d.magazine_capacity);
			row("E5", c.name, have && duration == c.duration && commit == want_commit && magazine == want_mag,
					fmt("%s: start -> finish %" PRId64 ", commit at +%" PRId64 ", magazine %d -> %d", short_name(d.variant).c_str(), duration, commit, magazine_before,
							magazine),
					fmt("%" PRId64 ", commit +%" PRId64 ", magazine %d", c.duration, want_commit, want_mag));
		}
		CHECK(r.s->server.game_stats().gunplay.weapon_precondition_failures == 0);
	}

	TEST_CASE("variants") {
		Metro *m = metro_gunplay();
		if (m == nullptr) {
			return;
		}
		const std::string base = read_text(mechanics_cfg_path());
		REQUIRE(!base.empty());
		const std::filesystem::path dir = std::filesystem::temp_directory_path() / "openbf3-sv-variants";
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		struct Variant {
			const char *disc;
			const char *recovery;
			sw::DiscDistribution d;
			sw::RecoveryShape r;
		};
		const Variant variants[] = { { "uniform_area", "linear", sw::DiscDistribution::UniformArea, sw::RecoveryShape::Linear },
			{ "uniform_area", "exponential", sw::DiscDistribution::UniformArea, sw::RecoveryShape::Exponential },
			{ "uniform_radius", "linear", sw::DiscDistribution::UniformRadius, sw::RecoveryShape::Linear },
			{ "uniform_radius", "exponential", sw::DiscDistribution::UniformRadius, sw::RecoveryShape::Exponential } };
		std::set<uint64_t> hashes;
		uint64_t shipped_hash = 0;
		for (const Variant &v : variants) {
			const std::filesystem::path cfg = dir / (std::string(v.disc) + "_" + v.recovery + ".cfg");
			{
				std::ofstream out(cfg, std::ios::binary);
				out << variant_cfg(base, v.disc, v.recovery);
			}
			const std::string name = std::string(v.disc) + " + " + v.recovery;
			const std::shared_ptr<const sw::WeaponTable> table = local_table(*m->level, cfg.string());
			REQUIRE(table != nullptr);
			if (table == nullptr) {
				return;
			}
			CHECK(table->mechanics.disc == v.d);
			CHECK(table->mechanics.recovery == v.r);
			CHECK_FALSE(table->mechanics.measured);

			host::GunplayConfig g;
			g.mechanics_cfg = cfg.string();
			g.player_team = 1;
			g.player_kit = 0;
			Run r = start_run(m->level, g);
			if (r.s == nullptr) {
				return;
			}
			const uint64_t server_hash = r.s->server.game_stats().weapon_hash;
			const uint64_t client_hash = host::match_weapon_hash(*table, m->level->gunplay->capsule_table);
			hashes.insert(server_hash);
			if (v.d == sw::DiscDistribution::UniformArea && v.r == sw::RecoveryShape::Linear) {
				shipped_hash = server_hash;
			}
			row("E6", name + ": the client-built table's hash = the server's", server_hash == client_hash,
					fmt("server %016" PRIx64 ", client %016" PRIx64, server_hash, client_hash), "equal");

			run_for(*r.s, 1.5);
			r.rec->clear();
			hold_fire(r, 10, 3.0);
			run_for(*r.s, 0.3);
			{
				const std::vector<WeaponTickRecord> ticks = r.rec->ticks();
				const sw::WeaponSimDef *d = nullptr;
				size_t shots = 0;
				double pitch = 0;
				bool first = true;
				bool started = false;
				float measured = NAN;
				for (const WeaponTickRecord &t : ticks) {
					if (t.slot != r.slot) {
						continue;
					}
					if (!started && t.shots.empty()) {
						continue;
					}
					if (!started) {
						started = true;
						d = &table->defs[t.after.slots[t.after.active].def];
					}
					const sw::SwayContext ctx;
					const sw::Recoil rec = sw::effective_recoil(*d, ctx);
					for (size_t k = 0; k < t.shots.size() && shots < 10; ++k) {
						const double up = double(rec.up_per_shot) * (first ? double(d->first_shot_recoil_multiplier) : 1.0);
						first = false;
						pitch = std::min(pitch + up, std::max(double(rec.up_max), 0.0));
						++shots;
					}
					const bool firing = !t.shots.empty();
					if (v.r == sw::RecoveryShape::Exponential) {
						pitch *= double(firing ? d->recovery_factor_firing[0][0] : d->recovery_factor_idle[0][0]);
					} else {
						const double step = double(d->sway[0][0].recoil.decrease_factor) * (firing ? double(d->shooting_recoil_decrease_scale) : 1.0) / double(sim::kSimHz);
						pitch = std::max(0.0, pitch - step);
					}
					if (shots == 10) {
						measured = t.after.slots[t.after.active].recoil.pitch;
						break;
					}
				}
				row("E6", name + ": recoil pitch after a 10-round burst = the variant's recurrence", shots == 10 && std::fabs(double(measured) - pitch) <= 1e-4,
						fmt("%.6f deg (recurrence %.6f, diff %.2e)", double(measured), pitch, double(measured) - pitch), "within 1e-4 deg");
			}

			const uint64_t shots_before = r.s->server.game_stats().gunplay.shots;
			run_for(*r.s, 10.0, [&](bool, uint64_t, double p_t) {
				const double phase = std::fmod(p_t, 3.0);
				uint32_t b = 0;
				if (std::fmod(p_t, 1.0) < 0.4) {
					b |= sw::weapon_buttons::kFire;
				}
				if (phase >= 2.5 && phase < 2.6) {
					b = sw::weapon_buttons::kReload;
				}
				r.s->input.held_buttons = b;
			});
			r.s->input.held_buttons = 0;
			const host::GameServerStats gs = r.s->server.game_stats();
			const host::ClientStats cs = r.s->client.stats();
			row("E6", name + ": 600 ticks of mixed fire", gs.gunplay.weapon_precondition_failures == 0 && gs.step_precondition_failures == 0 && gs.gunplay.shots > shots_before,
					fmt("%" PRIu64 " shots, weapon precondition failures %" PRIu64 ", motor %" PRIu64 " (client motor reconciliations %" PRIu64 "; weapon prediction is cl's)",
							gs.gunplay.shots - shots_before, gs.gunplay.weapon_precondition_failures, gs.step_precondition_failures, cs.reconciliations),
					"0 precondition failures");
			r.s.reset();

			auto rec = std::make_shared<GunplayRecorder>();
			host::GameServer server;
			host::GameServerConfig cfg2;
			cfg2.level = m->level;
			cfg2.max_slots = 40;
			cfg2.dev_commands = true;
			cfg2.seed = 7;
			cfg2.gunplay.mechanics_cfg = cfg.string();
			cfg2.gunplay.infinite_magazine = true;
			cfg2.gunplay.observer = rec;
			const core::Status st = server.init(cfg2);
			REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
			if (!st.ok()) {
				return;
			}
			for (uint32_t i = 0; i < 32; ++i) {
				CHECK(server.add_local_soldier(std::make_unique<FireSource>(i), 1 + int(i % 2u)) >= 0);
			}
			std::vector<host::OutDatagram> out;
			std::vector<double> u;
			for (int t = 0; t < 4000 && u.size() < 2000; ++t) {
				out.clear();
				server.tick(out);
				if (t % 60 == 59) {
					u.clear();
					for (const WeaponTickRecord &w : rec->ticks()) {
						for (const sw::ShotEvent &e : w.shots) {
							if (e.spread > 0.0f && u.size() < 2000) {
								u.push_back(angle_between_deg(e.aim, e.direction) / double(e.spread));
							}
						}
					}
				}
			}
			REQUIRE(u.size() == 2000);
			std::vector<double> u_other = u;
			const auto area = [](double p_x) { return p_x * p_x; };
			const auto radius = [](double p_x) { return p_x; };
			const bool is_area = v.d == sw::DiscDistribution::UniformArea;
			const double d_own = ks_d(u, is_area ? std::function<double(double)>(area) : std::function<double(double)>(radius));
			const double d_other = ks_d(u_other, is_area ? std::function<double(double)>(radius) : std::function<double(double)>(area));
			const double bound = 1.63 / std::sqrt(2000.0);
			row("E6", name + ": 2,000 server shot offsets fit the variant's radial CDF", d_own <= bound,
					fmt("KS D %.4f (against the other disc %.4f)", d_own, d_other), fmt("D <= %.4f", bound));
			CHECK(d_other > bound);
			CHECK(server.stats().gunplay.weapon_precondition_failures == 0);
		}
		row("E6", "four distinct weapon_hash values", hashes.size() == 4, fmt("%zu distinct", hashes.size()), "4");

		{
			const std::filesystem::path other = dir / "mismatch.cfg";
			{
				std::ofstream out(other, std::ios::binary);
				out << variant_cfg(base, "uniform_radius", "linear");
			}
			const std::shared_ptr<const sw::WeaponTable> t = local_table(*m->level, other.string());
			const uint64_t h = t != nullptr ? host::match_weapon_hash(*t, m->level->gunplay->capsule_table) : 0;
			row("E6", "a client cfg differing from the server's gives another weapon_hash", t != nullptr && h != shipped_hash && shipped_hash != 0,
					fmt("server %016" PRIx64 ", client %016" PRIx64 " (the disconnect itself: cl)", shipped_hash, h), "different");
		}
		std::filesystem::remove_all(dir, ec);
	}

	TEST_CASE("hostile") {
		Metro *m = metro_gunplay();
		if (m == nullptr) {
			return;
		}
		const auto setup = [&](DirectServer &r_d) {
			host::GameServerConfig cfg;
			cfg.level = m->level;
			cfg.max_slots = 16;
			cfg.dev_commands = true;
			cfg.walkers = 2;
			cfg.seed = 11;
			cfg.gunplay.mechanics_cfg = mechanics_cfg_path();
			cfg.gunplay.target_bots = 2;
			const core::Status st = r_d.server.init(cfg);
			REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
			r_d.server.on_connect(1);
			net::Hello hello;
			hello.name = "sv-hostile";
			r_d.receive(net::encode_hello(hello), net::kChannelReliable);
			r_d.tick();
			r_d.receive(net::encode_ready({}), net::kChannelReliable);
			r_d.tick();
		};
		DirectServer control, hostile;
		setup(control);
		setup(hostile);
		REQUIRE(control.match.has_value());
		REQUIRE(hostile.match.has_value());
		const uint8_t client = control.match->slot;
		CHECK(hostile.match->slot == client);
		CHECK(control.match->weapon_hash == hostile.match->weapon_hash);
		std::mt19937 rng(5);
		uint32_t seq_c = 1, seq_h = 1;
		size_t mismatched_ticks = 0;
		size_t compared = 0;
		uint64_t kit_selects = 0;
		const auto base_command = [&](sim::Tick p_tick, uint32_t p_seq) {
			sim::Command c;
			c.tick = p_tick;
			c.sequence = p_seq;
			c.pitch = sim::quantize_pitch(1.0f);
			c.buttons = ((p_tick / 10u) % 2u) == 0 ? sw::weapon_buttons::kFire : 0u;
			return c;
		};
		for (int i = 0; i < 900; ++i) {
			const sim::Tick next = control.server.current_tick() + 1u;
			{
				net::CommandPacket p;
				p.count = 1;
				p.commands[0] = base_command(next, seq_c++);
				control.receive(net::encode_command_packet(p), net::kChannelUnreliable);
			}

			for (int k = 0; k < 4; ++k) {
				net::CommandPacket p;
				const sim::Tick base_tick = k == 2 ? next + 40u : (k == 3 ? (next > 5 ? next - 5u : 1u) : next + uint32_t(k));
				p.count = k == 1 ? 4 : 1;
				const uint32_t newest_seq = seq_h + 4u;
				seq_h += 5u;
				for (uint8_t j = 0; j < p.count; ++j) {
					sim::Command c = base_command(base_tick - j, newest_seq - j);
					c.buttons |= (15u << sw::weapon_buttons::kSlotShift) | fire_mode_field(uint8_t((base_tick + j) % 4u));
					c.view_back_q8 = 65535;
					c.view_quality = uint8_t(sim::ViewQuality::Interpolated);
					c.view_seq8 = uint8_t(rng());
					p.commands[j] = c;
				}
				p.newest_snapshot = uint16_t(rng());
				p.snapshot_ack_bits = uint32_t(rng());
				p.others_ack_bits = uint32_t(rng());
				hostile.receive(net::encode_command_packet(p), net::kChannelUnreliable);
			}
			if (i % 30 == 0) {
				net::KitSelect k;
				k.kit = 1;
				k.team = 7;
				std::vector<uint8_t> bytes = net::encode_kit_select(k);
				hostile.receive(bytes, net::kChannelReliable);
				if (!bytes.empty()) {
					bytes.pop_back();
					hostile.receive(bytes, net::kChannelReliable);
					std::vector<uint8_t> garbage(9);
					for (uint8_t &b : garbage) {
						b = uint8_t(rng());
					}
					garbage[0] = bytes.empty() ? 0 : net::encode_kit_select({})[0];
					hostile.receive(garbage, net::kChannelReliable);
				}
				kit_selects += 3;
				net::DebugRequest q;
				q.kind = net::DebugKind::SpawnTarget;
				q.slot = client;
				q.position = { 0, 1e30f, 0 };
				hostile.receive(net::encode_debug(q), net::kChannelReliable);
				q.slot = 120;
				hostile.receive(net::encode_debug(q), net::kChannelReliable);
				q.kind = net::DebugKind::TargetInfo;
				q.slot = 99;
				hostile.receive(net::encode_debug(q), net::kChannelReliable);
			}
			control.tick();
			hostile.tick();
			const host::Gunplay *gc = control.server.gunplay();
			const host::Gunplay *gh = hostile.server.gunplay();
			REQUIRE(gc != nullptr);
			REQUIRE(gh != nullptr);
			bool same = true;
			for (uint32_t s = 0; s < 16; ++s) {
				if (s == client) {
					continue;
				}
				const sim::SoldierState *a = control.server.soldier(s);
				const sim::SoldierState *b = hostile.server.soldier(s);
				if ((a == nullptr) != (b == nullptr)) {
					same = false;
					continue;
				}
				if (a != nullptr) {
					same = same && sim::soldier_state_hash(*a) == sim::soldier_state_hash(*b);
					same = same && sw::soldier_weapons_hash(gc->weapons(uint8_t(s))) == sw::soldier_weapons_hash(gh->weapons(uint8_t(s)));
					++compared;
				}
			}
			mismatched_ticks += same ? 0 : 1;
		}
		const host::GameServerStats a = control.server.stats();
		const host::GameServerStats b = hostile.server.stats();
		const host::GunplayStats &hb = b.gunplay;
		std::printf("E17 hostile: clamps slot_field %" PRIu64 ", fire_mode_changes %" PRIu64 ", view_refused %" PRIu64 ", no_ack_state %" PRIu64
					", kit_select %" PRIu64 " / refused %" PRIu64 ", bad datagrams %" PRIu64 " (control %" PRIu64 "), late %" PRIu64 ", replaced %" PRIu64 "; shots %" PRIu64
					" (control %" PRIu64 ")\n",
				hb.slot_field_clamped, hb.fire_mode_changes, hb.view_refused, hb.no_ack_state, hb.kit_select, hb.kit_select_refused, b.bad_datagrams, a.bad_datagrams,
				b.slots[client].commands_late, b.slots[client].commands_replaced, hb.shots, a.gunplay.shots);
		row("E17", "hostile client changes no other soldier (state + weapon hashes per tick)", mismatched_ticks == 0 && compared > 0,
				fmt("%zu / 900 ticks differ (%zu soldier-ticks compared)", mismatched_ticks, compared), "0 ticks differ");
		row("E17", "wanted slot 15 clamped and counted", hb.slot_field_clamped > 0, fmt("%" PRIu64, hb.slot_field_clamped), "> 0");

		row("E17", "view_back 65535 / view_seq outside the history: refused or clamped, counted",
				hb.shots > 0 && hb.view_refused + hb.no_ack_state + hb.lag.clamped >= hb.shots && a.gunplay.view_refused == 0 && a.gunplay.no_ack_state == 0,
				fmt("%" PRIu64 " refused + %" PRIu64 " without an ack state + %" PRIu64 " clamped of %" PRIu64 " shots (provider: present %" PRIu64 ", rewound %" PRIu64
					", acks ignored %" PRIu64 ")",
						hb.view_refused, hb.no_ack_state, hb.lag.clamped, hb.shots, hb.lag.present, hb.lag.rewound, hb.lag.acks_ignored),
				"every such shot counted");
		row("E17", "fire-mode counter spam bounded (<= 3 steps per command)", hb.fire_mode_changes <= 3u * 900u, fmt("%" PRIu64 " changes", hb.fire_mode_changes),
				"<= 3 per applied command");
		row("E17", "KitSelect team 7 / malformed refused and counted", hb.kit_select_refused + (b.bad_datagrams - a.bad_datagrams) >= kit_selects && hb.kit_select == 0,
				fmt("%" PRIu64 " refused, %" PRIu64 " accepted, bad datagrams +%" PRIu64 " of %" PRIu64 " sent", hb.kit_select_refused, hb.kit_select,
						b.bad_datagrams - a.bad_datagrams, kit_selects),
				"all refused");
		const double p99_a = percentile(control.tick_ms, 0.99), p99_b = percentile(hostile.tick_ms, 0.99);
		row("E17", "server tick unaffected (precondition failures, tick time)", b.step_precondition_failures == 0 && hb.weapon_precondition_failures == 0 &&
						b.tick == a.tick && p99_b <= 16.7,
				fmt("ticks %u / %u, precondition failures %" PRIu64 " / %" PRIu64 ", tick p99 %.3f ms (control %.3f ms; wall clock, noisy)", b.tick, a.tick,
						b.step_precondition_failures, hb.weapon_precondition_failures, p99_b, p99_a),
				"0 failures, every tick ran, p99 within the 16.7 ms tick");
	}

	TEST_CASE("install") {
		Metro *m = metro_gunplay();
		if (m == nullptr) {
			return;
		}
		check_install_unchanged(*m);
	}
}
