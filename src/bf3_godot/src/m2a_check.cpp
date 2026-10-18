// The M2a in-game check (docs/design/m2a.md §1, §15 gx row; owner gx; as built: docs/design/m2a-gx.md §4). See
// m2a_check.h for the cases.
//
// Every case is a list of steps run one per display frame through the real path: the input mapper (scripted:
// set_scripted + m2a_script_*), the client thread, the loopback server, the PresentationFrame, present_weapon, the
// drained events, the viewmodel, the HUD feed. A step returns true when it is done; each has a timeout that fails its
// case. The rows compare what the game showed with the data (the WeaponSimDef, the kit table) and with the server's
// own record (M2aServerRecorder, a GunplayObserver on the server thread).
#include "m2a_check.h"

#include "fp_camera.h"
#include "fx_renderer.h"
#include "game_audio.h"
#include "game_crowd.h"
#include "game_fx.h"
#include "godot_util.h"
#include "hud_controls.h"
#include "level_session_m2a.h"
#include "viewmodel.h"

#include "bf3/audio/audio_config.h"
#include "bf3/audio/game_audio_check.h"
#include "bf3/fx/simulator.h"
#include "bf3/fx/triggers.h"

#include "bf3/host/firefight_run.h"
#include "bf3/host/kits.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/command.h"
#include "bf3/sim/weapons/hit_target.h"
#include "bf3/sim/weapons/shooting_range.h"
#include "bf3/sim/weapons/sim_time.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>

#ifndef OPENBF3_M2A_HAVE_FX
#define OPENBF3_M2A_HAVE_FX 0
#endif
#ifndef OPENBF3_M2A_HAVE_AUDIO
#define OPENBF3_M2A_HAVE_AUDIO 0
#endif
#ifndef OPENBF3_M2A_HAVE_BT
#define OPENBF3_M2A_HAVE_BT 0
#endif

namespace bf3::godot_side {

using namespace godot;
namespace host = bf3::host;
namespace net = bf3::net;
namespace sim = bf3::sim;
namespace sw = bf3::sim::weapons;
namespace fx = bf3::fx;
namespace ba = bf3::audio;

namespace {

constexpr double kPi = 3.14159265358979323846;
// E7: the shooter (the own soldier, heard in 3p) aims this far up, so its rounds leave Metro without an impact sound
// in the recording; the listener sits on the aim axis (au's G7 geometry).
constexpr double kAudioPitchDeg = 60.0;
// E8: the predicted M16A3 shots of the 1p flash row (3 sigma at p 0.75), split over the two surfaces.
constexpr uint64_t kFxShots = 1000;
constexpr double kFxOilfireSeconds = 15.0;
constexpr int64_t kFxOilfireParticles = 80; // fx G4's steady state (+-1)
constexpr const char *kFxOilfire = "fx/ambient/levelspecific/mp_12/emitters_oilfire/em_amb_mp12_oilfire_l_fire_01"; // MP_012 (fx_demo.cpp)
// fx G5's named cells (fx_triggers_tests.cpp "cells": the PhysicsMaterialIndex, the effect and the decal leaf names).
struct FxNamedCell {
	uint8_t material;
	const char *name;
	const char *effect;
	const char *decal;
};
constexpr std::array<FxNamedCell, 4> kFxNamedCells{ { { 8, "concrete", "fx_impact_concrete_01_s_xp2", "decal_concrete_s_01" },
		{ 2, "brick", "fx_impact_concrete_01_s", "decal_brick_s_01" }, { 10, "glass", "fx_impact_glass_01_s", "decal_glass_s_01" },
		{ 3, "metal", "fx_impact_metal_01_s", "decal_metalarmor_s_01" } } };
constexpr const char *kFxSoldierEffect = "fx_impact_soldier_body_s";
// E9 / E14 (m2a.md §1): 10 s warm-up, 60 s in alternating 5 s FX-on / FX-off windows (the S3 G1b method), 2560 x 1440.
constexpr double kFfWarmupSeconds = 10.0;
constexpr double kFfWindowSeconds = 5.0;
constexpr uint32_t kFfWindows = 12;
constexpr int32_t kFfWidth = 2560, kFfHeight = 1440;

const FxNamedCell *fx_named_cell(uint8_t p_material) {
	for (const FxNamedCell &c : kFxNamedCells) {
		if (c.material == p_material) {
			return &c;
		}
	}
	return nullptr;
}

std::string leaf(std::string_view p_name) {
	const size_t slash = p_name.rfind('/');
	return std::string(slash == std::string_view::npos ? p_name : p_name.substr(slash + 1));
}

bool env_is_one(const char *p_name) {
	char *v = nullptr;
	size_t n = 0;
	if (_dupenv_s(&v, &n, p_name) != 0 || v == nullptr) {
		return false;
	}
	const bool one = std::string(v) == "1";
	std::free(v);
	return one;
}

double percentile(std::vector<double> p_v, double p_q) {
	if (p_v.empty()) {
		return 0.0;
	}
	std::sort(p_v.begin(), p_v.end());
	const size_t i = size_t(std::clamp(std::ceil(p_q * double(p_v.size())) - 1.0, 0.0, double(p_v.size() - 1)));
	return p_v[i];
}
using Action = InputMapper::M2aAction;

std::string fmt(const char *p_format, ...) {
	char buf[1024];
	va_list args;
	va_start(args, p_format);
	std::vsnprintf(buf, sizeof(buf), p_format, args);
	va_end(args);
	return buf;
}

std::string lower(std::string p_text) {
	for (char &c : p_text) {
		c = char(std::tolower(static_cast<unsigned char>(c)));
	}
	return p_text;
}

std::string short_unlock(const std::string &p_unlock) {
	return p_unlock.substr(p_unlock.rfind('/') + 1);
}

float vdist(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	const float dx = p_a[0] - p_b[0], dy = p_a[1] - p_b[1], dz = p_a[2] - p_b[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

struct M2aCheck::State {
	using StepFn = std::function<bool(State &, double, bool)>; // (state, seconds in the step, first call)
	State(Bf3LevelSession &r_session, M2aGame &r_game) :
			s(r_session), g(r_game) {}
	Bf3LevelSession &s;
	M2aGame &g;
	std::vector<M2aCase> cases;
	M2aCheckOptions options;
	std::vector<M2aCaseResult> results;
	size_t next_case = 0;
	bool running = false;
	M2aCaseResult current;
	struct Step {
		std::string name;
		double timeout = 10.0;
		StepFn fn;
	};
	std::vector<Step> steps;
	size_t step = 0;
	double step_t = 0;
	bool step_first = true;
	double case_t = 0;
	double case_timeout = 180.0;
	uint64_t now_ns = 0;
	double dt = 0;
	bool done = false;
	uint32_t shots_saved = 0;

	// Debug requests.
	uint32_t next_id = 9000;
	uint32_t pending_id = 0;
	bool request_sent = false;
	net::DebugReport report;

	// Per-case scratch.
	std::optional<sw::RangeLane> lane;
	sim::Vec3 point{};
	sim::Vec3 origin{};
	uint32_t life_before = 0;
	size_t mark_predicted = 0, mark_server = 0, mark_timed = 0, mark_ptimed = 0, mark_damage = 0;
	uint64_t mark_hits_shown = 0, mark_kills = 0, mark_cues = 0, mark_own_shots = 0;
	int hits = 0, shots_fired = 0, misses = 0, taps = 0;
	bool killed = false;
	double first_damage = -1;
	double t_mark = 0;
	uint32_t counter = 0;
	std::vector<std::pair<double, float>> fov_samples;
	uint64_t frames_checked = 0, mismatches = 0;
	std::string first_mismatch;
	uint32_t vm_reload_frames = 0, hud_reload_frames = 0, reload_frames = 0;
	size_t kit_index = 0;
	uint8_t slot_cursor = 0;
	uint64_t deaths_mark = 0;
	std::vector<std::string> notes;
	// The play case.
	double play_next_event = 0;
	uint32_t play_deaths_target = 0;
	bool play_engaging = false;
	double play_engage_until = 0;
	std::array<bool, 5> play_shot_taken{};
	uint32_t play_fall_deaths = 0; // scheduled falls
	uint32_t play_fall_drops = 0; // teleports (a fall that lands alive is dropped again from where it landed)
	uint32_t play_fall_retries_left = 0;
	uint8_t play_team_at_start = 0;
	bool play_team_switched = false;
	double play_last_fall = -1000;
	double play_dead_since = -1;
	double play_zoomed_since = -1; // the ADS screenshot waits for 0.4 s of zoom (the viewmodel's blend done)
	sw::SimTime detonation_time = -1;
	// The audio case (E7).
	std::unique_ptr<ba::GameAudioCheck> acheck;
	ba::GameCheckScript ascript;
	ba::AudioTuning atuning;
	int64_t astep = -1;
	double astep_t0 = 0, anext_burst = 0;
	uint32_t abursts_left = 0, aburst_shots = 0;
	bool aburst_firing = false, aaudition_started = false, aorigin_valid = false;
	Vector3 aorigin, adir;
	// The fx case (E8).
	struct FxSurface {
		uint8_t material = 0;
		bool found = false;
		uint32_t spawn = 0;
		sim::Vec3 point{};
		float distance = 0;
	};
	std::array<FxSurface, 2> fx_surfaces{ { { 8 }, { 2 } } }; // concrete, brick
	int32_t fx_flash_component = -1;
	std::string fx_m16_1p;
	uint64_t fx_shots_1p = 0, fx_flash = 0, fx_seg_shots = 0, fx_frames = 0, fx_mismatch_frames = 0;
	uint64_t fx_tracer_flags = 0, fx_own_shot_events = 0, fx_impact_events = 0;
	std::map<uint8_t, std::array<uint64_t, 4>> fx_mat; // PhysicsMaterialIndex -> impacts, effect as named, decals, decal as named
	uint64_t fx_soldier_starts = 0, fx_soldier_ok = 0, fx_soldier_decals = 0;
	std::string fx_first_bad;
	fx::FxTriggerStats fx_trig0{};
	fx::EffectHandle fx_oil{};
	bool fx_oil_started = false;
	double fx_oil_t0 = -1;
	int64_t fx_oil_particles = -1;
	// The firefight case (E9 / E14).
	std::vector<double> ff_frame_on, ff_m2a_on, ff_crowd, ff_fx_main, ff_gpu_on, ff_gpu_off;
	uint32_t ff_max_lights = 0, ff_max_particles = 0, ff_lights_over = 0, ff_min_drawn = 0xFFFFFFFFu;
	uint64_t ff_late0 = 0, ff_short0 = 0, ff_remote0 = 0, ff_hits0 = 0;

	~State() {
		if (acheck != nullptr && g.audio != nullptr) {
			g.audio->set_check(nullptr); // the audio thread must not see the recorder after it goes
		}
		g.listener_override = false;
		g.fx_off = false;
	}

	// ---- helpers ----
	const host::PresentationFrame &f() const { return s.frame(); }
	InputMapper &mapper() { return s.mapper(); }
	host::ClientSession *client() { return s.client(); }
	void row(const std::string &p_name, double p_value, const std::string &p_rule, bool p_pass) {
		M2aRow r;
		r.name = p_name;
		r.value = p_value;
		r.rule = p_rule;
		r.pass = p_pass;
		current.rows.push_back(r);
		UtilityFunctions::print(to_godot(fmt("[m2a] %s %s.%s %.6g (%s)", p_pass ? "PASS" : "FAIL", std::string(m2a_case_name(current.which)).c_str(), p_name.c_str(),
				p_value, p_rule.c_str())));
	}
	void note(const std::string &p_text) {
		UtilityFunctions::print(to_godot("[m2a] " + std::string(m2a_case_name(current.which)) + ": " + p_text));
	}
	bool ready() const {
		return g.alive && g.weapon.valid && g.weapon.phase == sw::WeaponPhase::Ready && g.weapon.pending_slot == 0xFF;
	}
	const sw::WeaponSimDef *def(uint16_t p_def) const {
		return g.gunplay != nullptr && g.gunplay->table != nullptr && p_def < g.gunplay->table->defs.size() ? &g.gunplay->table->defs[p_def] : nullptr;
	}
	const host::KitLoadout &loadout(uint8_t p_team, uint8_t p_kit) const { return g.gunplay->kits.loadout(p_team, p_kit); }
	void send(net::DebugRequest p_request) {
		p_request.id = ++next_id;
		pending_id = p_request.id;
		report = net::DebugReport();
		if (client() != nullptr) {
			client()->send_debug(p_request);
		}
	}
	bool poll_report() {
		net::DebugReport r;
		while (client() != nullptr && client()->take_debug_report(r)) {
			if (r.id == pending_id) {
				report = std::move(r);
				return true;
			}
		}
		return false;
	}
	sim::Vec3 eye() const {
		const sim::SoldierState &o = f().own_curr;
		return { o.position[0] + f().visual_offset[0], o.position[1] + f().eye_height_curr, o.position[2] + f().visual_offset[2] };
	}
	void aim_at(const sim::Vec3 &p_from, const sim::Vec3 &p_to) {
		const double dx = double(p_to[0]) - double(p_from[0]), dy = double(p_to[1]) - double(p_from[1]), dz = double(p_to[2]) - double(p_from[2]);
		mapper().script_view(float(std::atan2(dx, dz)), float(std::atan2(dy, std::sqrt(dx * dx + dz * dz))));
	}
	void mark() {
		mark_predicted = g.predicted_shots.size();
		mark_server = g.server_shots.size();
		mark_timed = g.server_timed.size();
		mark_ptimed = g.predicted_timed.size();
		mark_damage = g.server_damage.size();
		mark_hits_shown = g.hud_state.hits_shown;
		mark_kills = g.kills;
		mark_cues = g.vm_shot_cues;
		mark_own_shots = g.own_shots_seen;
	}
	void release_weapon_input() {
		for (size_t a = 0; a < InputMapper::kM2aActionCount; ++a) {
			mapper().m2a_script_hold(Action(a), false);
		}
	}
	bool save_shot(const std::string &p_name) {
		if (options.shots_dir.empty()) {
			return false;
		}
		const std::u8string dir = options.shots_dir.u8string();
		const std::string path = std::string(dir.begin(), dir.end()) + "/" + p_name + ".png";
		if (s.save_screenshot(path)) {
			++shots_saved;
			UtilityFunctions::print(to_godot("[m2a] screenshot " + path));
			return true;
		}
		return false;
	}

	// ---- step builders ----
	void add(const std::string &p_name, double p_timeout, StepFn p_fn) { steps.push_back({ p_name, p_timeout, std::move(p_fn) }); }
	void wait(double p_seconds) {
		add(fmt("wait %.2f s", p_seconds), p_seconds + 5.0, [p_seconds](State &, double p_t, bool) { return p_t >= p_seconds; });
	}
	void wait_ready(const std::string &p_name, double p_timeout = 8.0) {
		add(p_name, p_timeout, [](State &st, double, bool) { return st.ready(); });
	}
	// A dev request built at the step's start; the step ends with its report (st.report).
	void request(const std::string &p_name, std::function<net::DebugRequest(State &)> p_make) {
		add(p_name, 10.0, [p_make](State &st, double, bool p_first) {
			if (p_first) {
				st.send(p_make(st));
			}
			return st.poll_report();
		});
	}
	void select_slot(uint8_t p_slot, double p_timeout = 6.0) {
		add(fmt("select slot %u", unsigned(p_slot)), p_timeout, [p_slot](State &st, double, bool p_first) {
			if (p_first) {
				st.mapper().m2a_script_select(p_slot);
			}
			return st.ready() && st.g.weapon.active_slot == p_slot;
		});
	}
	// Full magazine in hand (reload when not).
	void full_magazine() {
		add("full magazine", 12.0, [](State &st, double, bool p_first) {
			if (p_first && st.g.weapon.valid && st.g.weapon.magazine < st.g.weapon.magazine_capacity) {
				st.mapper().m2a_script_press(Action::Reload);
			}
			return st.ready() && st.g.weapon.magazine == st.g.weapon.magazine_capacity;
		});
	}
	// Spawn at a point (SpawnAt) and wait for the new life to be adopted, alive and ready.
	void respawn_at(std::function<uint32_t(State &)> p_index) {
		add("SpawnAt", 10.0, [p_index](State &st, double, bool p_first) {
			if (p_first) {
				st.life_before = st.f().own_curr.life;
				net::DebugRequest q;
				q.kind = net::DebugKind::SpawnAt;
				q.index = p_index(st);
				st.send(q);
			}
			st.poll_report();
			return st.g.alive && st.f().own_curr.life != st.life_before && st.mapper().adopted_life() == st.f().own_curr.life;
		});
		wait_ready("ready after spawn", 10.0);
	}
	void teleport_lane() {
		add("teleport to the range lane", 10.0, [](State &st, double, bool p_first) {
			if (p_first) {
				net::DebugRequest q;
				q.kind = net::DebugKind::Teleport;
				if (st.lane) {
					q.position = st.lane->feet;
					q.yaw = st.lane->yaw;
				} else {
					q.position = st.f().own_curr.position;
					q.yaw = sim::quantize_yaw(st.mapper().view().yaw);
				}
				st.mapper().set_requested_pose(sim::Pose::Stand);
				st.mapper().script_view(sim::yaw_radians(q.yaw), 0.0f);
				st.send(q);
			}
			return st.poll_report();
		});
		wait(0.4);
	}
	// The play case's fall: a Teleport 60 m above the soldier (M1's fall damage kills from there onto the ground).
	void play_drop(double p_t) {
		net::DebugRequest q;
		q.kind = net::DebugKind::Teleport;
		q.position = f().own_curr.position;
		q.position[1] += 60.0f;
		q.yaw = sim::quantize_yaw(mapper().view().yaw);
		send(q);
		play_last_fall = p_t;
		++play_fall_drops;
		const sim::Vec3 &from = f().own_curr.position;
		note(fmt("fall %u (drop %u) at %.1f s: from (%.1f, %.1f, %.1f)", play_fall_deaths, play_fall_drops, p_t, double(from[0]), double(from[1]), double(from[2])));
	}
	// Place the target bot p_distance m ahead along the view (on the floor), facing back.
	void place_target(float p_distance) {
		add(fmt("SpawnTarget %.0f m", double(p_distance)), 10.0, [p_distance](State &st, double, bool p_first) {
			if (p_first) {
				const sim::SoldierState &o = st.f().own_curr;
				const float yaw = st.mapper().view().yaw;
				sim::Vec3 at = { o.position[0] + std::sin(yaw) * p_distance, o.position[1], o.position[2] + std::cos(yaw) * p_distance };
				if (st.g.world != nullptr) {
					sim::StaticHit floor;
					if (st.g.world->ray({ at[0], at[1] + 1.5f, at[2] }, { 0.0f, -4.0f, 0.0f }, sim::query_mask::kMovement, &floor)) {
						at[1] = at[1] + 1.5f - 4.0f * floor.fraction;
					}
				}
				net::DebugRequest q;
				q.kind = net::DebugKind::SpawnTarget;
				q.slot = 0;
				q.position = at;
				q.yaw = uint16_t(sim::quantize_yaw(yaw) + 32768u);
				q.pose = sim::Pose::Stand;
				st.send(q);
			}
			return st.poll_report();
		});
		wait(0.3);
		request("TargetInfo", [](State &) {
			net::DebugRequest q;
			q.kind = net::DebugKind::TargetInfo;
			q.slot = 0;
			return q;
		});
	}

	void build_case(M2aCase p_case);
	void build_range();
	void build_reload();
	void build_kits();
	void build_hud();
	void build_ads();
	void build_play();
	void build_audio();
	void build_fx();
	void build_firefight();
	void automatic_mode();
	void fx_account();
	void kill_row(const char *p_name, sw::HitZone p_zone, float p_distance, int p_kill_in, double p_per_hit);
	void finish_case();
};

// ---- the cases --------------------------------------------------------------------------------------------------

void M2aCheck::State::build_case(M2aCase p_case) {
	steps.clear();
	step = 0;
	step_t = 0;
	step_first = true;
	case_t = 0;
	case_timeout = options.case_timeout_s;
	current = M2aCaseResult();
	current.which = p_case;
	current.outcome = M2aOutcome::Pass;
	release_weapon_input();
	mapper().release_all();
	mapper().m2a_script_select(0xFF);
	switch (p_case) {
		case M2aCase::Range:
			build_range();
			break;
		case M2aCase::Reload:
			build_reload();
			break;
		case M2aCase::Kits:
			case_timeout = std::max(case_timeout, 420.0);
			build_kits();
			break;
		case M2aCase::Hud:
			build_hud();
			break;
		case M2aCase::Ads:
			build_ads();
			break;
		case M2aCase::Play:
			case_timeout = options.play_seconds + 120.0;
			build_play();
			break;
		case M2aCase::Audio:
			if (!OPENBF3_M2A_HAVE_AUDIO) {
				current.outcome = M2aOutcome::Skip;
				current.reason = "waiting for au3";
				break;
			}
			case_timeout = std::max(case_timeout, 300.0);
			build_audio();
			break;
		case M2aCase::Fx:
			if (!OPENBF3_M2A_HAVE_FX) {
				current.outcome = M2aOutcome::Skip;
				current.reason = "waiting for fx3";
				break;
			}
			case_timeout = std::max(case_timeout, 420.0);
			build_fx();
			break;
		case M2aCase::Firefight: {
			if (!OPENBF3_M2A_HAVE_BT) {
				current.outcome = M2aOutcome::Skip;
				current.reason = "waiting for bt";
				break;
			}
			// The gate is defined at 2560 x 1440 (m2a.md E9 / E14, S3's rule): another primary screen skips the case.
			DisplayServer *ds = DisplayServer::get_singleton();
			const int32_t primary = ds->get_primary_screen();
			const Vector2i size = ds->screen_get_size(primary);
			if (size.x != kFfWidth || size.y != kFfHeight) {
				current.outcome = M2aOutcome::Skip;
				current.reason = fmt("the primary screen is %dx%d, the gate is defined at %dx%d", size.x, size.y, kFfWidth, kFfHeight);
				break;
			}
			case_timeout = std::max(case_timeout, 240.0);
			build_firefight();
			break;
		}
	}
	if (current.outcome == M2aOutcome::Skip) {
		return;
	}
	// Every functional case starts from a live soldier with a weapon in hand and the viewmodel and HUD built.
	steps.insert(steps.begin(), Step{ "spawned and ready", 20.0, [](State &st, double, bool p_first) {
		if (p_first) {
			st.mapper().set_scripted(true);
		}
		return st.ready() && st.f().stats.gunplay;
	} });
}

// E1 (hold and tap through the mapper, predicted vs server) and E3 (hits to kill, the hit indicator, the counter).
void M2aCheck::State::build_range() {
	add("range lane", 5.0, [](State &st, double, bool) {
		if (st.g.world != nullptr && st.s.shared_level() != nullptr) {
			const float eye = st.f().eye_height_curr;
			st.lane = sw::find_range_lane(*st.g.world, st.s.shared_level()->spawns, eye, 65.0f);
		}
		st.note(st.lane ? fmt("range lane: spawn %u, %.1f m clear, yaw %u", st.lane->spawn_index, double(st.lane->length), unsigned(st.lane->yaw))
						: std::string("no 65 m range lane: the 60 m rows are not run"));
		return true;
	});
	teleport_lane();
	select_slot(0);
	full_magazine();
	automatic_mode();
	// E1: fire held from a full magazine until it is empty.
	add("E1 hold fire", 8.0, [](State &st, double p_t, bool p_first) {
		if (p_first) {
			st.mark();
			st.t_mark = 0;
			st.mapper().m2a_script_hold(Action::Fire, true);
			return false;
		}
		if (st.g.weapon.valid && st.g.weapon.magazine == 0 && st.t_mark == 0) {
			st.t_mark = p_t;
		}
		if (st.t_mark > 0 && p_t > st.t_mark + 0.1) {
			st.mapper().m2a_script_hold(Action::Fire, false);
			return true;
		}
		return false;
	});
	wait(0.8); // the server's last shots and the drained events
	add("E1 rows", 1.0, [](State &st, double, bool) {
		const std::vector<host::PredictedShot> pred(st.g.predicted_shots.begin() + std::ptrdiff_t(st.mark_predicted), st.g.predicted_shots.end());
		const std::vector<M2aServerRecorder::Shot> serv(st.g.server_shots.begin() + std::ptrdiff_t(st.mark_server), st.g.server_shots.end());
		const sw::WeaponSimDef *d = pred.empty() ? nullptr : st.def(pred.front().shot.def);
		const int32_t capacity = d != nullptr ? d->magazine_capacity : 0;
		st.row("e1.predicted_shots", double(pred.size()), fmt("== magazine capacity %d", capacity), capacity > 0 && int32_t(pred.size()) == capacity);
		st.row("e1.server_shots", double(serv.size()), "== predicted", serv.size() == pred.size());
		size_t equal = 0;
		double worst = 0;
		for (const host::PredictedShot &p : pred) {
			for (const M2aServerRecorder::Shot &q : serv) {
				if (q.shot.shot_index != p.shot.shot_index) {
					continue;
				}
				double diff = 0;
				for (int k = 0; k < 3; ++k) {
					diff = std::max(diff, double(std::fabs(p.shot.origin[size_t(k)] - q.shot.origin[size_t(k)])));
					diff = std::max(diff, double(std::fabs(p.shot.direction[size_t(k)] - q.shot.direction[size_t(k)])));
					diff = std::max(diff, double(std::fabs(p.shot.aim[size_t(k)] - q.shot.aim[size_t(k)])));
				}
				worst = std::max(worst, diff);
				if (p.tick == q.tick && p.shot.time == q.shot.time && p.shot.def == q.shot.def && p.shot.weapon_slot == q.shot.weapon_slot && p.shot.seed == q.shot.seed &&
						diff <= 1e-5) {
					++equal;
				}
				break;
			}
		}
		st.row("e1.fields_equal", double(equal), fmt("every predicted shot equals the server's (tick, time, def, slot, seed; vectors within 1e-5, worst %.2g)", worst),
				!pred.empty() && equal == pred.size());
		size_t gaps_ok = 0;
		const sw::SimTime interval = d != nullptr ? d->modes[0].interval : 0;
		sw::SimTime min_gap = 0, max_gap = 0;
		for (size_t i = 1; i < pred.size(); ++i) {
			const sw::SimTime gap = pred[i].shot.time - pred[i - 1].shot.time;
			min_gap = i == 1 ? gap : std::min(min_gap, gap);
			max_gap = i == 1 ? gap : std::max(max_gap, gap);
			gaps_ok += gap == interval ? 1 : 0;
		}
		st.row("e1.gaps", double(max_gap), fmt("every gap == the automatic interval %" PRId64 " units (min %" PRId64 ")", int64_t(interval), int64_t(min_gap)),
				pred.size() > 1 && gaps_ok + 1 == pred.size());
		const bool first_at_start = !pred.empty() && pred.front().shot.time == sw::tick_start(pred.front().tick);
		st.row("e1.first_shot_at_tick_start", first_at_start ? 1.0 : 0.0, "the first shot of a trigger pull at its tick's start", first_at_start);
		st.row("e1.own_shot_events", double(st.g.own_shots_seen - st.mark_own_shots), "one OwnShot event per predicted shot",
				st.g.own_shots_seen - st.mark_own_shots == pred.size());
		st.row("e1.viewmodel_shot_cues", double(st.g.vm_shot_cues - st.mark_cues), "one viewmodel Shot cue per predicted shot",
				st.g.vm_shot_cues - st.mark_cues == pred.size());
		st.row("e1.hud_ammo", double(st.g.hud_model.weapon.active_weapon_ammo), "the HUD's ammo == the presented magazine after the burst",
				st.g.hud_model.weapon.active_weapon_ammo == st.g.weapon.magazine);
		return true;
	});
	// M9: one shot per tap (press counter), one shot for a held trigger.
	select_slot(1);
	full_magazine();
	add("M9 taps", 6.0, [](State &st, double p_t, bool p_first) {
		if (p_first) {
			st.mark();
			st.taps = 0;
			st.t_mark = 0;
		}
		if (st.taps < 5 && p_t >= st.t_mark) {
			st.mapper().m2a_script_press(Action::Fire);
			++st.taps;
			st.t_mark = p_t + 0.45;
		}
		return st.taps >= 5 && p_t >= st.t_mark + 0.3;
	});
	add("M9 tap rows", 1.0, [](State &st, double, bool) {
		const size_t pred = st.g.predicted_shots.size() - st.mark_predicted;
		const size_t serv = st.g.server_shots.size() - st.mark_server;
		st.row("e1.m9_taps", double(pred), "5 taps fire 5 shots (predicted)", pred == 5);
		st.row("e1.m9_taps_server", double(serv), "== predicted", serv == pred);
		st.mark();
		return true;
	});
	add("M9 held", 3.0, [](State &st, double p_t, bool p_first) {
		if (p_first) {
			st.mark();
			st.mapper().m2a_script_hold(Action::Fire, true);
		}
		if (p_t >= 0.6) {
			st.mapper().m2a_script_hold(Action::Fire, false);
		}
		return p_t >= 1.2;
	});
	add("M9 held row", 1.0, [](State &st, double, bool) {
		const size_t pred = st.g.predicted_shots.size() - st.mark_predicted;
		st.row("e1.m9_held", double(pred), "a held semi trigger fires once", pred == 1);
		return true;
	});
	// E3 through the game: chest and head at 5 m, chest at 60 m when the lane is long enough (report).
	select_slot(0);
	kill_row("e3.chest_5m", sw::HitZone::Chest, 5.0f, 4, 25.0);
	kill_row("e3.head_5m", sw::HitZone::Head, 5.0f, 2, 50.0);
	add("60 m lane", 1.0, [](State &st, double, bool) {
		if (!st.lane || st.lane->length < 62.0f) {
			st.note("60 m rows skipped (no lane)");
		}
		return true;
	});
	kill_row("e3.chest_60m", sw::HitZone::Chest, 60.0f, -1, -1.0);
	add("range shot", 2.0, [](State &st, double p_t, bool) {
		if (p_t < 0.3) {
			return false;
		}
		st.save_shot("gx_range");
		return true;
	});
}

// One E3 row: place the target, aim at the zone (with the bullet drop), tap single shots until it dies.
void M2aCheck::State::kill_row(const char *p_name, sw::HitZone p_zone, float p_distance, int p_kill_in, double p_per_hit) {
	const std::string name = p_name;
	const bool far = p_distance > 30.0f;
	add(name + " setup", 2.0, [far](State &st, double, bool) {
		st.counter = far && (!st.lane || st.lane->length < 62.0f) ? 1u : 0u; // 1: skip this row
		return true;
	});
	teleport_lane();
	full_magazine();
	place_target(p_distance);
	add(name + " aim", 3.0, [p_zone](State &st, double, bool) {
		if (st.counter == 1) {
			return true;
		}
		if (st.report.targets.empty() || !sw::zone_aim_point(st.report.targets[0].target, p_zone, st.point)) {
			st.counter = 2; // no target info
			return true;
		}
		st.origin = st.eye();
		st.mark();
		st.hits = st.shots_fired = st.misses = 0;
		st.killed = false;
		st.first_damage = -1;
		st.t_mark = 0;
		return true;
	});
	add(name + " fire", 20.0, [](State &st, double p_t, bool) {
		if (st.counter != 0) {
			return true;
		}
		// Count the server's applied hits on the target since the mark.
		int hits = 0;
		bool killed = false;
		double first = -1;
		for (size_t i = st.mark_damage; i < st.g.server_damage.size(); ++i) {
			const M2aServerRecorder::Damage &d = st.g.server_damage[i];
			if (d.event.attacker_slot == st.g.own_slot && d.event.target == 0 && d.applied) {
				++hits;
				first = first < 0 ? double(d.event.amount) : first;
				killed = killed || d.health_after <= 0.0f;
			}
		}
		st.hits = hits;
		st.killed = killed;
		st.first_damage = first;
		if (killed || st.shots_fired >= 12) {
			return p_t >= st.t_mark + 0.6;
		}
		if (p_t >= st.t_mark && st.ready()) {
			const sw::WeaponSimDef *d = st.def(st.g.weapon.def);
			const double speed = d != nullptr ? std::max(double(d->projectile.muzzle_speed), 1.0) : 400.0;
			const double flight = vdist(st.point, st.origin) / speed;
			const double drop = d != nullptr ? 0.5 * std::fabs(double(d->projectile.gravity)) * flight * flight : 0.0;
			st.aim_at(st.eye(), { st.point[0], float(double(st.point[1]) + drop), st.point[2] });
			if (p_t >= st.t_mark + 0.1) {
				st.mapper().m2a_script_press(Action::Fire);
				++st.shots_fired;
				st.t_mark = p_t + 0.4;
			}
		}
		return false;
	});
	add(name + " rows", 1.0, [name, p_kill_in, p_per_hit](State &st, double, bool) {
		if (st.counter == 1) {
			return true;
		}
		if (st.counter == 2) {
			st.row(name, 0, "TargetInfo answered with the target's capsules", false);
			return true;
		}
		const bool count_ok = p_kill_in < 0 ? st.killed : (st.killed && st.hits == p_kill_in);
		const bool damage_ok = p_per_hit < 0 || std::fabs(st.first_damage - p_per_hit) <= 1e-3;
		st.row(name + ".hits_to_kill", double(st.hits),
				p_kill_in < 0 ? fmt("killed within 12 shots (%d fired; reported)", st.shots_fired) : fmt("== %d, killed (%d fired)", p_kill_in, st.shots_fired),
				count_ok);
		if (p_per_hit >= 0) {
			st.row(name + ".damage", st.first_damage, fmt("%.3f +- 1e-3 per hit", p_per_hit), damage_ok);
		}
		const uint64_t shown = st.g.hud_state.hits_shown - st.mark_hits_shown;
		st.row(name + ".hit_indicator", double(shown), "the HUD's hit indicator rose once per server hit", shown == uint64_t(st.hits));
		const uint64_t kills = st.g.kills - st.mark_kills;
		st.row(name + ".kill_counter", double(kills), "the kill counter (Death with kOwnShooter) counted the kill", kills == (st.killed ? 1u : 0u));
		return true;
	});
}

// E5: tactical and empty reloads timed against the def, a switch cancelling one, the viewmodel clip and the HUD flag.
void M2aCheck::State::build_reload() {
	select_slot(0);
	full_magazine();
	for (const bool empty : { false, true }) {
		const std::string tag = empty ? "empty" : "tactical";
		add("reload " + tag + " fire", 8.0, [empty](State &st, double p_t, bool p_first) {
			if (p_first) {
				st.taps = 0;
				st.t_mark = 0;
				if (empty) {
					st.mapper().m2a_script_hold(Action::Fire, true);
				}
			}
			if (empty) {
				if (st.g.weapon.valid && st.g.weapon.magazine == 0) {
					st.mapper().m2a_script_hold(Action::Fire, false);
					return true;
				}
				return false;
			}
			if (st.taps < 5 && p_t >= st.t_mark && st.ready()) {
				st.mapper().m2a_script_press(Action::Fire);
				++st.taps;
				st.t_mark = p_t + 0.15;
			}
			return st.taps >= 5 && p_t > st.t_mark + 0.3 && st.ready();
		});
		add("reload " + tag, 10.0, [](State &st, double p_t, bool p_first) {
			if (p_first) {
				st.mark();
				st.reload_frames = st.vm_reload_frames = st.hud_reload_frames = 0;
				st.mapper().m2a_script_press(Action::Reload);
			}
			if (st.g.weapon.valid && st.g.weapon.phase == sw::WeaponPhase::Reloading) {
				++st.reload_frames;
				const Bf3Viewmodel *vm = st.g.own_viewmodel();
				if (vm != nullptr) {
					const uint32_t clip = vm->output().action_clip;
					st.vm_reload_frames += clip == uint32_t(bf3::anim::VmClip::Reload) || clip == uint32_t(bf3::anim::VmClip::ReloadEmpty) ? 1 : 0;
				}
				st.hud_reload_frames += st.g.hud_model.weapon.active_weapon_is_reloading ? 1 : 0;
			}
			return p_t > 0.5 && st.reload_frames > 0 && st.ready();
		});
		wait(0.5);
		add("reload " + tag + " rows", 1.0, [empty, tag](State &st, double, bool) {
			const sw::WeaponSimDef *d = st.def(st.g.weapon.def);
			auto find = [&](const auto &p_list, size_t p_from, uint32_t p_event) -> sw::SimTime {
				for (size_t i = p_from; i < p_list.size(); ++i) {
					if (p_list[i].event.event == p_event && p_list[i].event.weapon_slot == 0) {
						return p_list[i].event.time;
					}
				}
				return -1;
			};
			const sw::SimTime s0 = find(st.g.server_timed, st.mark_timed, sw::weapon_events::kReloadStarted);
			const sw::SimTime s1 = find(st.g.server_timed, st.mark_timed, sw::weapon_events::kReloadCommitted);
			const sw::SimTime s2 = find(st.g.server_timed, st.mark_timed, sw::weapon_events::kReloadFinished);
			const sw::SimTime p0 = find(st.g.predicted_timed, st.mark_ptimed, sw::weapon_events::kReloadStarted);
			const sw::SimTime p2 = find(st.g.predicted_timed, st.mark_ptimed, sw::weapon_events::kReloadFinished);
			const sw::SimTime want = d == nullptr ? 0 : (empty ? d->reload_empty : d->reload_tactical);
			const sw::SimTime want_commit = d == nullptr ? 0 : (empty ? d->reload_commit_empty : d->reload_commit_tactical);
			const bool timed = s0 >= 0 && s2 >= 0;
			st.row("e5." + tag + ".duration_s", timed ? sw::units_to_seconds(s2 - s0) : -1.0, fmt("== the def's %.4f s (server)", sw::units_to_seconds(want)),
					timed && s2 - s0 == want);
			st.row("e5." + tag + ".commit_s", s1 >= 0 && s0 >= 0 ? sw::units_to_seconds(s1 - s0) : -1.0,
					fmt("== the def's commit %.4f s (ReloadThreshold)", sw::units_to_seconds(want_commit)), s0 >= 0 && s1 >= 0 && s1 - s0 == want_commit);
			st.row("e5." + tag + ".predicted_equal", p0 >= 0 && p2 >= 0 ? sw::units_to_seconds(p2 - p0) : -1.0, "the first prediction's start and end == the server's",
					p0 == s0 && p2 == s2 && s0 >= 0);
			const double vm_share = st.reload_frames > 0 ? double(st.vm_reload_frames) / double(st.reload_frames) : 0.0;
			st.row("e5." + tag + ".viewmodel_clip", vm_share, fmt(">= 0.9 of the %u reloading frames draw a reload clip", st.reload_frames), vm_share >= 0.9);
			const double hud_share = st.reload_frames > 0 ? double(st.hud_reload_frames) / double(st.reload_frames) : 0.0;
			st.row("e5." + tag + ".hud_is_reloading", hud_share, "every reloading frame's HUD model says reloading", st.reload_frames > 0 && hud_share >= 0.999);
			return true;
		});
	}
	// A switch before the commit cancels the reload (rlWeaponSwitchCancelsUnfinishedReload).
	add("cancel: fire 3", 4.0, [](State &st, double p_t, bool p_first) {
		if (p_first) {
			st.taps = 0;
			st.t_mark = 0;
		}
		if (st.taps < 3 && p_t >= st.t_mark && st.ready()) {
			st.mapper().m2a_script_press(Action::Fire);
			++st.taps;
			st.t_mark = p_t + 0.15;
		}
		return st.taps >= 3 && p_t > st.t_mark + 0.3 && st.ready();
	});
	add("cancel: reload then switch", 6.0, [](State &st, double p_t, bool p_first) {
		if (p_first) {
			st.mark();
			st.counter = uint32_t(st.g.weapon.magazine);
			st.mapper().m2a_script_press(Action::Reload);
			st.t_mark = 0;
		}
		if (st.t_mark == 0 && st.g.weapon.phase == sw::WeaponPhase::Reloading) {
			st.t_mark = p_t;
		}
		if (st.t_mark > 0 && p_t >= st.t_mark + 0.25) {
			st.mapper().m2a_script_select(1);
			return true;
		}
		return false;
	});
	wait_ready("cancel: sidearm ready", 6.0);
	select_slot(0);
	add("cancel rows", 1.0, [](State &st, double, bool) {
		bool cancelled = false, finished = false;
		for (size_t i = st.mark_timed; i < st.g.server_timed.size(); ++i) {
			cancelled = cancelled || st.g.server_timed[i].event.event == sw::weapon_events::kReloadCancelled;
			finished = finished || (st.g.server_timed[i].event.event == sw::weapon_events::kReloadFinished && st.g.server_timed[i].event.weapon_slot == 0);
		}
		st.row("e5.switch_cancels", cancelled && !finished ? 1.0 : 0.0, "a switch 0.25 s into the reload cancels it (server)", cancelled && !finished);
		st.row("e5.magazine_kept", double(st.g.weapon.magazine), fmt("== %u (the rounds before the cancelled reload)", st.counter), uint32_t(st.g.weapon.magazine) == st.counter);
		return true;
	});
}

// E11: the 8 loadouts through KitSelect + SpawnAt; every slot deploys and shows its own viewmodel item.
void M2aCheck::State::build_kits() {
	add("kits: range lane", 5.0, [](State &st, double, bool) {
		if (st.g.world != nullptr && st.s.shared_level() != nullptr) {
			st.lane = sw::find_range_lane(*st.g.world, st.s.shared_level()->spawns, st.f().eye_height_curr, 20.0f);
		}
		return true;
	});
	for (uint8_t team = 1; team <= 2; ++team) {
		for (uint8_t kit = 0; kit < host::kKitCount; ++kit) {
			const std::string tag = fmt("e11.%s_%s", team == 1 ? "us" : "ru", host::kit_role_name(host::KitRole(kit)));
			add(tag + " select", 2.0, [team, kit](State &st, double p_t, bool p_first) {
				if (p_first && st.client() != nullptr) {
					st.client()->send_kit_select(net::KitSelect{ kit, team });
				}
				return p_t >= 0.3;
			});
			respawn_at([](State &st) { return st.lane ? st.lane->spawn_index : 0u; });
			add(tag + " loadout", 1.0, [team, kit, tag](State &st, double, bool) {
				const host::KitLoadout &l = st.loadout(team, kit);
				bool defs_ok = st.f().own_weapons_valid;
				for (const uint8_t s : host::kKitSlots) {
					defs_ok = defs_ok && st.f().own_weapons_curr.slots[s].def == l.defs[s];
				}
				st.row(tag + ".spawned_as", double(st.f().own_kit) + 10.0 * double(st.f().own_team), fmt("kit %u team %u (value kit + 10 x team)", kit, team),
						st.f().own_kit == kit && st.f().own_team == team);
				st.row(tag + ".slot_defs", defs_ok ? 1.0 : 0.0, "slots 0, 1, 6, 7 hold the kit table's defs", defs_ok);
				st.slot_cursor = 0;
				st.notes.clear();
				return true;
			});
			for (const uint8_t slot : { uint8_t(1), uint8_t(6), uint8_t(7), uint8_t(0) }) {
				add(fmt("%s deploy %u", tag.c_str(), unsigned(slot)), 6.0, [slot](State &st, double, bool p_first) {
					if (p_first) {
						const Action key = slot == 1 ? Action::Sidearm : (slot == 6 ? Action::Grenade : (slot == 7 ? Action::Knife : Action::Primary));
						st.mapper().m2a_script_press(key); // the 1 / 2 / G / F keys' path
					}
					return st.ready() && st.g.weapon.active_slot == slot;
				});
				add(fmt("%s item %u", tag.c_str(), unsigned(slot)), 1.0, [slot, team, kit, tag](State &st, double p_t, bool) {
					if (p_t < 0.1) {
						return false; // one presented frame with the item in hand
					}
					const host::KitLoadout &l = st.loadout(team, kit);
					const Bf3Viewmodel *vm = st.g.own_viewmodel();
					const bf3::render::ViewmodelItem *item = vm != nullptr ? vm->current_item() : nullptr;
					const bool ok = item != nullptr && lower(item->unlock) == lower(l.unlocks[slot]) && st.g.vm_item == st.g.item_of(team, l.defs[slot]);
					st.row(fmt("%s.item_%u", tag.c_str(), unsigned(slot)), double(st.g.vm_item),
							fmt("the viewmodel holds %s (PresentedWeapon::def -> kit unlock)", short_unlock(l.unlocks[slot]).c_str()), ok);
					return true;
				});
			}
			add(tag + " fire and reload", 8.0, [](State &st, double p_t, bool p_first) {
				if (p_first) {
					st.mark();
					st.mapper().m2a_script_press(Action::Fire);
					st.t_mark = 0;
				}
				if (st.t_mark == 0 && p_t > 0.5 && st.ready()) {
					st.mapper().m2a_script_press(Action::Reload);
					st.t_mark = p_t;
				}
				return st.t_mark > 0 && p_t > st.t_mark + 0.5 && st.ready();
			});
			add(tag + " fire rows", 1.0, [tag](State &st, double, bool) {
				const size_t serv = st.g.server_shots.size() - st.mark_server;
				// The magazine is refilled at the commit (ReloadThreshold); the soldier is ready again from there.
				bool reload_started = false, reload_finished = false;
				for (size_t i = st.mark_timed; i < st.g.server_timed.size(); ++i) {
					reload_started = reload_started || st.g.server_timed[i].event.event == sw::weapon_events::kReloadStarted;
					reload_finished = reload_finished || st.g.server_timed[i].event.event == sw::weapon_events::kReloadCommitted ||
							st.g.server_timed[i].event.event == sw::weapon_events::kReloadFinished;
				}
				reload_finished = reload_finished && st.g.weapon.magazine == st.g.weapon.magazine_capacity;
				st.row(tag + ".fires", double(serv), "the primary fired one shot (server)", serv == 1);
				std::string seen;
				for (size_t i = st.mark_timed; i < st.g.server_timed.size() && seen.size() < 200; ++i) {
					seen += fmt(" %x@%u", st.g.server_timed[i].event.event, unsigned(st.g.server_timed[i].event.weapon_slot));
				}
				st.row(tag + ".reloads", reload_finished ? 1.0 : 0.0,
						fmt("the primary reloaded (server ReloadStarted .. Committed, the magazine full: %d); timed events:%s", st.g.weapon.magazine, seen.c_str()),
						reload_started && reload_finished);
				return true;
			});
			if (team == 1 && kit == 0) {
				// The knife on the target bot 1.2 m ahead, then the M67's fuse along the lane.
				teleport_lane();
				place_target(1.3f);
				select_slot(7);
				add("knife", 4.0, [](State &st, double p_t, bool p_first) {
					if (p_first) {
						st.mark();
						if (!st.report.targets.empty()) {
							sw::zone_aim_point(st.report.targets[0].target, sw::HitZone::Chest, st.point);
							st.aim_at(st.eye(), st.point);
						}
					}
					if (p_t >= 0.2 && p_t < 0.25) {
						st.mapper().m2a_script_press(Action::Fire);
					}
					return p_t >= 1.5;
				});
				add("knife row", 1.0, [](State &st, double, bool) {
					uint32_t hits = 0;
					for (size_t i = st.mark_damage; i < st.g.server_damage.size(); ++i) {
						const M2aServerRecorder::Damage &d = st.g.server_damage[i];
						hits += d.event.attacker_slot == st.g.own_slot && d.event.target == 0 && d.applied ? 1 : 0;
					}
					st.row("e11.knife_hit", double(hits), "the knife hit the target bot (server damage applied)", hits >= 1);
					return true;
				});
				teleport_lane();
				select_slot(6);
				add("M67 throw", 8.0, [](State &st, double p_t, bool p_first) {
					if (p_first) {
						st.mark();
						st.detonation_time = -1;
						st.mapper().script_view(st.mapper().view().yaw, float(25.0 * kPi / 180.0));
						st.mapper().m2a_script_hold(Action::Fire, true);
					}
					if (p_t >= 0.5) {
						st.mapper().m2a_script_hold(Action::Fire, false);
					}
					for (const host::PresentEvent &e : st.g.events) {
						if (e.kind == host::PresentKind::Detonation && e.slot == st.g.own_slot && st.detonation_time < 0) {
							st.detonation_time = e.time;
						}
					}
					return p_t >= 5.5;
				});
				add("M67 row", 1.0, [](State &st, double, bool) {
					sw::SimTime thrown = -1, spawned = -1;
					for (size_t i = st.mark_server; i < st.g.server_shots.size(); ++i) {
						if (st.g.server_shots[i].shot.weapon_slot == 6) {
							thrown = st.g.server_shots[i].shot.time;
							spawned = st.g.server_shots[i].shot.spawn_time;
							break;
						}
					}
					st.row("e11.m67_thrown", thrown >= 0 ? 1.0 : 0.0, "the M67 left the hand (server shot on slot 6)", thrown >= 0);
					const double fuse = thrown >= 0 && st.detonation_time >= 0 ? sw::units_to_seconds(st.detonation_time - spawned) : -1.0;
					st.row("e11.m67_fuse_s", fuse, fmt("Detonation event 3.6 +- 0.1 s after the projectile spawned (thrown %.3f s before it)",
															thrown >= 0 ? sw::units_to_seconds(spawned - thrown) : 0.0),
							fuse >= 3.5 && fuse <= 3.7);
					return true;
				});
				select_slot(0);
			}
		}
	}
	// Back to the run's own kit and team.
	add("kits: restore", 2.0, [](State &st, double p_t, bool p_first) {
		if (p_first && st.client() != nullptr) {
			st.client()->send_kit_select(net::KitSelect{ st.g.kit, st.g.team });
		}
		return p_t >= 0.3;
	});
	respawn_at([](State &st) { return st.lane ? st.lane->spawn_index : 0u; });
}

// E10: per frame, the HUD model against the presented weapon; names and icons of every kit item.
void M2aCheck::State::build_hud() {
	select_slot(0);
	add("hud: names and icons", 1.0, [](State &st, double, bool) {
		Bf3Hud *hud = st.g.hud;
		uint32_t items = 0, names = 0, icons = 0;
		std::string missing;
		std::vector<uint16_t> seen;
		for (uint8_t team = 1; team <= 2; ++team) {
			for (uint8_t kit = 0; kit < host::kKitCount; ++kit) {
				const host::KitLoadout &l = st.loadout(team, kit);
				for (const uint8_t s : host::kKitSlots) {
					const uint16_t def = l.defs[s];
					if (def == sw::kNoWeapon || std::find(seen.begin(), seen.end(), def) != seen.end()) {
						continue;
					}
					seen.push_back(def);
					++items;
					const host::HudStaticRow *row = st.g.hud_static.row(def);
					const bool named = hud != nullptr && row != nullptr && !hud->localised(row->name_sid_hash).empty();
					const bool icon = hud != nullptr && row != nullptr && hud->icon(row->item_id).is_valid();
					names += named ? 1 : 0;
					icons += icon ? 1 : 0;
					if (!named || !icon) {
						missing += " " + short_unlock(l.unlocks[s]) + (named ? "" : "(name)") + (icon ? "" : "(icon)");
					}
				}
			}
		}
		st.row("e10.names", double(names), fmt("every kit item's localised name, %u / %u%s", names, items, missing.c_str()), items == 12 && names == items);
		st.row("e10.icons", double(icons), fmt("every kit item's _small icon, %u / %u", icons, items), items == 12 && icons == items);
		st.frames_checked = st.mismatches = 0;
		st.first_mismatch.clear();
		return true;
	});
	// A scripted 6 s: taps, a reload, ADS, a sidearm switch; every frame compared.
	add("hud: frames", 10.0, [](State &st, double p_t, bool) {
		const host::PresentedWeapon &w = st.g.weapon;
		const bf3::core::ui::HudWeapon &m = st.g.hud_model.weapon;
		if (w.valid && st.g.alive) {
			++st.frames_checked;
			std::string bad;
			if (m.active_weapon_ammo != w.magazine) {
				bad = fmt("ammo %d vs magazine %d", m.active_weapon_ammo, w.magazine);
			} else if (m.active_weapon_ammo_in_bag != w.reserve) {
				bad = fmt("in bag %d vs reserve %d", m.active_weapon_ammo_in_bag, w.reserve);
			} else if (m.active_weapon_is_reloading != (w.phase == sw::WeaponPhase::Reloading || w.phase == sw::WeaponPhase::ReloadShell)) {
				bad = "is_reloading";
			} else if (m.zoomed != w.zoomed) {
				bad = "zoomed";
			} else if (m.show_crosshair && w.zoomed) {
				bad = "crosshair while zoomed";
			} else if (m.active_weapon_index != int32_t(w.active_slot)) {
				bad = fmt("index %d vs slot %u", m.active_weapon_index, unsigned(w.active_slot));
			}
			if (!bad.empty()) {
				++st.mismatches;
				if (st.first_mismatch.empty()) {
					st.first_mismatch = fmt("%.2f s: ", p_t) + bad;
				}
			}
		}
		InputMapper &mp = st.mapper();
		const int phase = int(p_t * 4.0); // quarter seconds
		if (phase < 8 && phase % 2 == 0 && std::fmod(p_t, 0.5) < st.dt + 1e-6) {
			mp.m2a_script_press(Action::Fire);
		} else if (phase == 9 && std::fmod(p_t, 0.25) < st.dt + 1e-6) {
			mp.m2a_script_press(Action::Reload);
		}
		mp.m2a_script_hold(Action::Ads, p_t >= 4.0 && p_t < 5.0);
		if (p_t >= 5.2 && p_t < 5.2 + st.dt + 1e-6) {
			mp.m2a_script_select(1);
		}
		return p_t >= 7.0;
	});
	add("hud rows", 1.0, [](State &st, double, bool) {
		st.row("e10.frames", double(st.frames_checked), ">= 240 frames with a weapon compared", st.frames_checked >= 240);
		st.row("e10.model_mismatches", double(st.mismatches), "the HUD model equals the presented weapon every frame" + (st.first_mismatch.empty() ? std::string() : " (first: " + st.first_mismatch + ")"),
				st.mismatches == 0);
		const bool loaded = st.g.hud != nullptr && st.g.hud->is_loaded() && st.g.hud->has_layout();
		st.row("e10.hud_loaded", loaded ? 1.0 : 0.0, "Bf3Hud loaded its hudmpscreen layout", loaded);
		return true;
	});
	select_slot(0);
}

// E19: the ADS camera through the game (world FOV in and out, the look scale, refused while sprinting).
void M2aCheck::State::build_ads() {
	add("ads: lane", 5.0, [](State &st, double, bool) {
		if (st.g.world != nullptr && st.s.shared_level() != nullptr) {
			st.lane = sw::find_range_lane(*st.g.world, st.s.shared_level()->spawns, st.f().eye_height_curr, 30.0f);
		}
		return true;
	});
	teleport_lane();
	select_slot(0);
	add("ads in", 3.0, [](State &st, double p_t, bool p_first) {
		Camera3D *cam = st.s.cameras().first_person();
		if (p_first) {
			st.fov_samples.clear();
			st.mapper().m2a_script_hold(Action::Ads, true);
		}
		st.fov_samples.push_back({ p_t, cam != nullptr ? float(cam->get_fov()) : 0.0f });
		return p_t >= 1.0;
	});
	add("ads in rows", 1.0, [](State &st, double, bool) {
		const Bf3Viewmodel *vm = st.g.own_viewmodel();
		const bf3::anim::AdsParams p = vm != nullptr ? vm->ads_params() : bf3::anim::AdsParams();
		double reached = -1;
		for (const auto &[t, fov] : st.fov_samples) {
			if (reached < 0 && std::fabs(double(fov) - double(p.zoom_fov)) <= 0.05) {
				reached = t;
			}
		}
		const double end = st.fov_samples.empty() ? 0.0 : double(st.fov_samples.back().second);
		// From the press: the command of the next tick, its first prediction, the frame that presents it, then the FOV time.
		const double limit = double(p.fov_in_seconds) + 0.1;
		st.row("e19.fov_aimed", end, fmt("== the zoom FOV %.1f +- 0.05", double(p.zoom_fov)), std::fabs(end - double(p.zoom_fov)) <= 0.05);
		st.row("e19.fov_in_s", reached, fmt("reached within the FOV time %.3f s + 0.1 s of input and presentation latency", double(p.fov_in_seconds)),
				reached >= 0 && reached <= limit);
		const float scale = vm != nullptr ? vm->look_scale() : 0.0f;
		st.row("e19.look_scale", double(scale), fmt("== LookSpeedMultiplier aimed %.3f", double(p.look_speed_zoom)), std::fabs(scale - p.look_speed_zoom) <= 1e-4f);
		const double sens = double(st.mapper().mouse_sensitivity) / std::max(1e-9, double(st.g.base_sensitivity));
		st.row("e19.mouse_scaled", sens, "the mapper's mouse sensitivity follows the look scale", std::fabs(sens - double(scale)) <= 1e-4);
		return true;
	});
	add("ads out", 3.0, [](State &st, double p_t, bool p_first) {
		Camera3D *cam = st.s.cameras().first_person();
		if (p_first) {
			st.fov_samples.clear();
			st.mapper().m2a_script_hold(Action::Ads, false);
		}
		st.fov_samples.push_back({ p_t, cam != nullptr ? float(cam->get_fov()) : 0.0f });
		return p_t >= 1.0;
	});
	add("ads out rows", 1.0, [](State &st, double, bool) {
		const Bf3Viewmodel *vm = st.g.own_viewmodel();
		const bf3::anim::AdsParams p = vm != nullptr ? vm->ads_params() : bf3::anim::AdsParams();
		double reached = -1;
		for (const auto &[t, fov] : st.fov_samples) {
			if (reached < 0 && std::fabs(double(fov) - double(p.hip_fov)) <= 0.05) {
				reached = t;
			}
		}
		const double end = st.fov_samples.empty() ? 0.0 : double(st.fov_samples.back().second);
		st.row("e19.fov_hip", end, fmt("== the hip FOV %.1f +- 0.05", double(p.hip_fov)), std::fabs(end - double(p.hip_fov)) <= 0.05);
		st.row("e19.fov_out_s", reached, fmt("reached within the FOV time %.3f s + 0.1 s", double(p.fov_out_seconds)), reached >= 0 && reached <= double(p.fov_out_seconds) + 0.1);
		return true;
	});
	// Sprint, then ask for ADS: refused (the predicted zoom stays off, the FOV stays at the hip value).
	add("ads while sprinting", 4.0, [](State &st, double p_t, bool p_first) {
		Camera3D *cam = st.s.cameras().first_person();
		if (p_first) {
			st.fov_samples.clear();
			st.counter = 0;
			st.mapper().script_hold(InputAction::Forward, true);
			st.mapper().script_hold(InputAction::Sprint, true);
		}
		if (p_t >= 1.0) {
			st.mapper().m2a_script_hold(Action::Ads, true);
			st.fov_samples.push_back({ p_t, cam != nullptr ? float(cam->get_fov()) : 0.0f });
			const bool sprinting = (st.f().own_curr.flags & sim::soldier_flags::kSprinting) != 0;
			st.counter += sprinting ? 1u : 0u;
		}
		if (p_t >= 1.8) {
			st.mapper().m2a_script_hold(Action::Ads, false);
			st.mapper().script_hold(InputAction::Forward, false);
			st.mapper().script_hold(InputAction::Sprint, false);
			return true;
		}
		return false;
	});
	add("ads sprint rows", 1.0, [](State &st, double, bool) {
		const Bf3Viewmodel *vm = st.g.own_viewmodel();
		const bf3::anim::AdsParams p = vm != nullptr ? vm->ads_params() : bf3::anim::AdsParams();
		float lowest = 1000.0f;
		for (const auto &[t, fov] : st.fov_samples) {
			(void)t;
			lowest = std::min(lowest, fov);
		}
		st.row("e19.sprint_frames", double(st.counter), "the soldier sprinted while ADS was asked (frames)", st.counter > 10);
		st.row("e19.ads_refused_sprinting", double(lowest), fmt("the FOV stayed at the hip %.1f while sprinting", double(p.hip_fov)),
				st.counter > 10 && std::fabs(double(lowest) - double(p.hip_fov)) <= 0.05);
		return true;
	});
	wait_ready("ads: ready", 6.0);
}

// The scripted play run (docs/design/m2a-gx.md §4.7): move, look, fire at the nearest other, reload, switch, throw,
// knife, fall to death and respawn, K / T for the next spawn; screenshots of hip, ADS, reload, a firefight and the
// death screen.
void M2aCheck::State::build_play() {
	add("play", options.play_seconds + 60.0, [](State &st, double p_t, bool p_first) {
		InputMapper &mp = st.mapper();
		if (p_first) {
			st.play_team_at_start = st.g.own_team;
			st.deaths_mark = st.g.deaths;
			st.play_shot_taken.fill(false);
			st.play_fall_deaths = 0;
			st.play_last_fall = -1000;
			st.play_fall_drops = 0;
			st.play_fall_retries_left = 0;
			st.play_dead_since = -1;
			st.mark();
		}
		const double T = st.options.play_seconds;
		if (p_t >= T) {
			st.release_weapon_input();
			mp.release_all();
			return true;
		}
		const double seg = std::fmod(p_t, 30.0);
		const int slice = int(p_t / 5.0) % 6;
		// Dead: wait for the respawn; the death screen shot 1.5 s in.
		if (!st.g.alive) {
			st.release_weapon_input();
			mp.release_all();
			st.play_fall_retries_left = 0;
			if (st.play_dead_since < 0) {
				st.play_dead_since = p_t;
			}
			if (!st.play_shot_taken[4] && st.g.dead && p_t - st.play_dead_since >= 1.5) {
				st.play_shot_taken[4] = st.save_shot("gx_death");
			}
			return false;
		}
		st.play_dead_since = -1;
		// Falling: no input for 8 s after the teleport (the soldier drops straight down; no shots, no moves). A drop
		// that lands alive (on a roof on the way down: Metro's park is lined with ~30 m buildings) drops again from
		// where it landed, up to 3 times.
		if (p_t - st.play_last_fall < 8.0) {
			st.release_weapon_input();
			mp.release_all();
			st.poll_report();
			return false;
		}
		if (st.play_fall_retries_left > 0) {
			--st.play_fall_retries_left;
			st.note(fmt("alive after fall %u: health %.1f, dropping again", st.play_fall_deaths, double(st.f().own_curr.health)));
			st.play_drop(p_t);
			st.poll_report();
			return false;
		}
		if (st.play_team_at_start != 0 && st.g.own_team != st.play_team_at_start) {
			st.play_team_switched = true;
		}
		// Movement: a 30 s pattern of walk / sprint / strafe / back / crouch, the view sweeping.
		mp.script_hold(InputAction::Forward, slice == 0 || slice == 1 || slice == 4);
		mp.script_hold(InputAction::Sprint, slice == 1);
		mp.script_hold(InputAction::Right, slice == 2);
		mp.script_hold(InputAction::Back, slice == 3);
		mp.script_hold(InputAction::Left, slice == 4);
		if (std::fmod(p_t, 13.0) < st.dt) {
			mp.script_press(InputAction::Jump);
		}
		if (std::fmod(p_t, 47.0) < st.dt || std::fmod(p_t + 3.0, 47.0) < st.dt) {
			mp.script_press(InputAction::Crouch);
		}
		// Combat: every 8 s, 2.5 s at the nearest enemy within 80 m (bursts, half of them aimed down the sights).
		std::array<host::PresentedSoldier, net::kMaxSlots> others{};
		const size_t n = host::present_others(st.f(), st.now_ns, others);
		const sim::Vec3 eye = st.eye();
		int nearest = -1;
		float best = 80.0f;
		for (size_t i = 0; i < n; ++i) {
			const float d = vdist(others[i].position, eye);
			const uint8_t team = st.g.slot_team[others[i].slot];
			if (team != 0 && team == st.g.own_team) {
				continue; // a known teammate: the script fights the other team
			}
			if (others[i].mode != sim::MoveMode::Dead && d < best) {
				best = d;
				nearest = int(i);
			}
		}
		const double cyc = std::fmod(p_t, 8.0);
		const bool engage = cyc >= 5.5 && nearest >= 0 && st.g.weapon.valid && st.g.weapon.active_slot <= 1;
		if (engage) {
			const host::PresentedSoldier &o = others[size_t(nearest)];
			st.aim_at(eye, { o.position[0], o.position[1] + std::max(0.5f, o.eye_height - 0.35f), o.position[2] });
			mp.script_hold(InputAction::Forward, false);
			mp.script_hold(InputAction::Sprint, false);
			mp.m2a_script_hold(Action::Fire, std::fmod(cyc, 0.7) < 0.4);
			mp.m2a_script_hold(Action::Ads, int(p_t / 8.0) % 2 == 1);
			// The firefight shot: a bot within 45 m in the line of sight (the check's collision world).
			bool seen = st.g.world == nullptr;
			if (!seen) {
				const sim::Vec3 aim = { o.position[0], o.position[1] + std::max(0.5f, o.eye_height - 0.35f), o.position[2] };
				seen = !st.g.world->ray(eye, { aim[0] - eye[0], aim[1] - eye[1], aim[2] - eye[2] }, sim::query_mask::kMovement);
			}
			if (!st.play_shot_taken[3] && cyc >= 6.5 && best < 45.0f && seen) {
				st.play_shot_taken[3] = st.save_shot("gx_firefight");
			}
			if (!st.g.weapon.zoomed) {
				st.play_zoomed_since = -1;
			} else if (st.play_zoomed_since < 0) {
				st.play_zoomed_since = p_t;
			}
			if (!st.play_shot_taken[1] && int(p_t / 8.0) % 2 == 1 && st.play_zoomed_since >= 0 && p_t - st.play_zoomed_since >= 0.4) {
				st.play_shot_taken[1] = st.save_shot("gx_ads");
			}
		} else {
			mp.m2a_script_hold(Action::Fire, false);
			mp.m2a_script_hold(Action::Ads, false);
			const float yaw = float(std::fmod(p_t * 0.35, 2.0 * kPi));
			mp.script_view(yaw, float(0.08 * std::sin(p_t * 0.7)));
		}
		// Hip shot early on, reload with its shot, switches, the grenade and the knife.
		if (!st.play_shot_taken[0] && p_t >= 6.0 && st.ready() && !st.g.weapon.zoomed) {
			st.play_shot_taken[0] = st.save_shot("gx_hip");
		}
		if (seg >= 20.0 && seg < 20.0 + st.dt) {
			mp.m2a_script_press(Action::Reload);
		}
		if (!st.play_shot_taken[2] && seg >= 20.5 && seg < 22.0 && st.g.weapon.phase == sw::WeaponPhase::Reloading) {
			st.play_shot_taken[2] = st.save_shot("gx_reload");
		}
		const double cyc40 = std::fmod(p_t, 40.0);
		if (cyc40 >= 10.0 && cyc40 < 10.0 + st.dt) {
			mp.m2a_script_select(1);
		} else if (cyc40 >= 14.0 && cyc40 < 14.0 + st.dt) {
			mp.m2a_script_select(0);
		}
		const double cyc60 = std::fmod(p_t, 60.0);
		if (cyc60 >= 33.0 && cyc60 < 33.0 + st.dt) {
			mp.m2a_script_select(6);
		} else if (cyc60 >= 35.0 && cyc60 < 35.4 && st.g.weapon.active_slot == 6) {
			mp.m2a_script_hold(Action::Fire, true);
		} else if (cyc60 >= 35.4 && cyc60 < 35.4 + st.dt) {
			mp.m2a_script_hold(Action::Fire, false);
		} else if (cyc60 >= 38.0 && cyc60 < 38.0 + st.dt) {
			mp.m2a_script_select(7);
		} else if (cyc60 >= 39.0 && cyc60 < 39.0 + st.dt) {
			mp.m2a_script_press(Action::Fire);
		} else if (cyc60 >= 40.0 && cyc60 < 40.0 + st.dt) {
			mp.m2a_script_select(0);
		}
		// K / T at 120 s: the next spawn is the next kit on the other team.
		if (p_t >= 120.0 && p_t < 120.0 + st.dt) {
			mp.m2a_script_press(Action::Kit);
			mp.m2a_script_press(Action::Team);
		}
		// A fall to death every 70 s (play_drop).
		if (p_t - st.play_last_fall >= 70.0 && p_t >= 50.0 && p_t < T - 40.0) {
			++st.play_fall_deaths;
			st.play_fall_retries_left = 3;
			st.play_drop(p_t);
		}
		st.poll_report();
		return false;
	});
	add("play rows", 1.0, [](State &st, double, bool) {
		const host::ClientStats &cs = st.f().stats;
		const uint64_t deaths = st.g.deaths - st.deaths_mark;
		st.row("play.seconds", st.options.play_seconds, "the scripted run ran its length", true);
		st.row("play.fall_deaths_asked", double(st.play_fall_deaths), fmt(">= 3 falls (%u drops of 60 m)", st.play_fall_drops), st.play_fall_deaths >= 3);
		st.row("play.deaths", double(deaths), ">= the falls (Death events with kOwnVictim)", deaths >= st.play_fall_deaths && deaths >= 3);
		st.row("play.respawns", double(st.g.respawns), ">= 3 lives after the first", st.g.respawns >= 3);
		st.row("play.own_shots", double(st.g.own_shots_seen - st.mark_own_shots), ">= 50 OwnShot events", st.g.own_shots_seen - st.mark_own_shots >= 50);
		st.row("play.kills", double(st.g.kills), "kills by the player (reported)", true);
		st.row("play.team_kills", double(st.g.team_kills[1] + st.g.team_kills[2]), fmt("US %u : RU %u (reported)", st.g.team_kills[1], st.g.team_kills[2]), true);
		st.row("play.team_switch", st.play_team_switched ? 1.0 : 0.0, "K / T at 120 s: the next life spawned on the other team", st.play_team_switched);
		st.row("play.viewmodel_built", st.g.vm[0] != nullptr && st.g.vm[1] != nullptr ? 1.0 : 0.0, "both team viewmodels built", st.g.vm[0] != nullptr && st.g.vm[1] != nullptr);
		st.row("play.vm_unmapped_frames", double(st.g.vm_unmapped), "every presented def had a viewmodel item", st.g.vm_unmapped == 0);
		st.row("play.hud_loaded", st.g.hud != nullptr && st.g.hud->is_loaded() ? 1.0 : 0.0, "Bf3Hud loaded", st.g.hud != nullptr && st.g.hud->is_loaded());
		st.row("play.events_overflow", double(cs.events_overflow), "== 0 (E14's gate)", cs.events_overflow == 0);
		st.row("play.bad_gunplay_events", double(cs.bad_gunplay_events), "== 0", cs.bad_gunplay_events == 0);
		st.row("play.events_drained", double(st.g.events_drained), "> 0 events flowed", st.g.events_drained > 0);
		st.row("play.remote_shots", double(st.g.remote_shots_seen), "> 0 RemoteShot events (bt's bots fire)", !OPENBF3_M2A_HAVE_BT || st.g.remote_shots_seen > 0);
		st.row("play.killed_by_enemy", double(st.g.deaths_by_enemy), "own deaths by another soldier (reported)", true);
		st.row("play.firefight_bots", double(st.g.firefight_bots), "bt's firefight bots in the match (reported)", true);
		const bool crowd_ok = st.g.crowd != nullptr && st.g.crowd_built;
		st.row("play.crowd_built", crowd_ok ? 1.0 : 0.0, "tp's 3p soldiers drawn (Bf3GameCrowd built; the capsules hidden)", crowd_ok);
		if (crowd_ok) {
			const GameCrowdStats cr = st.g.crowd->stats();
			st.row("play.crowd_frames", double(cr.frames), fmt("crowd steps (%" PRIu64 " frozen, %u unknown weapons; reported)", cr.frozen_frames, cr.unknown_weapons), cr.frames > 0);
		}
		const bool fx_ok = st.g.fx != nullptr && st.g.fx->built();
		st.row("play.fx_built", fx_ok ? 1.0 : 0.0, "fx3's effects built (Bf3GameFx)", fx_ok);
		if (fx_ok) {
			const GameFxStats fs = st.g.fx->stats();
			st.row("play.fx_muzzles", double(fs.triggers.muzzle_starts),
					fmt("muzzle starts (%" PRIu64 " remote shots without a crowd muzzle; %" PRIu64 " own 1p muzzles from fx3, %" PRIu64 " from the stand-in)", fs.triggers.muzzle_fallbacks,
							st.g.own_muzzles_fx, st.g.own_muzzles_standin),
					fs.triggers.muzzle_starts > 0);
			st.row("play.fx_impacts", double(fs.triggers.impacts), fmt("impact cells started (%" PRIu64 " decals, %" PRIu64 " double starts)", fs.triggers.decals, fs.triggers.double_starts),
					fs.triggers.impacts > 0 && fs.triggers.double_starts == 0);
		}
		const bool audio_ok = st.g.audio != nullptr && st.g.audio->built();
		st.row("play.audio_built", audio_ok ? 1.0 : 0.0, "au3's game audio built (Bf3GameAudio)", audio_ok);
		if (audio_ok) {
			const GameAudioStats as = st.g.audio->stats();
			st.row("play.audio_mixed_s", double(as.mixer_frames) / double(ba::kMixRate), fmt("seconds mixed (peak %u voices, %" PRIu64 " impacts, %" PRIu64 " reloads)", as.peak_instances,
																							  as.impacts.impacts, as.router.reloads),
					as.mixer_frames > 0);
			st.row("play.audio_late_commands", double(as.late_commands), "== 0 (short / late blocks are wall clock: reported)", as.late_commands == 0);
		}
		st.row("play.weapon_reconciliations", double(cs.weapon_reconciliations), "reported", true);
		st.row("play.kit_selects", double(st.g.kit_selects_sent), ">= 1 KitSelect sent after Spawned", st.g.kit_selects_sent >= 1);
		st.row("play.screenshots", double(st.shots_saved), st.options.shots_dir.empty() ? "no --m2a-shots dir (reported)" : "5 (hip, ADS, reload, firefight, death)",
				st.options.shots_dir.empty() || st.shots_saved >= 5);
		return true;
	});
}

// E7 (au3's GameAudioCheck in the running game). The shooter is the own soldier, heard in 3p (M2aFrame::free_camera
// with the listener held by the check: the router plays an own shot at its OwnShot position then, exactly as a
// remote one), firing M16A3 bursts of the script's rounds up into the sky; the listener holds at each distance on the
// aim axis, facing the muzzle; then the client-local auditions at their band distances. analyse_game_check judges the
// recording with au's rules. Deviation from m2a.md E7 ("a server bot fires"): no bot fires on command (bt's brains
// pick their own targets), and the own soldier's 3p route is the router's remote route with the own slot.
void M2aCheck::State::build_audio() {
	add("audio: built", 20.0, [](State &st, double, bool) { return st.g.audio != nullptr && st.g.audio->built(); });
	add("audio: range lane", 5.0, [](State &st, double, bool) {
		if (st.g.world != nullptr && st.s.shared_level() != nullptr) {
			st.lane = sw::find_range_lane(*st.g.world, st.s.shared_level()->spawns, st.f().eye_height_curr, 30.0f);
		}
		return true;
	});
	teleport_lane();
	select_slot(0);
	automatic_mode();
	full_magazine();
	add("audio: aim up", 1.0, [](State &st, double p_t, bool p_first) {
		if (p_first) {
			st.mapper().script_view(st.mapper().view().yaw, float(kAudioPitchDeg * kPi / 180.0));
		}
		return p_t >= 0.5;
	});
	// One round first: its OwnShot position is the emitter the listener lines up with.
	add("audio: probe round", 4.0, [](State &st, double p_t, bool p_first) {
		if (p_first) {
			st.aorigin_valid = false;
			st.mapper().m2a_script_press(Action::Fire);
		}
		for (const host::PresentEvent &e : st.g.events) {
			if (e.kind == host::PresentKind::OwnShot && !st.aorigin_valid) {
				st.aorigin = Vector3(e.pos[0], e.pos[1], e.pos[2]);
				const double yaw = st.mapper().view().yaw, pitch = st.mapper().view().pitch;
				st.adir = Vector3(float(std::sin(yaw) * std::cos(pitch)), float(std::sin(pitch)), float(std::cos(yaw) * std::cos(pitch))).normalized();
				st.aorigin_valid = true;
			}
		}
		return st.aorigin_valid && p_t > 0.3 && st.ready();
	});
	full_magazine();
	add("audio: recorder", 2.0, [](State &st, double, bool) {
		st.ascript = ba::game_check_script();
		// The recording holds the whole run: the script's 120 s plus the reloads between the steps of a real magazine.
		st.ascript.timeout_s = std::max(st.ascript.timeout_s, 200.0);
		const sw::WeaponSimDef *d = st.def(st.g.weapon.def);
		if (d != nullptr) {
			const double interval = sw::units_to_seconds(d->modes[std::min<size_t>(st.g.weapon.mode, 3)].interval);
			st.ascript.analysis.shot_interval_s = interval;
			st.ascript.analysis.expected_spacing_frames = interval * double(ba::kMixRate);
		}
		const String path = "res://config/audio_tuning.cfg"; // the file Bf3GameAudio::build reads
		if (FileAccess::file_exists(path)) {
			bf3::core::Result<ba::AudioTuning> t = ba::parse_audio_tuning(to_std(FileAccess::get_file_as_string(path)));
			if (t) {
				st.atuning = *t;
			} else {
				st.note("audio_tuning.cfg: " + t.error().message + " (defaults)");
			}
		}
		st.acheck = std::make_unique<ba::GameAudioCheck>(st.ascript, ba::kMixRate, st.g.audio->stats().mixer_frames);
		st.acheck->set_guard(st.s.install().write_guard());
		st.acheck->set_shooter(st.g.own_slot);
		st.g.audio->set_check(st.acheck.get());
		st.astep = -1;
		st.abursts_left = 0;
		st.aburst_firing = false;
		st.note(fmt("shooter slot %u at (%.1f, %.1f, %.1f), aim pitch %.0f deg; %zu steps, %u rounds per burst", unsigned(st.g.own_slot), double(st.aorigin.x),
				double(st.aorigin.y), double(st.aorigin.z), kAudioPitchDeg, st.ascript.steps.size(), st.ascript.rounds));
		return true;
	});
	add("audio: run", 400.0, [](State &st, double p_t, bool) {
		const ba::GameCheckScript &sc = st.ascript;
		ba::GameAudioCheck &ck = *st.acheck;
		if (st.aburst_firing) {
			for (const host::PresentEvent &e : st.g.events) {
				st.aburst_shots += e.kind == host::PresentKind::OwnShot ? 1u : 0u;
			}
			if (st.aburst_shots >= sc.rounds || !st.g.alive) {
				st.mapper().m2a_script_hold(Action::Fire, false);
				st.aburst_firing = false;
			}
		}
		const bool current_done = st.astep >= 0 && !st.aburst_firing && st.abursts_left == 0 && st.aaudition_started && ck.step_done(uint32_t(st.astep));
		if (st.astep < 0 || current_done) {
			if (st.astep + 1 >= int64_t(sc.steps.size())) {
				return true;
			}
			const ba::GameCheckStep &next = sc.steps[size_t(st.astep + 1)];
			const int32_t need = next.audition ? 0 : int32_t(next.bursts * sc.rounds);
			if (st.g.weapon.valid && st.g.weapon.magazine < need) {
				if (st.ready()) {
					st.mapper().m2a_script_press(Action::Reload); // between steps: the reload is outside every window
				}
				return false;
			}
			if (!st.ready()) {
				return false;
			}
			++st.astep;
			st.g.listener_position = st.aorigin + st.adir * next.distance_m;
			st.g.listener_forward = -st.adir;
			st.g.listener_override = true;
			ck.begin_step(uint32_t(st.astep), (st.g.listener_position - st.aorigin).length());
			st.astep_t0 = p_t;
			st.anext_burst = p_t + sc.settle_s + sc.analysis.start_s;
			st.abursts_left = next.audition ? 0u : next.bursts;
			st.aaudition_started = !next.audition;
			st.note(fmt("step %s: listener %.1f m on the aim axis at %.1f s", next.name.c_str(), double(next.distance_m), p_t));
			return false;
		}
		const ba::GameCheckStep &cur = sc.steps[size_t(st.astep)];
		if (cur.audition) {
			if (!st.aaudition_started && p_t >= st.astep_t0 + sc.settle_s) {
				std::string err;
				const fx::Vec3 at{ st.aorigin.x, st.aorigin.y, st.aorigin.z };
				if (!st.g.audio->audition(cur.patch, at, &err)) {
					st.note("audition " + cur.name + " refused: " + err); // the check skipped (and fails) the step
				}
				st.aaudition_started = true;
			}
			return false;
		}
		if (st.abursts_left > 0 && !st.aburst_firing && p_t >= st.anext_burst && st.ready()) {
			st.aburst_shots = 0;
			st.aburst_firing = true;
			st.mapper().m2a_script_hold(Action::Fire, true);
			--st.abursts_left;
			st.anext_burst = std::max(st.anext_burst + sc.burst_period_s, p_t + 0.5);
		}
		return false;
	});
	add("audio: rows", 2.0, [](State &st, double, bool) {
		st.mapper().m2a_script_hold(Action::Fire, false);
		st.g.audio->set_check(nullptr);
		st.g.listener_override = false;
		const GameAudioStats as = st.g.audio->stats();
		ba::GameCheckCounters c;
		c.short_blocks = as.short_blocks;
		c.late_blocks = as.late_blocks;
		c.late_commands = as.late_commands;
		c.mixer_refusals = as.router.mixer_refusals;
		const ba::CheckReport rep = ba::analyse_game_check(st.ascript, *st.acheck, c, st.atuning, ba::kMixRate);
		const bool gate_late = env_is_one("OPENBF3_AU3_GATE_LATE");
		for (const ba::CheckRow &r : rep.rows) {
			const bool timing = r.name == "mixer.late_blocks";
			st.row("e7." + r.name, r.value, r.rule + (timing && !gate_late ? " (wall clock: reported on a branch; OPENBF3_AU3_GATE_LATE=1 gates it)" : ""),
					r.pass || (timing && !gate_late));
		}
		// Item 1: every kit item compiles and binds (Bf3GameAudio::load_report: "<unlock> [<patch>]: driver | one-shot").
		uint32_t items = 0, drivers = 0, one_shots = 0;
		for (const std::string &l : st.g.audio->load_report()) {
			if (l.rfind("weapons/", 0) != 0) {
				continue;
			}
			++items;
			drivers += l.find("]: driver") != std::string::npos ? 1u : 0u;
			one_shots += l.find("]: one-shot") != std::string::npos ? 1u : 0u;
		}
		st.row("e7.item1.kit_items", double(items), "12 kit items in the load report", items == 12);
		st.row("e7.item1.forms", double(drivers + one_shots), fmt("every kit item bound: %u drivers (10 firearms), %u one-shots (knife, M67)", drivers, one_shots),
				drivers == 10 && one_shots == 2);
		st.row("e7.item1.program_failures", double(as.program_failures), fmt("== 0 (%u programs)", as.programs), as.program_failures == 0 && as.programs > 0);
		st.row("e7.steps_skipped", double(st.acheck->skipped().size()), "== 0 (every audition item loaded and started)", st.acheck->skipped().empty());
		st.row("e7.dropped_frames", double(st.acheck->dropped_frames()), "== 0 (the recording held the run)", st.acheck->dropped_frames() == 0);
		st.row("e7.ring_overflows", double(as.ring_overflows), "== 0", as.ring_overflows == 0);
		if (!st.options.shots_dir.empty()) {
			const bf3::core::Status w = st.acheck->write_wav(st.options.shots_dir / "gx_e7.wav");
			st.note(w.ok() ? std::string("recording: gx_e7.wav") : "recording not written: " + w.error().message);
		}
		return true;
	});
}

// The E8 bookkeeping of one display frame (after Bf3GameFx::step in m2a_frame): the starts, decals and sound cues of
// this frame's events, the renderer's instance count against the world's particles.
void M2aCheck::State::fx_account() {
	if (g.fx == nullptr || !g.fx->built() || g.fx->triggers() == nullptr) {
		return;
	}
	++fx_frames;
	const GameFxStats fs = g.fx->stats();
	const Bf3FxRenderer *r = g.fx->renderer();
	if (r != nullptr && r->stats().drawn_instances != fs.particles) {
		++fx_mismatch_frames;
	}
	std::map<std::pair<uint8_t, uint32_t>, uint8_t> impact_material; // (slot, id) -> PhysicsMaterialIndex
	for (const host::PresentEvent &e : g.events) {
		if (e.kind == host::PresentKind::OwnShot) {
			++fx_own_shot_events;
			fx_tracer_flags += (e.flags & host::present_flags::kTracer) != 0 ? 1u : 0u;
		} else if (e.kind == host::PresentKind::Impact) {
			++fx_impact_events;
			impact_material[{ e.slot, e.id }] = uint8_t(e.material);
		}
	}
	const fx::FxTriggers &t = *g.fx->triggers();
	int frame_material = -1; // the one material of this frame's impact starts (-2: several)
	for (const fx::TriggerStart &ts : t.starts()) {
		if (ts.def == nullptr) {
			continue;
		}
		if (ts.kind == host::PresentKind::OwnShot && ts.purpose == fx::TriggerPurpose::Muzzle && ts.first_person) {
			if (fx_m16_1p.empty() || ts.def->name != fx_m16_1p) {
				continue;
			}
			if (fx_flash_component < 0) {
				for (size_t c = 0; c < ts.def->components.size(); ++c) {
					const fx::EffectComponentDef &cd = ts.def->components[c];
					if (cd.kind == fx::ComponentKind::Emitter && cd.emitter < ts.def->emitters.size() &&
							ts.def->emitters[cd.emitter].name.find("_1p_flash") != std::string::npos) {
						fx_flash_component = int32_t(c);
					}
				}
			}
			++fx_shots_1p;
			++fx_seg_shots;
			fx_flash += fx_flash_component >= 0 && fx::fx_component_selected(*ts.def, uint32_t(fx_flash_component), ts.seed, fx::kTierHigh /* FxTriggerConfig's default tier, Bf3GameFx's */) ? 1u : 0u;
		} else if (ts.kind == host::PresentKind::Impact && ts.purpose == fx::TriggerPurpose::ImpactEffect) {
			const auto it = impact_material.find({ ts.slot, ts.id });
			if (it == impact_material.end()) {
				continue;
			}
			const uint8_t m = it->second;
			frame_material = frame_material == -1 || frame_material == int(m) ? int(m) : -2;
			std::array<uint64_t, 4> &row = fx_mat[m];
			++row[0];
			const FxNamedCell *named = fx_named_cell(m);
			if (named != nullptr) {
				const bool ok = leaf(ts.def->name) == named->effect;
				row[1] += ok ? 1u : 0u;
				if (!ok && fx_first_bad.empty()) {
					fx_first_bad = fmt("material %u started %s, not %s", unsigned(m), leaf(ts.def->name).c_str(), named->effect);
				}
			}
		} else if (ts.kind == host::PresentKind::SoldierContact && ts.purpose == fx::TriggerPurpose::ImpactEffect) {
			++fx_soldier_starts;
			fx_soldier_ok += leaf(ts.def->name) == kFxSoldierEffect ? 1u : 0u;
		}
	}
	for (const fx::DecalCue &d : t.decals()) {
		if (frame_material >= 0) {
			std::array<uint64_t, 4> &row = fx_mat[uint8_t(frame_material)];
			++row[2];
			const FxNamedCell *named = fx_named_cell(uint8_t(frame_material));
			row[3] += named != nullptr && leaf(d.decal) == named->decal ? 1u : 0u;
		} else if (frame_material == -1) {
			++fx_soldier_decals; // a decal without a static impact this frame (a soldier contact must leave none)
		}
	}
}

// E8 in the game: the fx tables (1, 6); 1,000 predicted M16A3 shots at Metro concrete and brick (3: the 1p flash
// fraction and the drawn instances; 5: the named cells' effects and decals; 7: tracers, shells, sounds, no double
// start); the client-local oilfire after 15 s (4); a soldier hit (5).
void M2aCheck::State::build_fx() {
	add("fx: built", 20.0, [](State &st, double, bool) { return st.g.fx != nullptr && st.g.fx->built() && st.g.fx->fire_effects() != nullptr; });
	add("fx: tables", 2.0, [](State &st, double, bool) {
		const GameFxStats fs = st.g.fx->stats();
		st.row("e8.1.preload", double(fs.preload_failures), fmt("every kit and impact blueprint compiles: %u preloaded, 0 failures", fs.preloaded),
				fs.preloaded > 0 && fs.preload_failures == 0);
		st.row("e8.6.atlas", double(fs.atlas_entries), fmt("52 / 52 entries uploaded, %u default-texture fallbacks (0)", fs.atlas_fallbacks),
				fs.atlas_entries == 52 && fs.atlas_fallbacks == 0);
		const uint16_t m16 = st.loadout(1, 0).defs[0]; // US assault primary: the M16A3
		const fx::FireEffectTable *fire = st.g.fx->fire_effects();
		st.fx_m16_1p = m16 < fire->defs.size() && !fire->defs[m16].first_person.empty() ? fire->defs[m16].first_person.front().blueprint : std::string();
		st.row("e8.m16_1p_muzzle", st.fx_m16_1p.empty() ? 0.0 : 1.0, "the M16A3 has a FireEffects1p blueprint (" + leaf(st.fx_m16_1p) + ")", !st.fx_m16_1p.empty());
		return true;
	});
	// The surfaces: from every spawn of the mode, the first heading (5 degree steps, a few pitches) whose ray meets a
	// collision triangle of the wanted PhysicsMaterialIndex 6..30 m out, facing the shooter.
	add("fx: surfaces", 5.0, [](State &st, double, bool) {
		const std::shared_ptr<const host::SharedLevel> shared = st.s.shared_level();
		if (st.g.world == nullptr || shared == nullptr || shared->gunplay == nullptr || shared->gunplay->materials == nullptr) {
			return true;
		}
		const sw::SurfaceMaterialTable &materials = *shared->gunplay->materials;
		const float eye_h = st.f().eye_height_curr > 0.5f ? st.f().eye_height_curr : 1.6f;
		for (FxSurface &surface : st.fx_surfaces) {
			for (size_t i = 0; i < shared->spawns.size() && !surface.found; ++i) {
				const sim::Vec3 &feet = shared->spawns[i].position;
				const sim::Vec3 eye = { feet[0], feet[1] + eye_h, feet[2] };
				for (int h = 0; h < 72 && !surface.found; ++h) {
					for (const float pitch_deg : { -12.0f, -6.0f, -2.0f, 3.0f, 8.0f }) {
						const double yaw = double(h) * 5.0 * kPi / 180.0, pitch = double(pitch_deg) * kPi / 180.0;
						const sim::Vec3 dir = { float(std::sin(yaw) * std::cos(pitch)), float(std::sin(pitch)), float(std::cos(yaw) * std::cos(pitch)) };
						sim::StaticHit hit;
						if (!st.g.world->ray(eye, { dir[0] * 30.0f, dir[1] * 30.0f, dir[2] * 30.0f }, sim::query_mask::kMovement, &hit)) {
							continue;
						}
						const float dist = 30.0f * hit.fraction;
						const float facing = -(hit.normal[0] * dir[0] + hit.normal[1] * dir[1] + hit.normal[2] * dir[2]);
						const sw::SurfacePair pair = materials.static_pair(hit.tag.material);
						if (pair.resolved && pair.physics_material == surface.material && dist >= 6.0f && facing > 0.5f) {
							surface.found = true;
							surface.spawn = uint32_t(i);
							surface.point = hit.position;
							surface.distance = dist;
							break;
						}
					}
				}
			}
			const FxNamedCell *named = fx_named_cell(surface.material);
			st.note(surface.found ? fmt("%s (%u): spawn %u, %.1f m to (%.1f, %.1f, %.1f)", named->name, unsigned(surface.material), surface.spawn,
											double(surface.distance), double(surface.point[0]), double(surface.point[1]), double(surface.point[2]))
								  : fmt("%s (%u): no triangle within 30 m of a spawn", named->name, unsigned(surface.material)));
		}
		st.fx_trig0 = st.g.fx->stats().triggers;
		return true;
	});
	for (size_t k = 0; k < 2; ++k) {
		respawn_at([k](State &st) { return st.fx_surfaces[k].found ? st.fx_surfaces[k].spawn : 0u; });
		select_slot(0);
		automatic_mode();
		add(fmt("fx: segment %zu aim", k), 2.0, [k](State &st, double, bool p_first) {
			if (p_first) {
				st.fx_seg_shots = 0;
				if (st.fx_surfaces[k].found) {
					st.aim_at(st.eye(), st.fx_surfaces[k].point);
				}
			}
			return true;
		});
		if (k == 0) {
			// The oilfire 20 m ahead on the floor, started client-locally (no server request).
			add("fx: oilfire start", 30.0, [](State &st, double, bool) {
				const double yaw = st.mapper().view().yaw;
				const sim::Vec3 e = st.eye();
				const Vector3 at(float(double(e[0]) + std::sin(yaw) * 20.0), e[1] - st.f().eye_height_curr, float(double(e[2]) + std::cos(yaw) * 20.0));
				fx::Transform t;
				t.right = fx::Vec3{ 1, 0, 0 };
				t.up = fx::Vec3{ 0, 1, 0 };
				t.forward = fx::Vec3{ 0, 0, 1 };
				t.trans = fx::Vec3{ at.x, at.y, at.z };
				bf3::core::Result<fx::EffectHandle> h = st.g.fx->start_effect(kFxOilfire, t);
				st.fx_oil_started = h.ok();
				if (h) {
					st.fx_oil = *h;
				} else {
					st.note("oilfire: " + h.error().message);
				}
				st.fx_oil_t0 = st.case_t;
				return true;
			});
		}
		add(fmt("fx: segment %zu fire", k), 200.0, [k](State &st, double, bool p_first) {
			if (p_first) {
				st.mapper().m2a_script_hold(Action::Fire, true);
			}
			st.fx_account();
			// The oilfire's live particles at 15 s.
			if (st.fx_oil_started && st.fx_oil_particles < 0 && st.case_t - st.fx_oil_t0 >= kFxOilfireSeconds && st.g.fx->world() != nullptr) {
				int64_t n = 0;
				for (const fx::EmitterOutput &o : st.g.fx->world()->outputs()) {
					n += o.effect == st.fx_oil ? int64_t(o.particle_count) : 0;
				}
				st.fx_oil_particles = n;
			}
			if (!st.fx_surfaces[k].found || st.fx_seg_shots >= kFxShots / 2) {
				st.mapper().m2a_script_hold(Action::Fire, false);
				return st.fx_oil_particles >= 0 || !st.fx_oil_started || k > 0;
			}
			// Keep the aim on the surface; reload an empty magazine (the hold resumes when ready).
			if (st.g.weapon.valid && st.g.weapon.magazine == 0 && st.ready()) {
				st.mapper().m2a_script_press(Action::Reload);
			}
			st.aim_at(st.eye(), st.fx_surfaces[k].point);
			return false;
		});
		wait(0.5);
		add(fmt("fx: segment %zu tail", k), 1.0, [](State &st, double, bool) {
			st.fx_account();
			return true;
		});
	}
	// A soldier hit: the target bot 8 m down the range lane, single rounds at its chest.
	add("fx: lane", 5.0, [](State &st, double, bool) {
		if (st.g.world != nullptr && st.s.shared_level() != nullptr) {
			st.lane = sw::find_range_lane(*st.g.world, st.s.shared_level()->spawns, st.f().eye_height_curr, 20.0f);
		}
		return true;
	});
	teleport_lane();
	full_magazine();
	place_target(8.0f);
	add("fx: soldier hits", 6.0, [](State &st, double p_t, bool p_first) {
		if (p_first) {
			st.fx_soldier_starts = st.fx_soldier_ok = st.fx_soldier_decals = 0;
			st.taps = 0;
			st.t_mark = 0;
			if (!st.report.targets.empty()) {
				sw::zone_aim_point(st.report.targets[0].target, sw::HitZone::Chest, st.point);
			}
		}
		st.fx_account();
		if (!st.report.targets.empty()) {
			st.aim_at(st.eye(), st.point);
		}
		if (st.taps < 3 && p_t >= st.t_mark && st.ready()) {
			st.mapper().m2a_script_press(Action::Fire);
			++st.taps;
			st.t_mark = p_t + 0.4;
		}
		return st.taps >= 3 && p_t >= st.t_mark + 0.5;
	});
	add("fx: rows", 1.0, [](State &st, double, bool) {
		const fx::FxTriggerStats now = st.g.fx->stats().triggers;
		const fx::FxTriggerStats &a = st.fx_trig0;
		const double flash = st.fx_shots_1p > 0 ? double(st.fx_flash) / double(st.fx_shots_1p) : 0.0;
		st.row("e8.3.shots", double(st.fx_shots_1p), fmt(">= %" PRIu64 " predicted M16A3 shots with a 1p muzzle start", kFxShots), st.fx_shots_1p >= kFxShots);
		st.row("e8.3.flash_fraction", flash, fmt("the _1p_flash component present in [0.71, 0.79] of them (%" PRIu64 " of %" PRIu64 ")", st.fx_flash, st.fx_shots_1p),
				st.fx_shots_1p >= kFxShots && flash >= 0.71 && flash <= 0.79);
		st.row("e8.3.drawn_equals_particles", double(st.fx_mismatch_frames), fmt("frames whose drawn instances != FxWorld::particles() (0 of %" PRIu64 ")", st.fx_frames),
				st.fx_frames > 0 && st.fx_mismatch_frames == 0);
		st.row("e8.4.oilfire_particles", double(st.fx_oil_particles), fmt("the client-local oilfire draws %" PRId64 " +- 1 live particles after %.0f s", kFxOilfireParticles, kFxOilfireSeconds),
				st.fx_oil_started && std::llabs(st.fx_oil_particles - kFxOilfireParticles) <= 1);
		for (const FxSurface &surface : st.fx_surfaces) {
			const FxNamedCell *named = fx_named_cell(surface.material);
			const std::array<uint64_t, 4> row = st.fx_mat.count(surface.material) ? st.fx_mat[surface.material] : std::array<uint64_t, 4>{};
			const std::string tag = std::string("e8.5.") + named->name;
			st.row(tag + ".effect", double(row[1]), fmt("every impact on it started %s (%" PRIu64 " of %" PRIu64 ")%s", named->effect, row[1], row[0],
															 st.fx_first_bad.empty() ? "" : (", first other: " + st.fx_first_bad).c_str()),
					surface.found && row[0] > 0 && row[1] == row[0]);
			st.row(tag + ".decal", double(row[3]), fmt("its decals are %s (%" PRIu64 " of %" PRIu64 ")", named->decal, row[3], row[2]), surface.found && row[2] > 0 && row[3] == row[2]);
		}
		std::string others;
		for (const auto &[m, row] : st.fx_mat) {
			if (m != 8 && m != 2) {
				others += fmt(" %u: %" PRIu64 " impacts (%" PRIu64 " as named)", unsigned(m), row[0], row[1]);
			}
		}
		if (!others.empty()) {
			st.note("other materials hit:" + others);
		}
		st.row("e8.5.soldier", double(st.fx_soldier_ok), fmt("soldier contacts started %s (%" PRIu64 " of %" PRIu64 "), no decal (%" PRIu64 ")", kFxSoldierEffect, st.fx_soldier_ok,
														 st.fx_soldier_starts, st.fx_soldier_decals),
				st.fx_soldier_starts > 0 && st.fx_soldier_ok == st.fx_soldier_starts && st.fx_soldier_decals == 0);
		st.row("e8.5.water", 0.0, "Metro: no water (the water cell is fx.install.cells' Caspian row, E8 (5))", true);
		const uint64_t tracers = now.tracers - a.tracers, tracer_shots = now.tracer_shots - a.tracer_shots;
		st.row("e8.7.tracers", double(tracers), fmt("tracer starts == shots flagged kTracer with a tracer effect (%" PRIu64 "; %" PRIu64 " OwnShot events flagged)", tracer_shots, st.fx_tracer_flags),
				tracers == tracer_shots && tracer_shots == st.fx_tracer_flags && tracers > 0);
		const uint64_t shells = now.shells - a.shells, firearm = now.firearm_shots - a.firearm_shots;
		st.row("e8.7.shells", double(shells), fmt("one shell eject per firearm shot (%" PRIu64 ")", firearm), shells == firearm && shells > 0);
		const uint64_t sounds = now.sounds - a.sounds, impacts = now.impacts - a.impacts;
		st.row("e8.7.impact_sounds", double(sounds), fmt("one impact sound cue per impact cell started (%" PRIu64 "; %" PRIu64 " Impact events, %" PRIu64 " culled)", impacts,
															st.fx_impact_events, now.impacts_culled - a.impacts_culled),
				sounds == impacts && sounds > 0);
		st.row("e8.7.double_starts", double(now.double_starts - a.double_starts), "== 0 after the replays", now.double_starts == a.double_starts);
		return true;
	});
}

// E9 / E14 with bt's bots: 64 firefight bots in pairs in front of the observer (the player at the range lane, slot
// 64: never hit), 2560 x 1440 borderless, vsync off, 10 s warm-up, then 12 windows of 5 s with FX on and off in turn
// (the S3 G1b method for the FX GPU time). A timing case: the integrator runs it on main under the quiet lock.
void M2aCheck::State::build_firefight() {
	add("firefight: window", 3.0, [](State &st, double p_t, bool p_first) {
		if (p_first) {
			DisplayServer *ds = DisplayServer::get_singleton();
			const int32_t primary = ds->get_primary_screen();
			ds->window_set_mode(DisplayServer::WINDOW_MODE_WINDOWED);
			ds->window_set_flag(DisplayServer::WINDOW_FLAG_BORDERLESS, true);
			ds->window_set_current_screen(primary);
			ds->window_set_position(ds->screen_get_position(primary));
			ds->window_set_size(Vector2i(kFfWidth, kFfHeight));
			ds->window_set_vsync_mode(DisplayServer::VSYNC_DISABLED);
			if (Viewport *vp = st.s.get_viewport()) {
				RenderingServer::get_singleton()->viewport_set_measure_render_time(vp->get_viewport_rid(), true);
			}
		}
		return p_t >= 1.0;
	});
	add("firefight: lane", 5.0, [](State &st, double, bool) {
		if (st.g.world != nullptr && st.s.shared_level() != nullptr) {
			st.lane = sw::find_range_lane(*st.g.world, st.s.shared_level()->spawns, st.f().eye_height_curr, 60.0f);
		}
		return true;
	});
	teleport_lane(); // the observer: bt's default layout starts at the range lane (firefight_brain.h)
	add("firefight: warm-up", kFfWarmupSeconds + 5.0, [](State &st, double p_t, bool) {
		if (st.lane) {
			st.mapper().script_view(sim::yaw_radians(st.lane->yaw), 0.0f);
		}
		return p_t >= kFfWarmupSeconds;
	});
	add("firefight: measure", kFfWindowSeconds * kFfWindows + 10.0, [](State &st, double p_t, bool p_first) {
		if (p_first) {
			const GameAudioStats as = st.g.audio != nullptr ? st.g.audio->stats() : GameAudioStats();
			st.ff_late0 = as.late_blocks;
			st.ff_short0 = as.short_blocks;
			st.ff_remote0 = st.g.remote_shots_seen;
			st.ff_hits0 = st.g.hits_seen;
		}
		const uint32_t window = uint32_t(p_t / kFfWindowSeconds);
		if (window >= kFfWindows) {
			st.g.fx_off = false;
			return true;
		}
		const bool on = window % 2 == 0;
		const bool window_start = p_t - double(window) * kFfWindowSeconds < 0.5; // the first 0.5 s of a window settles
		RenderingServer *rs = RenderingServer::get_singleton();
		const double gpu = st.s.get_viewport() != nullptr ? rs->viewport_get_measured_render_time_gpu(st.s.get_viewport()->get_viewport_rid()) : 0.0;
		if (!window_start) {
			if (on) {
				st.ff_frame_on.push_back(st.dt * 1000.0);
				st.ff_m2a_on.push_back(st.g.ms_total);
				st.ff_gpu_on.push_back(gpu);
				if (const Bf3FxRenderer *r = st.g.fx != nullptr ? st.g.fx->renderer() : nullptr) {
					st.ff_fx_main.push_back(r->stats().step_ms + r->stats().prepare_ms + r->stats().upload_ms);
				}
			} else {
				st.ff_gpu_off.push_back(gpu);
			}
			st.ff_crowd.push_back(st.g.ms_crowd);
		}
		if (on && st.g.fx != nullptr) {
			const GameFxStats fs = st.g.fx->stats();
			st.ff_max_lights = std::max(st.ff_max_lights, fs.lights);
			st.ff_lights_over += fs.lights > fx::kMaxFxLights ? 1u : 0u;
			st.ff_max_particles = std::max(st.ff_max_particles, fs.particles);
		}
		if (st.g.crowd != nullptr && st.g.crowd_built) {
			st.ff_min_drawn = std::min(st.ff_min_drawn, st.g.crowd->stats().soldiers);
		}
		// The next frame's FX state (the FX-off window skips the FX step and hides its instances).
		const uint32_t next = uint32_t((p_t + st.dt) / kFfWindowSeconds);
		st.g.fx_off = next < kFfWindows && next % 2 == 1;
		return false;
	});
	add("firefight: rows", 1.0, [](State &st, double, bool) {
		const host::ClientStats &cs = st.f().stats;
		const host::FirefightCounters ff = st.g.firefight_stats != nullptr ? host::firefight_counters(*st.g.firefight_stats) : host::FirefightCounters();
		st.row("ff.bots", double(ff.bots), "64 firefight bots in pairs", ff.bots == 64);
		st.row("ff.bursts", double(ff.bursts), "the bots fired (bursts)", ff.bursts > 0);
		st.row("ff.remote_shots", double(st.g.remote_shots_seen - st.ff_remote0), "RemoteShot events presented during the window", st.g.remote_shots_seen > st.ff_remote0);
		st.row("ff.crowd_drawn_min", st.ff_min_drawn == 0xFFFFFFFFu ? 0.0 : double(st.ff_min_drawn), "3p soldiers drawn every frame (minimum; reported)", true);
		st.row("e9.lights_max", double(st.ff_max_lights), fmt("effect lights <= %u in every frame (%u frames over)", fx::kMaxFxLights, st.ff_lights_over), st.ff_lights_over == 0);
		st.row("e9.particles_max", double(st.ff_max_particles), "live particles (reported; <= 16k expected)", true);
		const double fx_main_p99 = percentile(st.ff_fx_main, 0.99);
		st.row("e9.fx_main_p99_ms", fx_main_p99, fmt("FX main thread (world step + RenderPrep + upload + lights) p99 <= 1.0 ms (%zu frames)", st.ff_fx_main.size()),
				!st.ff_fx_main.empty() && fx_main_p99 <= 1.0);
		const double gpu_on = percentile(st.ff_gpu_on, 0.5), gpu_off = percentile(st.ff_gpu_off, 0.5);
		st.row("e9.fx_gpu_ms", gpu_on - gpu_off, fmt("FX GPU time: p50 FX on %.3f - FX off %.3f ms <= 2.0 ms (alternating 5 s windows)", gpu_on, gpu_off),
				!st.ff_gpu_on.empty() && !st.ff_gpu_off.empty() && gpu_on - gpu_off <= 2.0);
		const double frame_p99 = percentile(st.ff_frame_on, 0.99);
		st.row("e14.frame_p99_ms", frame_p99, fmt("frame p99 <= 16.7 ms at %dx%d, vsync off (FX-on frames, %zu)", kFfWidth, kFfHeight, st.ff_frame_on.size()),
				!st.ff_frame_on.empty() && frame_p99 <= 16.7);
		const double m2a_p99 = percentile(st.ff_m2a_on, 0.99);
		st.row("e14.m2a_main_p99_ms", m2a_p99, "M2a main-thread additions (viewmodel, HUD, crowd, triggers + FX, audio router) p99 <= 3.0 ms", !st.ff_m2a_on.empty() && m2a_p99 <= 3.0);
		const double crowd_p99 = percentile(st.ff_crowd, 0.99);
		st.row("e14.crowd_step_p99_ms", crowd_p99, "Bf3GameCrowd::step p99 <= 2.0 ms (S3 G1a)", !st.ff_crowd.empty() && crowd_p99 <= 2.0);
		const GameAudioStats as = st.g.audio != nullptr ? st.g.audio->stats() : GameAudioStats();
		st.row("e14.audio_underruns", double(as.late_blocks - st.ff_late0 + as.short_blocks - st.ff_short0), "0 audio underruns (late + short blocks)",
				st.g.audio != nullptr && as.late_blocks == st.ff_late0 && as.short_blocks == st.ff_short0);
		st.row("e14.event_overflows", double(cs.events_overflow), "0 presentation-event overflows", cs.events_overflow == 0);
		return true;
	});
}

// Automatic: cycle the fire mode (B) until the weapon in hand is on its automatic mode.
void M2aCheck::State::automatic_mode() {
	add("automatic fire mode", 4.0, [](State &st, double p_t, bool p_first) {
		if (p_first) {
			st.t_mark = 0;
		}
		const sw::WeaponSimDef *d = st.def(st.g.weapon.def);
		if (d == nullptr) {
			return false;
		}
		if (d->modes[std::min<size_t>(st.g.weapon.mode, 3)].logic == core::gameplay::FireLogic::Automatic) {
			return st.ready();
		}
		if (p_t - st.t_mark > 0.3 || st.t_mark > p_t) {
			st.t_mark = p_t;
			st.mapper().m2a_script_press(Action::FireMode);
		}
		return false;
	});
}

void M2aCheck::State::finish_case() {
	if (acheck != nullptr && g.audio != nullptr) {
		g.audio->set_check(nullptr);
	}
	g.listener_override = false;
	g.fx_off = false;
	if (current.outcome != M2aOutcome::Skip) {
		for (const M2aRow &r : current.rows) {
			if (!r.pass) {
				current.outcome = M2aOutcome::Fail;
				if (current.reason.empty()) {
					current.reason = r.name;
				}
			}
		}
	}
	UtilityFunctions::print(to_godot(fmt("[m2a] case %s: %s%s%s (%.1f s)", std::string(m2a_case_name(current.which)).c_str(),
			current.outcome == M2aOutcome::Pass ? "PASS" : (current.outcome == M2aOutcome::Fail ? "FAIL" : "SKIP"), current.reason.empty() ? "" : ": ",
			current.reason.c_str(), case_t)));
	results.push_back(std::move(current));
	current = M2aCaseResult();
	release_weapon_input();
	mapper().release_all();
	running = false;
}

// ---- the driver -------------------------------------------------------------------------------------------------

M2aCheck::M2aCheck(Bf3LevelSession &r_session, M2aGame &r_game, std::vector<M2aCase> p_cases, const M2aCheckOptions &p_options) :
		state_(std::make_unique<State>(r_session, r_game)) {
	state_->cases = std::move(p_cases);
	state_->options = p_options;
	UtilityFunctions::print(to_godot(fmt("[m2a] check: %zu case(s)", state_->cases.size())));
}

M2aCheck::~M2aCheck() = default;

bool M2aCheck::frame(uint64_t p_now_ns, double p_dt) {
	State &st = *state_;
	if (st.done) {
		return true;
	}
	st.now_ns = p_now_ns;
	st.dt = p_dt;
	if (!st.running) {
		if (st.next_case >= st.cases.size()) {
			st.done = true;
			st.mapper().set_scripted(false);
			return true;
		}
		st.build_case(st.cases[st.next_case++]);
		st.running = true;
		if (st.current.outcome == M2aOutcome::Skip) {
			st.finish_case();
			return false;
		}
	}
	st.case_t += p_dt;
	if (st.step >= st.steps.size()) {
		st.finish_case();
		return false;
	}
	State::Step &s = st.steps[st.step];
	const bool first = st.step_first;
	st.step_first = false;
	const bool step_done = s.fn(st, st.step_t, first);
	st.step_t += p_dt;
	if (step_done) {
		++st.step;
		st.step_t = 0;
		st.step_first = true;
	} else if (st.step_t > s.timeout || st.case_t > st.case_timeout) {
		st.row(fmt("step.%s", s.name.c_str()), st.step_t, fmt("timed out after %.1f s", s.timeout), false);
		st.current.reason = "step '" + s.name + "' timed out";
		st.step = st.steps.size();
	}
	return false;
}

int M2aCheck::exit_code() const {
	for (const M2aCaseResult &r : state_->results) {
		if (r.outcome == M2aOutcome::Fail) {
			return 1;
		}
	}
	return state_->results.size() == state_->cases.size() ? 0 : 1;
}

std::vector<M2aCaseResult> M2aCheck::results() const {
	return state_->results;
}

std::string M2aCheck::report() const {
	std::string out;
	uint32_t pass = 0, fail = 0, skip = 0;
	for (const M2aCaseResult &r : state_->results) {
		const std::string c(m2a_case_name(r.which));
		if (r.outcome == M2aOutcome::Skip) {
			out += "SKIP " + c + " " + r.reason + "\n";
			++skip;
			continue;
		}
		for (const M2aRow &row : r.rows) {
			out += fmt("%s %s.%s %.6g %s\n", row.pass ? "PASS" : "FAIL", c.c_str(), row.name.c_str(), row.value, row.rule.c_str());
		}
		(r.outcome == M2aOutcome::Pass ? pass : fail) += 1;
	}
	out += fmt("M2A CHECK: %s (%u pass, %u fail, %u skip of %zu cases)", fail == 0 && pass + skip == state_->cases.size() ? "PASS" : "FAIL", pass, fail, skip,
			state_->cases.size());
	return out;
}

} // namespace bf3::godot_side
