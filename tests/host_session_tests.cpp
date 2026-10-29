#include <doctest/doctest.h>

#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/host/client_session.h"
#include "bf3/host/game_server.h"
#include "bf3/host/level_load.h"
#include "bf3/host/rest_check.h"
#include "bf3/host/server_host.h"
#include "bf3/net/loopback_ping.h"
#include "bf3/net/netsim.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/spawn.h"
#include "bf3/sim/static_world.h"
#include "client_transport.h"
#include "server_net_thread.h"
#include "test_support.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace bf3;
using namespace bf3::host;
using namespace bf3::sim;
namespace gameplay = bf3::core::gameplay;
using namespace std::chrono_literals;

namespace {

void add_quad(std::vector<StaticTriangle> &r_out, const Vec3 &p_a, const Vec3 &p_b, const Vec3 &p_c, const Vec3 &p_d) {
	StaticTriangle t;
	t.v = { p_a, p_b, p_c };
	r_out.push_back(t);
	t.v = { p_a, p_c, p_d };
	r_out.push_back(t);
}

void add_floor(std::vector<StaticTriangle> &r_out, float p_x0, float p_z0, float p_x1, float p_z1, float p_y) {
	for (float x = p_x0; x < p_x1; x += 8.0f) {
		for (float z = p_z0; z < p_z1; z += 8.0f) {
			const float xe = std::min(x + 8.0f, p_x1);
			const float ze = std::min(z + 8.0f, p_z1);
			add_quad(r_out, { x, p_y, z }, { x, p_y, ze }, { xe, p_y, ze }, { xe, p_y, z });
		}
	}
}

void add_box(std::vector<StaticTriangle> &r_out, const Vec3 &p_lo, const Vec3 &p_hi) {
	const float x0 = p_lo[0], y0 = p_lo[1], z0 = p_lo[2], x1 = p_hi[0], y1 = p_hi[1], z1 = p_hi[2];
	add_quad(r_out, { x0, y1, z0 }, { x0, y1, z1 }, { x1, y1, z1 }, { x1, y1, z0 });
	add_quad(r_out, { x0, y0, z0 }, { x1, y0, z0 }, { x1, y0, z1 }, { x0, y0, z1 });
	add_quad(r_out, { x0, y0, z0 }, { x0, y0, z1 }, { x0, y1, z1 }, { x0, y1, z0 });
	add_quad(r_out, { x1, y0, z0 }, { x1, y1, z0 }, { x1, y1, z1 }, { x1, y0, z1 });
	add_quad(r_out, { x0, y0, z0 }, { x0, y1, z0 }, { x1, y1, z0 }, { x1, y0, z0 });
	add_quad(r_out, { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 });
}

gameplay::StatePoseSpeed speed(gameplay::SoldierPose p_pose, float p_v, float p_f, float p_b, float p_side, float p_accel, float p_decel,
		float p_sprint_gain, float p_sprint) {
	gameplay::StatePoseSpeed s;
	s.pose = p_pose;
	s.velocity = p_v;
	s.forward = p_f;
	s.backward = p_b;
	s.left = p_side;
	s.right = p_side;
	s.acceleration_gain = p_accel;
	s.deceleration_gain = p_decel;
	s.sprint_gain = p_sprint_gain;
	s.sprint_multiplier = p_sprint;
	return s;
}

gameplay::SoldierDef synthetic_soldier() {
	using gameplay::SoldierPose;
	gameplay::SoldierDef d;
	d.blueprint = "characters/soldiers/mpsoldier";
	d.max_health = 100;
	d.immortal_time_after_spawn = 2;
	d.mass = 100;
	d.radius = 0.35f;
	d.max_ascend_angle = 50;
	d.jump_penalty_time = 1;
	d.jump_penalty_factor = 0.3f;
	d.allow_pose_change_during_transition = true;
	d.body_sprint_multiplier = 1;
	const std::vector<gameplay::CurvePoint> throttle = { { 0, 0 }, { 0.1f, 0 }, { 1, 1 } };
	gameplay::PoseDef stand;
	stand.pose = SoldierPose::Stand;
	stand.height = 1.8f;
	stand.eye_height = 1.6f;
	stand.step_height = 0.3f;
	stand.throttle_curve = throttle;
	stand.min_look_pitch = -45;
	stand.max_look_pitch = 45;
	stand.transition_to[1] = 0.3f;
	stand.transition_to[2] = 0.7f;
	stand.box_min_expand = { 0.2f, 0.0f, 0.1f };
	stand.box_max_expand = { 0.1f, 0.6f, 0.2f };
	gameplay::PoseDef crouch = stand;
	crouch.pose = SoldierPose::Crouch;
	crouch.height = 1.15f;
	crouch.eye_height = 1.0f;
	crouch.box_min_expand = { 0.1f, 0.0f, 0.6f };
	crouch.box_max_expand = { 0.2f, 0.1f, 0.4f };
	crouch.transition_to = {};
	crouch.transition_to[0] = 0.3f;
	crouch.transition_to[2] = 0.5f;
	gameplay::PoseDef prone = stand;
	prone.pose = SoldierPose::Prone;
	prone.height = 0.3f;
	prone.eye_height = 0.3f;
	prone.box_min_expand = { 0.3f, 0.0f, 1.3f };
	prone.box_max_expand = { 0.3f, 0.0f, 1.1f };
	prone.min_look_pitch = -30;
	prone.transition_to = {};
	prone.transition_to[0] = 0.7f;
	prone.transition_to[1] = 1.05f;
	d.poses = { stand, crouch, prone };
	gameplay::MovementStateDef ground;
	ground.type = "OnGroundStateData";
	ground.poses = { speed(SoldierPose::Stand, 4, 1, 0.75f, 0.75f, 0.35f, -15, 0.25f, 1.625f), speed(SoldierPose::Crouch, 2, 1, 0.8f, 0.8f, 0.35f, -15, 1, 0),
		speed(SoldierPose::Prone, 0.9f, 1, 0.5f, 0.7f, 0.35f, -15, 1, 0) };
	ground.scalars = { { "AllowedDistanceFromGround", 0.5 }, { "GroundHugging", 1 } };
	gameplay::MovementStateDef jump;
	jump.type = "JumpStateData";
	jump.poses = { speed(SoldierPose::Stand, 4, 1, 1, 1, 0, 0, 0, 1.2f) };
	jump.scalars = { { "JumpHeight", 0.6 } };
	gameplay::MovementStateDef air;
	air.type = "InAirStateData";
	for (SoldierPose p : { SoldierPose::Stand, SoldierPose::Crouch, SoldierPose::Prone }) {
		air.poses.push_back(speed(p, 6, 1, 0.5f, 0.5f, 0.1f, 0, 0.1f, 1.5f));
	}
	air.scalars = { { "FreeFallVelocity", 16 } };
	gameplay::MovementStateDef falling;
	falling.type = "FallingStateData";
	for (SoldierPose p : { SoldierPose::Stand, SoldierPose::Crouch, SoldierPose::Prone }) {
		falling.poses.push_back(speed(p, 15, 1, 1, 1, 0.05f, 0, 0, 1));
	}
	d.states = { ground, jump, air, falling };
	d.fall_damage_vertical = { { 0, 0 }, { 13.9f, 0 }, { 14, 20 }, { 22, 40 }, { 30, 100 } };
	d.fall_damage_horizontal = d.fall_damage_vertical;
	return d;
}

std::string read_text(const char *p_path) {
	std::ifstream in(p_path, std::ios::binary);
	std::ostringstream s;
	s << in.rdbuf();
	return s.str();
}

MotorTuning shipped_tuning() {
	const std::string text = read_text(OPENBF3_MOTOR_TUNING_CFG);
	REQUIRE_MESSAGE(!text.empty(), OPENBF3_MOTOR_TUNING_CFG);
	core::Result<MotorTuning> t = parse_motor_tuning(text);
	REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
	return *t;
}

MotorParams synthetic_params() {
	core::Result<MotorParams> p = make_motor_params(synthetic_soldier(), shipped_tuning());
	REQUIRE_MESSAGE(p.ok(), (p.ok() ? std::string() : p.error().message));
	return *p;
}

std::shared_ptr<const SharedLevel> synthetic_level() {
	static std::shared_ptr<const SharedLevel> s_level;
	if (s_level) {
		return s_level;
	}
	std::vector<StaticTriangle> t;
	add_floor(t, -120, -120, 120, 120, 0);
	add_box(t, { 10, 0, 10 }, { 12, 2.5f, 14 });
	add_box(t, { -30, 0, 20 }, { -26, 1.0f, 24 });
	add_box(t, { 40, 0, -40 }, { 41, 3.0f, -30 });
	core::Result<std::shared_ptr<const StaticWorldShapes>> shapes = build_static_world(std::span<const StaticTriangle>(t));
	REQUIRE_MESSAGE(shapes.ok(), (shapes.ok() ? std::string() : shapes.error().message));
	std::vector<SpawnPoint> points;
	const Vec3 at[] = { { 0, 0.05f, 0 }, { 20, 0, 0 }, { -20, -0.1f, 0 }, { 0, 0.2f, 20 }, { 0, 0, -20 }, { 12.2f, 0, 12 }, { 60, 0, 60 },
		{ -60, 0, 60 }, { 60, 0.1f, -60 }, { -60, 0, -60 }, { 41.2f, 0, -35 }, { -80, 0, 0 } };
	for (size_t i = 0; i < std::size(at); ++i) {
		SpawnPoint p;
		p.position = at[i];
		p.yaw = uint16_t(i * 5461u);
		p.index = uint32_t(i);
		points.push_back(p);
	}
	s_level = synthetic_shared_level(*shapes, points, synthetic_params());
	return s_level;
}

float horizontal(const Vec3 &p_v) {
	return std::sqrt(p_v[0] * p_v[0] + p_v[2] * p_v[2]);
}

float dist(const Vec3 &p_a, const Vec3 &p_b) {
	const float dx = p_a[0] - p_b[0], dy = p_a[1] - p_b[1], dz = p_a[2] - p_b[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

using bf3::core::seconds_since;

RestSample rest_sample(const PresentationFrame &p_f) {
	RestSample r;
	r.time_ns = p_f.publish_ns;
	r.tick = p_f.tick;
	r.input_idle = p_f.own_command.move_forward == 0 && p_f.own_command.move_right == 0 && p_f.own_command.buttons == 0;
	r.horizontal_speed = horizontal(p_f.own_curr.velocity);
	const float t = float(p_f.tick_fraction);
	for (size_t k = 0; k < 3; ++k) {
		r.eye[k] = p_f.own_prev.position[k] + (p_f.own_curr.position[k] - p_f.own_prev.position[k]) * t + p_f.visual_offset[k];
	}
	r.eye[1] += p_f.eye_height_prev + (p_f.eye_height_curr - p_f.eye_height_prev) * t;
	r.visual_offset_m = std::sqrt(p_f.visual_offset[0] * p_f.visual_offset[0] + p_f.visual_offset[1] * p_f.visual_offset[1] +
			p_f.visual_offset[2] * p_f.visual_offset[2]);
	r.reconciliations = p_f.stats.reconciliations;
	r.last_reconcile_tick = p_f.stats.last_reconcile_tick;
	return r;
}

struct LiveOptions {
	uint32_t walkers = 0;
	std::string netsim = "off";
	int32_t clock_offset = 0;
	uint32_t max_slots = uint32_t(net::kMaxSlots);
	bool dev_commands = true;
	uint64_t seed = 1;
};

struct Live {
	std::shared_ptr<const SharedLevel> level;
	ServerHost server;
	ClientSession client;
	InputSample input;
	std::unique_ptr<PresentationFrame> frame = std::make_unique<PresentationFrame>();
	bool adopt = true;
	float adopt_yaw = NAN;
};

std::unique_ptr<Live> start_live(std::shared_ptr<const SharedLevel> p_level, const LiveOptions &p_options) {
	auto live = std::make_unique<Live>();
	live->level = p_level;
	auto game = std::make_shared<GameServerConfig>();
	game->level = p_level;
	game->max_slots = p_options.max_slots;
	game->walkers = p_options.walkers;
	game->dev_commands = p_options.dev_commands;
	game->seed = p_options.seed;
	ServerConfig sc;
	sc.game = game;
	std::string err;
	REQUIRE_MESSAGE(live->server.start(sc, &err), err);
	ClientSessionConfig cc;
	cc.port = live->server.port();
	core::Result<net::NetSimConfig> ns = net::parse_netsim(p_options.netsim);
	REQUIRE_MESSAGE(ns.ok(), (ns.ok() ? std::string() : ns.error().message));
	cc.netsim = *ns;
	cc.netsim.seed = p_options.seed * 7919u + 1u;
	cc.request_dev_commands = true;
	cc.debug_hash_compare = true;
	cc.initial_clock_offset_ticks = p_options.clock_offset;
	const core::Status s = live->client.start(cc, p_level);
	REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
	return live;
}

bool poll(Live &r_live) {
	const bool fresh = r_live.client.read_presentation(*r_live.frame);
	const PresentationFrame &f = *r_live.frame;
	if (r_live.adopt && f.valid && f.own_curr.life > r_live.input.life) {
		r_live.input.life = f.own_curr.life;
		r_live.input.yaw = std::isnan(r_live.adopt_yaw) ? yaw_radians(f.own_curr.yaw) : r_live.adopt_yaw;
		r_live.input.pitch = pitch_radians(f.own_curr.pitch);
	}
	r_live.client.set_input(r_live.input);
	return fresh;
}

void run_for(Live &r_live, double p_seconds, const std::function<void(bool, uint64_t, double)> &p_each = {}, int p_sleep_ms = 4) {
	const auto start = std::chrono::steady_clock::now();
	for (;;) {
		const double t = seconds_since(start);
		if (t >= p_seconds) {
			break;
		}
		const bool fresh = poll(r_live);
		if (p_each) {
			p_each(fresh, net::monotonic_ns(), t);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(p_sleep_ms));
	}
}

bool wait_spawned(Live &r_live, double p_timeout_s = 5.0) {
	const auto start = std::chrono::steady_clock::now();
	while (seconds_since(start) < p_timeout_s) {
		poll(r_live);
		if (r_live.frame->valid && r_live.frame->own_curr.life > 0 && r_live.input.life == r_live.frame->own_curr.life) {
			return true;
		}
		std::this_thread::sleep_for(4ms);
	}
	return false;
}

std::optional<net::DebugReport> wait_report(Live &r_live, uint32_t p_id, double p_timeout_s = 10.0) {
	const auto start = std::chrono::steady_clock::now();
	while (seconds_since(start) < p_timeout_s) {
		poll(r_live);
		net::DebugReport r;
		while (r_live.client.take_debug_report(r)) {
			if (r.id == p_id) {
				return r;
			}
		}
		std::this_thread::sleep_for(4ms);
	}
	return std::nullopt;
}

const SlotStats *own_slot(const GameServerStats &p_stats) {
	for (const SlotStats &s : p_stats.slots) {
		if (s.used && s.remote) {
			return &s;
		}
	}
	return nullptr;
}

struct WalkerTrack {
	std::map<uint8_t, std::map<Tick, net::OtherSoldier>> per_slot;
	void sample(const GameServerStats &p_stats) {
		for (size_t i = 0; i < p_stats.slots.size(); ++i) {
			const SlotStats &s = p_stats.slots[i];
			if (s.used && !s.remote && s.alive) {
				per_slot[uint8_t(i)][s.state.tick] = net::quantize_other(uint8_t(i), s.state);
			}
		}
	}

	std::optional<Vec3> at(uint8_t p_slot, double p_tick) const {
		auto s = per_slot.find(p_slot);
		if (s == per_slot.end() || p_tick < 0) {
			return std::nullopt;
		}
		const Tick a = Tick(std::floor(p_tick));
		auto ia = s->second.find(a), ib = s->second.find(a + 1);
		if (ia == s->second.end() || ib == s->second.end()) {
			return std::nullopt;
		}
		const float u = float(p_tick - double(a));
		Vec3 out;
		for (size_t k = 0; k < 3; ++k) {
			out[k] = ia->second.position[k] + (ib->second.position[k] - ia->second.position[k]) * u;
		}
		return out;
	}
};

struct InterpCheck {
	uint64_t presented = 0;
	uint64_t extrapolated = 0;
	uint64_t compared = 0;
	float max_error = 0;
	std::vector<float> errors;
	void frame(Live &r_live, const WalkerTrack *p_track, uint64_t p_now) {
		const PresentationFrame &f = *r_live.frame;
		if (f.other_count == 0) {
			return;
		}
		std::array<PresentedSoldier, net::kMaxSlots> out{};
		uint64_t extra = 0;
		const size_t n = present_others(f, p_now, out, &extra);
		presented += n;
		extrapolated += extra;
		if (p_track == nullptr) {
			return;
		}
		const double render = f.render_ticks_at_publish + double(p_now - f.publish_ns) * f.ticks_per_ns;
		for (size_t i = 0; i < n; ++i) {
			if (out[i].extrapolated) {
				continue;
			}
			const std::optional<Vec3> truth = p_track->at(out[i].slot, render);
			if (!truth) {
				continue;
			}
			const float e = dist(*truth, out[i].position);
			++compared;
			max_error = std::max(max_error, e);
			errors.push_back(e);
		}
	}
	float p99() const {
		if (errors.empty()) {
			return 0;
		}
		std::vector<float> s = errors;
		std::sort(s.begin(), s.end());
		return s[std::min(s.size() - 1, (s.size() * 99) / 100)];
	}
};

struct E2Run {
	std::vector<RestSample> samples;
	std::vector<RestCycleResult> cycles;
	ClientStats stats;
	InterpCheck interp;
	double rtt_sum = 0;
	size_t rtt_n = 0;
	std::set<uint32_t> targets;
	uint32_t target_changes = 0;
	double buffer_sum = 0;
	size_t buffer_n = 0;
	uint64_t server_precondition_failures = 0;
};

E2Run run_e2(Live &r_live, double p_idle_s, int p_cycles, double p_walk_s, double p_rest_s, float p_heading, bool p_track_walkers) {
	E2Run run;
	WalkerTrack track;
	uint32_t last_target = 0;
	const double total = p_idle_s + double(p_cycles) * (p_walk_s + p_rest_s);
	r_live.input.move_forward = 0;
	r_live.input.yaw = p_heading;
	run_for(r_live, total, [&](bool p_fresh, uint64_t p_now, double p_t) {

		float forward = 0;
		if (p_t >= p_idle_s) {
			const double in_cycle = std::fmod(p_t - p_idle_s, p_walk_s + p_rest_s);
			const int k = int((p_t - p_idle_s) / (p_walk_s + p_rest_s));
			if (in_cycle < p_walk_s && k < p_cycles) {
				forward = (k % 2 == 0) ? 1.0f : -1.0f;
			}
		}
		r_live.input.move_forward = forward;
		r_live.input.yaw = p_heading;
		if (p_track_walkers) {
			track.sample(r_live.server.game_stats());
		}
		const PresentationFrame &f = *r_live.frame;
		run.interp.frame(r_live, p_track_walkers ? &track : nullptr, p_now);
		if (!p_fresh || !f.valid) {
			return;
		}
		run.samples.push_back(rest_sample(f));
		if (p_t > 5.0) {
			if (f.stats.rtt_ms > 0) {
				run.rtt_sum += f.stats.rtt_ms;
				++run.rtt_n;
			}
			run.buffer_sum += f.stats.command_buffer_ticks;
			++run.buffer_n;
			run.targets.insert(f.stats.target_buffer_ticks);
			if (last_target != 0 && f.stats.target_buffer_ticks != last_target) {
				++run.target_changes;
			}
			last_target = f.stats.target_buffer_ticks;
		}
	});
	run.stats = r_live.client.stats();
	run.server_precondition_failures = r_live.server.game_stats().step_precondition_failures;
	run.cycles = judge_rest(run.samples);
	return run;
}

void print_e2(const char *p_name, const E2Run &p_run) {
	const ClientStats &s = p_run.stats;
	std::printf("E2 %s: %zu samples, %zu rest periods; reconciliations %llu (corrections > 1 mm %llu, max %.4f m, teleports %llu), ring misses %llu, "
				"base fills %llu, clock resets %llu (forced %llu), precondition failures %llu client / %llu server, hash mismatches %llu\n",
			p_name, p_run.samples.size(), p_run.cycles.size(), (unsigned long long)s.reconciliations, (unsigned long long)s.corrections_over_1mm,
			double(s.max_correction_m), (unsigned long long)s.teleports, (unsigned long long)s.ring_misses, (unsigned long long)s.base_fills,
			(unsigned long long)s.clock_resets, (unsigned long long)s.forced_forward, (unsigned long long)s.step_precondition_failures,
			(unsigned long long)p_run.server_precondition_failures, (unsigned long long)s.hash_mismatches);
	std::string targets;
	for (uint32_t t : p_run.targets) {
		targets += (targets.empty() ? "" : ",") + std::to_string(t);
	}
	std::printf("E2 %s: RTT mean %.2f ms (min %.2f), netsim one-way means %.2f + %.2f ms, loss out %llu/%llu in %llu/%llu; buffer mean %.2f, targets {%s}, "
				"%u target changes after 5 s; interp delay %.1f ms; others %llu presented, %llu extrapolated (%.2f %%), track error max %.4f m p99 %.4f m "
				"(%llu compared)\n",
			p_name, p_run.rtt_n ? p_run.rtt_sum / double(p_run.rtt_n) : 0.0, s.rtt_min_ms, s.netsim_out.mean_delay_ms, s.netsim_in.mean_delay_ms,
			(unsigned long long)s.netsim_out.dropped, (unsigned long long)s.netsim_out.submitted, (unsigned long long)s.netsim_in.dropped,
			(unsigned long long)s.netsim_in.submitted, p_run.buffer_n ? p_run.buffer_sum / double(p_run.buffer_n) : 0.0, targets.c_str(), p_run.target_changes,
			s.interp_delay_ms, (unsigned long long)p_run.interp.presented, (unsigned long long)p_run.interp.extrapolated,
			p_run.interp.presented ? 100.0 * double(p_run.interp.extrapolated) / double(p_run.interp.presented) : 0.0, double(p_run.interp.max_error),
			double(p_run.interp.p99()), (unsigned long long)p_run.interp.compared);
	for (size_t i = 0; i < p_run.cycles.size(); ++i) {
		const RestCycleResult &c = p_run.cycles[i];
		std::printf("  period %zu: %s stop tick %u, rest %.3f s after the stop, window %.2f..%.2f s (period %.2f s)%s, edge corrections %u, "
					"window reconciliations %u, eye jitter %.7f m, max offset %.6f m%s%s\n",
				i, c.ok ? "ok  " : "FAIL", c.stop_tick, double(c.rest_ns - c.stop_ns) / 1e9, double(c.window_start_ns - c.stop_ns) / 1e9,
				double(c.window_end_ns - c.stop_ns) / 1e9, double(c.period_end_ns - c.stop_ns) / 1e9, c.pushed ? " pushed" : "", c.edge_corrections,
				c.window_reconciliations, double(c.eye_jitter_m), double(c.max_visual_offset_m), c.ok ? "" : ": ", c.ok ? "" : c.failure.data());
	}
}

struct Direct {
	GameServer server;
	std::vector<OutDatagram> out;
	std::vector<net::Spawned> spawned;
	std::vector<net::Snapshot> snapshots;
	std::vector<net::DebugReport> reports;
	std::optional<net::MatchInfo> match;

	void tick() {
		out.clear();
		server.tick(out);
		for (const OutDatagram &d : out) {
			const std::optional<net::MessageType> t = net::peek_type(d.bytes);
			REQUIRE(t.has_value());
			if (*t == net::MessageType::Spawned) {
				core::Result<net::Spawned> s = net::decode_spawned(d.bytes);
				REQUIRE(s.ok());
				spawned.push_back(*s);
			} else if (*t == net::MessageType::Snapshot) {
				core::Result<net::Snapshot> s = net::decode_snapshot(d.bytes);
				REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
				snapshots.push_back(std::move(*s));
				CHECK(d.channel == net::kChannelUnreliable);
				CHECK_FALSE(d.reliable);
			} else if (*t == net::MessageType::DebugReport) {
				core::Result<net::DebugReport> r = net::decode_debug_report(d.bytes);
				REQUIRE(r.ok());
				reports.push_back(std::move(*r));
			} else if (*t == net::MessageType::MatchInfo) {
				core::Result<net::MatchInfo> m = net::decode_match_info(d.bytes);
				REQUIRE(m.ok());
				match = *m;
			}
		}
	}
	void receive(const std::vector<uint8_t> &p_bytes, uint8_t p_channel = net::kChannelReliable) { server.on_receive(0, p_channel, p_bytes); }
	void commands(std::initializer_list<Command> p_newest_first) {
		net::CommandPacket p;
		p.count = uint8_t(p_newest_first.size());
		size_t i = 0;
		for (const Command &c : p_newest_first) {
			p.commands[i++] = c;
		}
		receive(net::encode_command_packet(p), net::kChannelUnreliable);
	}
};

Command walk(Tick p_tick, uint32_t p_sequence, int8_t p_forward = kAxisMax) {
	Command c;
	c.tick = p_tick;
	c.sequence = p_sequence;
	c.move_forward = p_forward;
	return c;
}

}

TEST_SUITE("host-session-unit") {
	TEST_CASE("host.session: judge_rest on synthetic recordings (edge push, late correction, window fit, creep, eye wobble)") {

		auto recording = [](double p_rest_s, double p_decel_s) {
			std::vector<RestSample> s;
			const uint64_t step = 16'666'667ull;
			const int walk = 120;
			const int rest = int(p_rest_s * 60.0);
			for (int i = 0; i < walk + rest; ++i) {
				RestSample r;
				r.time_ns = uint64_t(i) * step + 1'000'000'000ull;
				r.tick = Tick(1000 + i);
				r.input_idle = i >= walk;
				const double since_stop = double(i - walk) / 60.0;
				r.horizontal_speed = i < walk ? 4.0f : (since_stop >= p_decel_s ? 0.0f : float(4.0 * (1.0 - since_stop / p_decel_s)));
				r.eye = { 1.0f, 1.6f, 2.0f };
				s.push_back(r);
			}
			return s;
		};
		const RestCheckOptions o;
		{
			const std::vector<RestSample> s = recording(13.0, 0.27);
			const std::vector<RestCycleResult> r = judge_rest(s, o);
			REQUIRE(r.size() == 1);
			CHECK_MESSAGE(r[0].ok, r[0].failure.data());
			CHECK(r[0].stop_tick == 1120);
			CHECK_FALSE(r[0].pushed);
		}
		auto with_reconciliation = [](std::vector<RestSample> p_s, size_t p_at, Tick p_tick) {
			for (size_t i = p_at; i < p_s.size(); ++i) {
				p_s[i].reconciliations = 1;
				p_s[i].last_reconcile_tick = p_tick;
			}
			return p_s;
		};
		{

			const std::vector<RestSample> s = with_reconciliation(recording(13.0, 0.27), 120 + 18, 1120 + 3);
			const std::vector<RestCycleResult> r = judge_rest(s, o);
			REQUIRE(r.size() == 1);
			CHECK_MESSAGE(r[0].ok, r[0].failure.data());
			CHECK(r[0].pushed);
			CHECK(r[0].edge_corrections == 1);
			CHECK(r[0].window_start_ns == s[138].time_ns + 500'000'000ull);
		}
		{

			const std::vector<RestSample> s = with_reconciliation(recording(13.0, 0.27), 120 + 30, 1120 + 11);
			const std::vector<RestCycleResult> r = judge_rest(s, o);
			REQUIRE(r.size() == 1);
			CHECK_FALSE(r[0].ok);
			CHECK(std::string(r[0].failure.data()).find("at rest") != std::string::npos);
		}
		{

			const std::vector<RestSample> s = with_reconciliation(recording(13.0, 0.27), 120 + 30, 1120 + 10);
			CHECK(judge_rest(s, o)[0].ok);
		}
		{

			const std::vector<RestSample> s = with_reconciliation(recording(11.0, 0.27), 120 + 60, 1120 + 2);
			const std::vector<RestCycleResult> r = judge_rest(s, o);
			REQUIRE(r.size() == 1);
			CHECK_FALSE(r[0].ok);
			CHECK(std::string(r[0].failure.data()).find("does not fit") != std::string::npos);
		}
		{

			const std::vector<RestCycleResult> r = judge_rest(recording(13.0, 1.2), o);
			REQUIRE(r.size() == 1);
			CHECK_FALSE(r[0].ok);
			CHECK(std::string(r[0].failure.data()).find("creep") != std::string::npos);
		}
		{

			for (const float wobble : { 2e-4f, 5e-5f }) {
				std::vector<RestSample> s = recording(13.0, 0.27);
				s[120 + 300].eye[0] += wobble;
				const std::vector<RestCycleResult> r = judge_rest(s, o);
				REQUIRE(r.size() == 1);
				CHECK(r[0].ok == (wobble < 1e-4f));
				CHECK(r[0].eye_jitter_m == doctest::Approx(wobble).epsilon(0.01));
			}
		}
		{

			std::vector<RestSample> s = recording(13.0, 0.27);
			s[120 + 400].visual_offset_m = 1e-6f;
			CHECK_FALSE(judge_rest(s, o)[0].ok);
			std::vector<RestSample> two = recording(13.0, 0.27);
			std::vector<RestSample> more = recording(13.0, 0.27);
			for (RestSample &r : more) {
				r.time_ns += two.back().time_ns + 16'666'667ull - 1'000'000'000ull + 16'666'667ull;
				r.tick += 2000;
			}
			two.insert(two.end(), more.begin(), more.end());
			const std::vector<RestCycleResult> r2 = judge_rest(two, o);
			REQUIRE(r2.size() == 2);
			CHECK(r2[0].ok);
			CHECK(r2[1].ok);
			CHECK(judge_rest({}, o).empty());
		}
	}

	TEST_CASE("host.session: present_others brackets, extrapolates <= 250 ms and holds; present_own_fraction advances and clamps") {
		PresentationFrame f;
		f.publish_ns = 1'000'000'000ull;
		f.render_ticks_at_publish = 101.0;
		f.ticks_per_ns = 60e-9;
		f.extrapolate_max_ms = 250.0f;
		f.other_count = 1;
		OtherTrack &t = f.others[0];
		t.slot = 5;
		t.count = 3;
		for (uint32_t i = 0; i < 3; ++i) {
			net::OtherSoldier o;
			o.slot = 5;
			o.position = { 4.0f * float(i) * (2.0f / 60.0f), 0.0f, 0.0f };
			o.velocity = { 4.0f, 0.0f, 0.0f };
			o.yaw = uint16_t(65000 + i * 300);
			o.pose = i == 2 ? Pose::Crouch : Pose::Stand;
			o.target_pose = o.pose;
			t.samples[i].server_tick = 100 + 2 * i;
			t.samples[i].soldier = o;
		}
		std::array<PresentedSoldier, 4> out{};
		uint64_t extra = 0;

		REQUIRE(present_others(f, f.publish_ns, out, &extra) == 1);
		CHECK(out[0].slot == 5);
		CHECK(out[0].position[0] == doctest::Approx(4.0f / 60.0f).epsilon(1e-4));
		CHECK_FALSE(out[0].extrapolated);
		CHECK(extra == 0);
		const float yaw_mid = yaw_radians(uint16_t(65000 + 150));
		CHECK(out[0].yaw == doctest::Approx(yaw_mid).epsilon(1e-4));
		CHECK(out[0].pose == Pose::Stand);

		REQUIRE(present_others(f, f.publish_ns + 50'000'000ull, out, &extra) == 1);
		CHECK(out[0].position[0] == doctest::Approx(4.0f * 4.0f / 60.0f).epsilon(1e-4));
		CHECK(out[0].pose == Pose::Crouch);

		const uint64_t at_200ms = f.publish_ns + uint64_t((3.0 + 12.0) / 60.0 * 1e9);
		REQUIRE(present_others(f, at_200ms, out, &extra) == 1);
		CHECK(out[0].extrapolated);
		CHECK(extra == 1);
		CHECK(out[0].position[0] == doctest::Approx(4.0f * 4.0f / 60.0f + 4.0f * 0.2f).epsilon(1e-3));
		const uint64_t at_2s = f.publish_ns + 2'000'000'000ull;
		REQUIRE(present_others(f, at_2s, out, &extra) == 1);
		CHECK(out[0].position[0] == doctest::Approx(4.0f * 4.0f / 60.0f + 4.0f * 0.25f).epsilon(1e-3));
		CHECK(extra == 2);

		REQUIRE(present_others(f, f.publish_ns - 100'000'000ull, out, nullptr) == 1);
		CHECK(out[0].position[0] == 0.0f);

		CHECK(present_others(f, f.publish_ns, std::span<PresentedSoldier>(out.data(), 0), nullptr) == 0);

		PresentationFrame g;
		g.publish_ns = 5'000'000'000ull;
		g.tick_fraction = 0.25;
		g.own_ticks_per_ns = 60e-9;
		CHECK(present_own_fraction(g, g.publish_ns) == doctest::Approx(0.25));
		CHECK(present_own_fraction(g, g.publish_ns + 8'333'333ull) == doctest::Approx(0.75).epsilon(1e-4));
		CHECK(present_own_fraction(g, g.publish_ns + 100'000'000ull) == 1.0);
		CHECK(present_own_fraction(g, g.publish_ns - 100'000'000ull) == doctest::Approx(0.25));
	}

	TEST_CASE("host.session: GameServer without sockets: hold command, jitter buffer, buffer report, Teleport / SpawnAt / Trace, refused and hostile input") {
		auto level = synthetic_level();
		Direct d;
		GameServerConfig cfg;
		cfg.level = level;
		cfg.max_slots = 8;
		cfg.dev_commands = true;
		REQUIRE(d.server.init(cfg).ok());
		d.server.on_connect(0);
		net::Hello hello;
		hello.name = "test";
		d.receive(net::encode_hello(hello));
		d.tick();
		REQUIRE(d.match.has_value());
		CHECK(d.match->slot == 0);
		CHECK(d.match->server_tick == 0);
		CHECK(d.match->motor_hash == motor_params_hash(level->motor));
		d.receive(net::encode_ready({}));
		d.tick();
		REQUIRE(d.spawned.size() == 1);
		const SoldierState spawn = d.spawned[0].state;
		CHECK(spawn.tick == 2);
		CHECK(spawn.life == 1);
		CHECK((spawn.events & soldier_events::kSpawned) != 0);
		CHECK(spawn.protect_ticks == level->motor.immortal_ticks);
		CHECK(d.snapshots.back().server_tick == 2);
		CHECK(d.snapshots.back().own->tick == 2);

		for (int i = 0; i < 4; ++i) {
			d.tick();
		}
		const SoldierState *s = d.server.soldier(0);
		REQUIRE(s != nullptr);
		CHECK(s->tick == 6);
		CHECK(s->yaw == spawn.yaw);
		CHECK(horizontal(s->velocity) == 0.0f);
		CHECK(d.snapshots.back().applied_repeated);
		CHECK(d.snapshots.back().applied_sequence == 0);

		{
			auto world = CollisionWorld::create(level->static_world);
			REQUIRE(world.ok());
			SoldierMotor m;
			REQUIRE(m.init(**world, level->motor).ok());
			PredictionRing ring(128);
			catch_up(m, spawn, ring, 7);
			REQUIRE(ring.find(6) != nullptr);
			CHECK(ring.find(6)->state == *s);
		}
		GameServerStats g = d.server.stats();
		CHECK(g.slots[0].commands_repeated == 4);

		d.commands({ walk(10, 2), walk(9, 1) });
		d.commands({ walk(10, 2), walk(9, 1) });
		g = d.server.stats();
		CHECK(g.slots[0].buffer_ticks == 10 - 6);
		CHECK(g.slots[0].commands_replaced == 0);

		d.commands({ walk(10, 5, 64) });
		d.commands({ walk(10, 5, 64) });
		d.commands({ walk(10, 3, 32) });
		g = d.server.stats();
		CHECK(g.slots[0].commands_replaced == 1);

		d.commands({ walk(8, 6) });
		g = d.server.stats();
		CHECK(g.slots[0].buffer_ticks == 8 - 6);
		d.tick();
		d.tick();
		CHECK(d.server.soldier(0)->tick == 8);
		CHECK(d.snapshots.back().applied_sequence == 6);
		CHECK_FALSE(d.snapshots.back().applied_repeated);
		CHECK(d.snapshots.back().command_buffer_ticks == 0);
		d.tick();
		d.tick();
		g = d.server.stats();
		CHECK(g.slots[0].commands_applied == 3);
		CHECK(d.snapshots.back().applied_sequence == 5);
		CHECK(d.snapshots.back().sequence_echo == 6);

		d.commands({ walk(9, 7) });
		d.commands({ walk(9, 7) });
		d.commands({ walk(10, 5, 64) });
		g = d.server.stats();
		CHECK(g.slots[0].commands_late == 1);

		d.commands({ walk(10 + 33, 8) });
		g = d.server.stats();
		CHECK(g.slots[0].buffer_ticks == 43 - 10);

		net::DebugRequest tp;
		tp.kind = net::DebugKind::Teleport;
		tp.id = 11;
		tp.position = { 5.0f, 3.0f, -5.0f };
		tp.yaw = 1234;
		d.receive(net::encode_debug(tp));
		d.tick();
		s = d.server.soldier(0);
		CHECK(s->tick == 11);
		CHECK(s->position == Vec3{ 5.0f, 3.0f, -5.0f });
		CHECK(s->life == 1);
		CHECK(s->protect_ticks == 0);
		REQUIRE(d.reports.size() == 1);
		CHECK(d.reports[0].id == 11);
		CHECK(d.reports[0].ok);
		net::DebugRequest at;
		at.kind = net::DebugKind::SpawnAt;
		at.id = 12;
		at.index = 3;
		d.receive(net::encode_debug(at));
		d.tick();
		REQUIRE(d.spawned.size() == 2);
		CHECK(d.spawned[1].state.tick == 12);
		CHECK(d.spawned[1].state.life == 2);
		CHECK(d.spawned[1].spawn_index == 3);
		at.index = 999;
		at.id = 13;
		d.receive(net::encode_debug(at));
		d.tick();
		CHECK_FALSE(d.reports.back().ok);

		net::DebugRequest tr;
		tr.kind = net::DebugKind::Trace;
		tr.id = 14;
		tr.ticks = 10;
		d.receive(net::encode_debug(tr));
		std::vector<SoldierState> seen;
		for (int i = 0; i < 10; ++i) {
			d.commands({ walk(d.server.current_tick() + 1, 100 + i) });
			d.tick();
			seen.push_back(*d.server.soldier(0));
		}
		REQUIRE(d.reports.back().id == 14);
		REQUIRE(d.reports.back().trace.size() == 10);
		for (size_t i = 0; i < 10; ++i) {
			CHECK(d.reports.back().trace[i] == seen[i]);
		}
		tr.ticks = net::kMaxTraceTicks + 1;
		d.receive(net::encode_debug(tr));
		g = d.server.stats();
		CHECK(g.bad_datagrams == 1);
		CHECK(g.step_precondition_failures == 0);

		Direct off;
		GameServerConfig ocfg = cfg;
		ocfg.dev_commands = false;
		REQUIRE(off.server.init(ocfg).ok());
		off.server.on_connect(0);
		off.receive(net::encode_hello(hello));
		off.receive(net::encode_ready({}));
		off.tick();
		off.receive(net::encode_debug(tp));
		off.tick();
		REQUIRE(off.reports.size() == 1);
		CHECK_FALSE(off.reports[0].ok);
		CHECK(off.server.soldier(0)->position != tp.position);

		std::mt19937 rng(7);
		std::vector<std::vector<uint8_t>> seeds = { net::encode_hello(hello), net::encode_ready({}), net::encode_debug(tp), net::encode_debug(tr),
			net::encode_bye(), net::encode_loading({}) };
		{
			net::CommandPacket p;
			p.count = 4;
			for (int i = 0; i < 4; ++i) {
				p.commands[size_t(i)] = walk(Tick(200 - i), uint32_t(300 - i));
			}
			seeds.push_back(net::encode_command_packet(p));
		}
		Direct fuzz;
		REQUIRE(fuzz.server.init(cfg).ok());
		fuzz.server.on_connect(0);
		fuzz.receive(net::encode_hello(hello));
		fuzz.receive(net::encode_ready({}));
		for (int i = 0; i < 20000; ++i) {
			std::vector<uint8_t> b;
			if (i % 4 == 0) {
				b.resize(rng() % 64);
				for (uint8_t &x : b) {
					x = uint8_t(rng());
				}
			} else {
				b = seeds[rng() % seeds.size()];
				const int flips = 1 + int(rng() % 4);
				for (int k = 0; k < flips && !b.empty(); ++k) {
					b[rng() % b.size()] ^= uint8_t(1u << (rng() % 8));
				}
				if (rng() % 8 == 0 && !b.empty()) {
					b.resize(rng() % b.size());
				}
			}
			fuzz.server.on_receive(uint32_t(rng() % 3), uint8_t(rng() % 3), b);
			if (i % 50 == 0) {
				fuzz.out.clear();
				fuzz.server.tick(fuzz.out);
			}
		}
		g = fuzz.server.stats();
		std::printf("GameServer fuzz: %llu bad datagrams of 20000, tick %u, precondition failures %llu\n", (unsigned long long)g.bad_datagrams, g.tick,
				(unsigned long long)g.step_precondition_failures);
		CHECK(g.bad_datagrams > 1000);
		CHECK(g.step_precondition_failures == 0);
	}

	TEST_CASE("host.session: GameServer without sockets: a Ready with another level's source digest is kicked and its slot freed, the same or no digest is admitted") {
		auto digested = std::make_shared<SharedLevel>(*synthetic_level());
		digested->source_digest = core::Sha256Digest{};
		digested->source_digest->bytes[3] = 0x5D;
		core::Sha256Digest other = *digested->source_digest;
		other.bytes[3] = 0x5E;
		GameServerConfig cfg;
		cfg.level = digested;
		cfg.max_slots = 8;
		net::Hello hello;
		hello.name = "test";

		const auto run = [&](std::optional<core::Sha256Digest> p_digest, size_t &r_kicks, size_t &r_spawned) {
			Direct d;
			REQUIRE(d.server.init(cfg).ok());
			d.server.on_connect(0);
			d.receive(net::encode_hello(hello));
			d.tick();
			REQUIRE(d.server.stats().clients == 1);
			d.receive(net::encode_ready({ p_digest }));
			r_kicks = 0;
			for (int i = 0; i < 4; ++i) {
				d.tick();
				for (const OutDatagram &o : d.out) {
					if (net::peek_type(o.bytes) == net::MessageType::Kick) {
						core::Result<net::Kick> k = net::decode_kick(o.bytes);
						REQUIRE(k.ok());
						CHECK(k->reason.find("source digest") != std::string::npos);
						CHECK(o.peer == 0);
						CHECK(o.reliable);
						++r_kicks;
					}
				}
			}
			r_spawned = d.spawned.size();
			return d.server.stats();
		};

		size_t kicks = 0, spawned = 0;
		GameServerStats g = run(other, kicks, spawned);
		CHECK(kicks == 1);
		CHECK(spawned == 0);
		CHECK(g.level_mismatch_kicks == 1);
		CHECK(g.clients == 0);
		CHECK(g.soldiers == 0);
		CHECK(g.bad_datagrams == 0);

		g = run(digested->source_digest, kicks, spawned);
		CHECK(kicks == 0);
		CHECK(spawned == 1);
		CHECK(g.level_mismatch_kicks == 0);
		CHECK(g.clients == 1);

		g = run(std::nullopt, kicks, spawned);
		CHECK(kicks == 0);
		CHECK(spawned == 1);
		CHECK(g.level_mismatch_kicks == 0);
		CHECK(g.clients == 1);
	}

	TEST_CASE("host.session: GameServer SpawnSweep in batches of the free slots equals headless place_spawn + idle ticks + judge_settle") {
		auto level = synthetic_level();
		Direct d;
		GameServerConfig cfg;
		cfg.level = level;
		cfg.max_slots = 5;
		cfg.dev_commands = true;
		REQUIRE(d.server.init(cfg).ok());
		d.server.on_connect(0);
		d.receive(net::encode_hello({}));
		d.receive(net::encode_ready({}));
		d.tick();
		net::DebugRequest q;
		q.kind = net::DebugKind::SpawnSweep;
		q.id = 21;
		q.ticks = 60;
		d.receive(net::encode_debug(q));
		for (int i = 0; i < 400 && d.reports.empty(); ++i) {
			d.tick();
		}
		REQUIRE(d.reports.size() == 1);
		const net::DebugReport &r = d.reports[0];
		REQUIRE(r.sweep.size() == level->spawns.size());
		auto world = CollisionWorld::create(level->static_world);
		REQUIRE(world.ok());
		SoldierMotor m;
		REQUIRE(m.init(**world, level->motor).ok());
		size_t ok = 0;
		for (size_t i = 0; i < r.sweep.size(); ++i) {
			const net::SpawnSweepRow &row = r.sweep[i];
			const SpawnPoint &p = level->spawns[i];
			CHECK(row.index == p.index);
			SettleResult h = place_spawn(m, **world, p, 1, 0);
			REQUIRE(h.failure.empty());
			for (uint32_t t = 0; t < 60; ++t) {
				const SoldierState &st = h.state;
				m.step(h.state, idle_command(st.tick + 1u, st.yaw, st.pitch, st.target_pose));
			}
			judge_settle(m, **world, p, SettleOptions(), h);
			CHECK(row.ok == h.ok);
			CHECK(row.settled == h.state.position);
			CHECK(row.lift == h.lift);
			CHECK(row.shift == h.shift);
			CHECK(row.gap == h.gap);
			CHECK(row.grounded == ((h.state.flags & soldier_flags::kGrounded) != 0));
			CHECK(row.overlapping == h.overlapping);
			ok += row.ok ? 1 : 0;
		}
		std::printf("synthetic SpawnSweep: %zu / %zu ok (%s)\n", ok, r.sweep.size(), r.text.c_str());
		CHECK(ok == r.sweep.size());

		const GameServerStats g = d.server.stats();
		size_t used = 0;
		for (const SlotStats &s : g.slots) {
			used += s.used ? 1 : 0;
		}
		CHECK(used == 1);
	}

	TEST_CASE("host.session: loopback: spawn without reconciliation, walk, stop; the clock target 2; RTT bound; Trace; events; the spawn view; walkers") {
		auto live = start_live(synthetic_level(), { .walkers = 2 });

		live->adopt = false;
		live->input.yaw = 1.0f;
		run_for(*live, 1.0);
		const PresentationFrame &f = *live->frame;
		REQUIRE(f.valid);
		const uint16_t spawn_yaw = f.own_curr.yaw;
		CHECK(f.own_command.yaw == spawn_yaw);
		CHECK(f.stats.reconciliations == 0);
		CHECK(f.stats.spawns == 1);
		live->adopt = true;
		live->adopt_yaw = yaw_radians(spawn_yaw) + 0.5f;
		run_for(*live, 1.0);
		CHECK(live->frame->own_curr.yaw == quantize_yaw(yaw_radians(spawn_yaw) + 0.5f));
		CHECK(live->frame->stats.reconciliations == 0);

		WalkerTrack track;
		InterpCheck interp;
		net::DebugRequest tr;
		tr.kind = net::DebugKind::Trace;
		tr.id = 31;
		tr.ticks = 240;
		live->client.send_debug(tr);
		std::map<Tick, SoldierState> server_own, predicted;
		uint16_t seen_events = 0;
		double rtt_sum = 0;
		size_t rtt_n = 0;
		uint64_t last_serial = 0, missed_frames = 0;
		run_for(*live, 4.5, [&](bool p_fresh, uint64_t p_now, double p_t) {
			live->input.move_forward = p_t < 1.5 ? 1.0f : 0.0f;
			if (p_t > 0.5 && p_t < 0.6) {
				live->input.jump_presses = 1;
			}
			live->input.pose_request = PoseRequest::Set;
			live->input.pose = (p_t > 2.0 && p_t < 3.0) ? Pose::Crouch : Pose::Stand;
			track.sample(live->server.game_stats());
			interp.frame(*live, &track, p_now);
			const PresentationFrame &g = *live->frame;
			if (p_fresh && g.serial != last_serial + 1 && last_serial != 0) {
				missed_frames += g.serial - last_serial - 1;
			}
			if (p_fresh) {
				last_serial = g.serial;
			}
			if (p_fresh && g.valid) {
				if (g.server_own_valid) {
					server_own[g.server_own.tick] = g.server_own;
				}
				predicted[g.own_curr.tick] = g.own_curr;
				seen_events = uint16_t(seen_events | g.server_events);
				if (g.stats.rtt_ms > 0) {
					rtt_sum += g.stats.rtt_ms;
					++rtt_n;
				}
			}
		}, 1);
		const std::optional<net::DebugReport> trace = wait_report(*live, 31);
		REQUIRE(trace.has_value());
		REQUIRE(trace->trace.size() == 240);
		size_t compared = 0, predicted_equal = 0, predicted_compared = 0;
		uint16_t odd_events = 0, trace_events = 0;
		for (size_t i = 0; i < trace->trace.size(); ++i) {
			const SoldierState &e = trace->trace[i];
			CHECK(e.tick == trace->trace[0].tick + Tick(i));
			trace_events = uint16_t(trace_events | e.events);
			if (e.tick % 2 == 1) {
				odd_events = uint16_t(odd_events | e.events);
			}
			if (auto it = server_own.find(e.tick); it != server_own.end()) {
				++compared;
				CHECK(it->second == e);
			}
			if (auto it = predicted.find(e.tick); it != predicted.end()) {
				++predicted_compared;
				predicted_equal += soldier_state_hash(it->second) == soldier_state_hash(e) ? 1 : 0;
			}
		}
		const ClientStats s = live->client.stats();
		const GameServerStats g = live->server.game_stats();
		std::printf("loopback: trace %zu ticks from %u, %zu snapshot states compared, %zu / %zu predictions equal; events in the trace %04x, on odd ticks "
					"%04x, seen in server_events %04x\n",
				trace->trace.size(), trace->trace[0].tick, compared, predicted_equal, predicted_compared, trace_events, odd_events, seen_events);
		std::printf("loopback: RTT mean %.2f ms (min %.2f, ENet %.1f), buffer %d, target %u, clock adjust %.4f, resets %llu (forced %llu), "
					"reconciliations %llu, replayed %llu, hash mismatches %llu; walkers %llu presented, %llu extrapolated, track error max %.4f p99 %.4f m\n",
				rtt_n ? rtt_sum / double(rtt_n) : 0.0, s.rtt_min_ms, s.enet_rtt_ms, s.command_buffer_ticks, s.target_buffer_ticks, double(s.clock_adjust),
				(unsigned long long)s.clock_resets, (unsigned long long)s.forced_forward, (unsigned long long)s.reconciliations,
				(unsigned long long)s.replayed_ticks, (unsigned long long)s.hash_mismatches, (unsigned long long)interp.presented,
				(unsigned long long)interp.extrapolated, double(interp.max_error), double(interp.p99()));
		CHECK(compared > 60);
		CHECK(predicted_compared > 100);
		CHECK(predicted_equal == predicted_compared);
		CHECK((trace_events & soldier_events::kJumped) != 0);
		CHECK((trace_events & soldier_events::kPoseStarted) != 0);
		CHECK((trace_events & soldier_events::kPoseFinished) != 0);
		CHECK(odd_events != 0);
		std::printf("loopback: %llu frames missed by the 1 ms poll\n", (unsigned long long)missed_frames);
		if (missed_frames == 0) {
			CHECK((seen_events & odd_events) == odd_events);
		} else {
			WARN((seen_events & odd_events) == odd_events);
		}
		CHECK(s.reconciliations == 0);
		CHECK(s.step_precondition_failures == 0);
		CHECK(s.hash_mismatches == 0);
		CHECK(s.target_buffer_ticks == 2);
		CHECK(rtt_n > 0);

		CHECK(rtt_sum / double(rtt_n) >= 0.0);
		CHECK(rtt_sum / double(rtt_n) <= 1000.0 / 60.0 + 2.0);
		CHECK(g.step_precondition_failures == 0);
		const SlotStats *own = own_slot(g);
		REQUIRE(own != nullptr);
		CHECK(own->commands_late == 0);
		CHECK(interp.presented > 500);
		CHECK(interp.compared > 300);
		CHECK(interp.max_error < 0.15f);
		CHECK(interp.p99() < 0.05f);

		net::DebugRequest tr2 = tr;
		tr2.id = 32;
		tr2.ticks = 120;
		uint16_t after_stall = 0;
		Tick newest_after = 0;
		live->client.send_debug(tr2);
		run_for(*live, 0.1);

		live->input.pose = Pose::Crouch;
		bool started = false;
		const auto pose_sent = std::chrono::steady_clock::now();
		while (!started && seconds_since(pose_sent) < 2.0) {
			poll(*live);
			started = live->frame->valid && live->frame->own_curr.target_pose == Pose::Crouch;
			if (!started) {
				std::this_thread::sleep_for(1ms);
			}
		}
		REQUIRE(started);
		const auto stall_requested = std::chrono::steady_clock::now();
		live->client.debug_stall(800);
		bool first = true;
		run_for(*live, 1.5, [&](bool p_fresh, uint64_t, double) {
			const PresentationFrame &h = *live->frame;
			if (p_fresh && seconds_since(stall_requested) > 0.75 && h.server_own_valid && first && h.server_events != 0) {
				after_stall = h.server_events;
				newest_after = h.server_own.tick;
				first = false;
			}
		});
		const std::optional<net::DebugReport> trace2 = wait_report(*live, 32);
		REQUIRE(trace2.has_value());
		Tick finished = 0;
		for (const SoldierState &e : trace2->trace) {
			if ((e.events & soldier_events::kPoseFinished) != 0) {
				finished = e.tick;
			}
		}
		std::printf("stall: PoseFinished at server tick %u; first frame after the stall with server events %04x names snapshot tick %u\n", finished,
				after_stall, newest_after);
		REQUIRE(finished != 0);
		CHECK((after_stall & soldier_events::kPoseFinished) != 0);
		CHECK(newest_after > finished + 2);
		live->client.stop();
		live->server.stop();
	}

	TEST_CASE("host.session: forced clock resets forward and backward, a 3 s stall, and a start behind the server keep the prediction base") {

		auto live = start_live(synthetic_level(), { .netsim = "lat=40" });
		REQUIRE(wait_spawned(*live));
		run_for(*live, 3.0);
		const ClientStats s0 = live->client.stats();
		CHECK(s0.reconciliations == 0);
		CHECK(s0.step_precondition_failures == 0);

		live->client.debug_shift_clock(-20);
		run_for(*live, 2.0);
		const ClientStats s1 = live->client.stats();
		std::printf("shift -20: resets +%llu (forced +%llu), base fills +%llu, reconciliations +%llu, ring misses +%llu, precondition %llu\n",
				(unsigned long long)(s1.clock_resets - s0.clock_resets), (unsigned long long)(s1.forced_forward - s0.forced_forward),
				(unsigned long long)(s1.base_fills - s0.base_fills), (unsigned long long)(s1.reconciliations - s0.reconciliations),
				(unsigned long long)(s1.ring_misses - s0.ring_misses), (unsigned long long)s1.step_precondition_failures);
		CHECK(s1.clock_resets - s0.clock_resets == 1);

		CHECK(s1.reconciliations - s0.reconciliations <= 1);
		CHECK(s1.step_precondition_failures == 0);

		const uint64_t replaced0 = own_slot(live->server.game_stats())->commands_replaced;
		live->client.debug_shift_clock(20);
		run_for(*live, 2.0);
		const ClientStats s2 = live->client.stats();
		const uint64_t replaced1 = own_slot(live->server.game_stats())->commands_replaced;
		std::printf("shift +20: resets +%llu, base fills +%llu, reconciliations +%llu, commands replaced +%llu, precondition %llu\n",
				(unsigned long long)(s2.clock_resets - s1.clock_resets), (unsigned long long)(s2.base_fills - s1.base_fills),
				(unsigned long long)(s2.reconciliations - s1.reconciliations), (unsigned long long)(replaced1 - replaced0),
				(unsigned long long)s2.step_precondition_failures);
		CHECK(s2.clock_resets - s1.clock_resets == 1);
		CHECK(replaced1 > replaced0);

		CHECK(s2.base_fills > s1.base_fills);

		CHECK(s2.reconciliations - s1.reconciliations <= 1);
		CHECK(s2.step_precondition_failures == 0);

		live->client.debug_stall(3000);
		run_for(*live, 4.5);
		const ClientStats s3 = live->client.stats();
		std::printf("stall 3 s: ring misses +%llu, base fills +%llu, resets +%llu, reconciliations +%llu, precondition %llu\n",
				(unsigned long long)(s3.ring_misses - s2.ring_misses), (unsigned long long)(s3.base_fills - s2.base_fills),
				(unsigned long long)(s3.clock_resets - s2.clock_resets), (unsigned long long)(s3.reconciliations - s2.reconciliations),
				(unsigned long long)s3.step_precondition_failures);
		CHECK(s3.ring_misses + s3.base_fills > s2.ring_misses + s2.base_fills);
		CHECK(s3.step_precondition_failures == 0);
		run_for(*live, 2.0);
		const ClientStats s4 = live->client.stats();
		CHECK(s4.reconciliations == s3.reconciliations);
		CHECK(s4.clock_resets == s3.clock_resets);
		CHECK(s4.hash_mismatches == 0);
		CHECK(live->server.game_stats().step_precondition_failures == 0);
		live->client.stop();
		live->server.stop();

		auto behind = start_live(synthetic_level(), { .clock_offset = -30 });
		REQUIRE(wait_spawned(*behind));
		run_for(*behind, 2.0);
		const ClientStats b = behind->client.stats();
		std::printf("start behind: forced forward %llu, resets %llu, reconciliations %llu, precondition %llu\n", (unsigned long long)b.forced_forward,
				(unsigned long long)b.clock_resets, (unsigned long long)b.reconciliations, (unsigned long long)b.step_precondition_failures);
		CHECK(b.forced_forward >= 1);
		CHECK(b.step_precondition_failures == 0);
		CHECK(b.reconciliations == 0);
		CHECK(b.spawns == 1);
		behind->client.stop();
		behind->server.stop();
	}

	TEST_CASE("host.session: a server-side teleport is reconciled once and smoothed (offset 0 within 400 ms)") {
		auto live = start_live(synthetic_level(), {});
		REQUIRE(wait_spawned(*live));
		run_for(*live, 1.5);
		const SoldierState here = live->frame->own_curr;
		net::DebugRequest tp;
		tp.kind = net::DebugKind::Teleport;
		tp.id = 41;
		tp.position = { here.position[0] + 0.08f, here.position[1], here.position[2] };
		tp.yaw = here.yaw;
		live->client.send_debug(tp);
		uint64_t reconcile_ns = 0, zero_ns = 0;
		float max_offset = 0;
		run_for(*live, 2.5, [&](bool p_fresh, uint64_t, double) {
			const PresentationFrame &f = *live->frame;
			if (!p_fresh || !f.valid) {
				return;
			}
			const float o = std::sqrt(f.visual_offset[0] * f.visual_offset[0] + f.visual_offset[1] * f.visual_offset[1] + f.visual_offset[2] * f.visual_offset[2]);
			max_offset = std::max(max_offset, o);
			if (f.stats.reconciliations > 0 && reconcile_ns == 0) {
				reconcile_ns = f.stats.last_reconcile_ns;
			}
			if (reconcile_ns != 0 && zero_ns == 0 && o == 0.0f) {
				zero_ns = f.publish_ns;
			}
		});
		const ClientStats s = live->client.stats();
		std::printf("teleport 0.08 m: reconciliations %llu, max offset %.4f m, offset 0 after %.0f ms, teleports %llu\n", (unsigned long long)s.reconciliations,
				double(max_offset), reconcile_ns && zero_ns ? double(zero_ns - reconcile_ns) / 1e6 : -1.0, (unsigned long long)s.teleports);
		CHECK(s.reconciliations == 1);
		CHECK(s.teleports == 0);
		CHECK(max_offset > 0.05f);
		REQUIRE(zero_ns != 0);
		CHECK(double(zero_ns - reconcile_ns) / 1e6 <= 400.0);
		CHECK(s.step_precondition_failures == 0);
		CHECK(live->server.game_stats().step_precondition_failures == 0);
		live->client.stop();
		live->server.stop();
	}

	TEST_CASE("host.session: E2 under 150/2 on the synthetic world through judge_rest (shortened); clock, RTT, extrapolation") {
		auto live = start_live(synthetic_level(), { .walkers = 2, .netsim = "150/2", .seed = 3 });
		REQUIRE(wait_spawned(*live));
		const float heading = yaw_radians(live->frame->own_curr.yaw);
		const E2Run run = run_e2(*live, 12.0, 2, 2.0, 12.0, heading, true);
		print_e2("150/2 (synthetic, shortened)", run);
		REQUIRE(run.cycles.size() == 3);
		for (const RestCycleResult &c : run.cycles) {
			CHECK_MESSAGE(c.ok, c.failure.data());
		}
		CHECK(run.stats.step_precondition_failures == 0);
		CHECK(run.server_precondition_failures == 0);
		CHECK(run.stats.hash_mismatches == 0);
		for (uint32_t t : run.targets) {
			CHECK((t == 2 || t == 3));
		}
		CHECK(run.target_changes <= 2);
		const double rtt = run.rtt_sum / double(std::max<size_t>(run.rtt_n, 1));
		const double nominal = run.stats.netsim_out.mean_delay_ms + run.stats.netsim_in.mean_delay_ms;
		CHECK(rtt >= nominal - 2.0);
		CHECK(rtt <= nominal + 1000.0 / 60.0 + 2.0);
		CHECK(run.interp.presented > 1000);
		CHECK(double(run.interp.extrapolated) < 0.05 * double(run.interp.presented));
		live->client.stop();
		live->server.stop();
	}

	TEST_CASE("host.session: SpawnSweep through the live server (128 slots) equals the in-process rows") {
		auto live = start_live(synthetic_level(), { .walkers = 2 });
		REQUIRE(wait_spawned(*live));
		net::DebugRequest q;
		q.kind = net::DebugKind::SpawnSweep;
		q.id = 51;
		q.ticks = 60;
		live->client.send_debug(q);
		const std::optional<net::DebugReport> r = wait_report(*live, 51);
		REQUIRE(r.has_value());
		auto level = synthetic_level();
		REQUIRE(r->sweep.size() == level->spawns.size());
		auto world = CollisionWorld::create(level->static_world);
		REQUIRE(world.ok());
		SoldierMotor m;
		REQUIRE(m.init(**world, level->motor).ok());
		for (size_t i = 0; i < r->sweep.size(); ++i) {
			const SettleResult h = settle_spawn(m, **world, level->spawns[i], 1, 0);
			CHECK(r->sweep[i].ok == h.ok);
			CHECK(r->sweep[i].settled == h.state.position);
			CHECK(r->sweep[i].gap == h.gap);
		}
		CHECK(r->ok);
		CHECK(live->client.stats().reconciliations == 0);
		live->client.stop();
		live->server.stop();
	}
}

namespace {

struct Metro {
	std::unique_ptr<core::InstallReader> reader;
	std::shared_ptr<core::Install> install;
	core::InstallSnapshot before;
	std::shared_ptr<const SharedLevel> level;
	std::filesystem::path cache_root;
	double seconds_cold = 0, seconds_warm = 0;
};

Metro *metro() {
	static std::unique_ptr<Metro> s_metro;
	static bool s_tried = false;
	if (s_tried) {
		if (s_metro == nullptr) {
			bf3::test::skip_install_case("no BF3 install (see the first host-session case)");
		}
		return s_metro.get();
	}
	s_tried = true;
	auto m = std::make_unique<Metro>();
	m->reader = std::make_unique<core::InstallReader>();
	if (!m->reader->locate({})) {
		bf3::test::skip_install_case(m->reader->describe().c_str());
		return nullptr;
	}
	m->before = m->reader->snapshot();
	core::Result<std::shared_ptr<core::Install>> opened = core::Install::open(*m->reader);
	REQUIRE_MESSAGE(opened.ok(), (opened.ok() ? std::string() : opened.error().message));
	m->install = *opened;

	m->cache_root = std::filesystem::temp_directory_path() / ("openbf3-host-session-" + std::to_string(GetCurrentProcessId()));
	std::error_code ec;
	std::filesystem::remove_all(m->cache_root, ec);
	SharedLoadOptions options;
	options.tuning = shipped_tuning();
	for (int pass = 0; pass < 2; ++pass) {
		core::Cache cache;
		core::CacheOptions co;
		co.root = m->cache_root;
		co.guard = m->reader->write_guard();
		std::string detail;
		REQUIRE_MESSAGE(cache.open(std::move(co), &detail) == core::CacheStatus::Ok, detail);
		options.cache = &cache;
		LoadProgress progress;
		const auto start = std::chrono::steady_clock::now();
		core::Result<LevelDatabase> db = open_level_database(m->install, "MP_Subway", "TeamDeathMatch0", 0, &progress);
		REQUIRE_MESSAGE(db.ok(), (db.ok() ? std::string() : db.error().message));
		core::Result<std::shared_ptr<const SharedLevel>> level = load_shared_level(*db, options, &progress);
		REQUIRE_MESSAGE(level.ok(), (level.ok() ? std::string() : level.error().message));
		const double secs = seconds_since(start);
		const StaticWorldStats &st = (*level)->static_world->stats();
		std::printf("Metro load_shared_level pass %d: %.2f s (mount %.2f, db %.2f, tables %.3f, shapes %.3f), %s, %zu / %zu tiles hit, %zu spawns, "
					"protection radius %.1f, warmth %u, permille %u, peak working set %.1f MiB\n",
				pass, secs, db->seconds_mount, db->seconds_database, (*level)->seconds_tables, (*level)->seconds_shapes,
				st.world_hit ? "world hit" : "world miss", st.tiles_hit, st.tiles, (*level)->spawns.size(), double((*level)->spawn_protection_radius),
				unsigned(progress.warmth.load()), progress.permille(), double(progress.peak_working_set.load()) / (1024.0 * 1024.0));
		CHECK((*level)->spawns.size() == 102);
		CHECK(st.world_hit == (pass == 1));
		CHECK(progress.warmth.load() == uint8_t(pass == 1 ? CacheWarmth::Warm : CacheWarmth::Cold));
		CHECK((*level)->motor.tuning == shipped_tuning());
		(pass == 0 ? m->seconds_cold : m->seconds_warm) = secs;
		m->level = *level;
		cache.close();
	}
	s_metro = std::move(m);
	return s_metro.get();
}

std::optional<float> metro_heading(Live &r_live, const Metro &p_metro) {
	auto world = CollisionWorld::create(p_metro.level->static_world);
	REQUIRE(world.ok());
	SoldierMotor m;
	REQUIRE(m.init(**world, p_metro.level->motor).ok());
	const std::optional<uint16_t> h = find_clear_heading(m, **world, r_live.frame->own_curr.position, 10.0f);
	if (h) {
		return yaw_radians(*h);
	}
	for (const uint32_t point : { 7u, 20u, 22u, 23u, 36u, 70u }) {
		net::DebugRequest q;
		q.kind = net::DebugKind::SpawnAt;
		q.id = 900 + point;
		q.index = point;
		r_live.client.send_debug(q);
		REQUIRE(wait_report(r_live, q.id).has_value());
		const uint32_t life = r_live.frame->own_curr.life;
		const auto start = std::chrono::steady_clock::now();
		while (seconds_since(start) < 3.0 && !(r_live.frame->valid && r_live.frame->own_curr.life > life && r_live.input.life == r_live.frame->own_curr.life)) {
			poll(r_live);
			std::this_thread::sleep_for(4ms);
		}
		run_for(r_live, 1.2);
		const std::optional<uint16_t> g = find_clear_heading(m, **world, r_live.frame->own_curr.position, 10.0f);
		if (g) {
			std::printf("Metro E2 course: spawn point %u, heading %u\n", point, unsigned(*g));
			return yaw_radians(*g);
		}
	}
	return std::nullopt;
}

E2Run metro_e2(Metro &r_metro, const char *p_profile, uint64_t p_seed) {
	auto live = start_live(r_metro.level, { .walkers = 2, .netsim = p_profile, .seed = p_seed });
	REQUIRE(wait_spawned(*live));
	run_for(*live, 1.2);
	const std::optional<float> heading = metro_heading(*live, r_metro);
	REQUIRE(heading.has_value());

	E2Run run = run_e2(*live, 30.0, 4, 2.0, 13.0, *heading, true);
	live->client.stop();
	live->server.stop();
	return run;
}

}

TEST_SUITE("host-session") {
	TEST_CASE("host.session: Metro: load_shared_level cold then warm on a scratch cache, with the shipped tuning") {
		Metro *m = metro();
		if (m == nullptr) {
			return;
		}
		CHECK(m->level->level == "MP_Subway");
		CHECK(m->level->mode == "TeamDeathMatch0");

		CHECK(m->level->static_world->tile_count() == 149);
		CHECK(m->level->static_world->terrain_count() == 1);
		CHECK(m->level->spawn_protection_radius == 10.0f);
		CHECK(m->level->source_digest.has_value());
	}

	TEST_CASE("host.session: Metro E2 under 150/2 (150 ms RTT, 2 % loss each way): the gate") {
		Metro *m = metro();
		if (m == nullptr) {
			return;
		}
		const E2Run run = metro_e2(*m, "150/2", 11);
		print_e2("150/2 (Metro, gate)", run);
		REQUIRE(run.cycles.size() == 5);
		for (const RestCycleResult &c : run.cycles) {
			CHECK_MESSAGE(c.ok, c.failure.data());
		}
		CHECK(run.stats.step_precondition_failures == 0);
		CHECK(run.server_precondition_failures == 0);
		CHECK(run.stats.hash_mismatches == 0);
		for (uint32_t t : run.targets) {
			CHECK((t == 2 || t == 3));
		}
		CHECK(run.target_changes <= 2);
		const double rtt = run.rtt_sum / double(std::max<size_t>(run.rtt_n, 1));
		const double nominal = run.stats.netsim_out.mean_delay_ms + run.stats.netsim_in.mean_delay_ms;
		CHECK(rtt >= nominal - 2.0);
		CHECK(rtt <= nominal + 1000.0 / 60.0 + 2.0);
		CHECK(double(run.interp.extrapolated) < 0.05 * double(std::max<uint64_t>(run.interp.presented, 1)));
	}

	TEST_CASE("host.session: Metro E2 under lat=150-165,loss=2 (150 ms one way): a reported stress row, not gated") {
		Metro *m = metro();
		if (m == nullptr) {
			return;
		}
		const E2Run run = metro_e2(*m, "lat=150-165,loss=2", 12);
		print_e2("lat=150-165,loss=2 (Metro, stress row)", run);
		size_t ok = 0;
		for (const RestCycleResult &c : run.cycles) {
			ok += c.ok ? 1 : 0;
		}
		std::printf("E2 stress row: %zu / %zu rest periods ok (reported, not gated: orchestrator decision on docs §16 question 2)\n", ok, run.cycles.size());

		CHECK(run.stats.step_precondition_failures == 0);
		CHECK(run.server_precondition_failures == 0);
		CHECK(run.stats.hash_mismatches == 0);
	}

	TEST_CASE("host.session: Metro E3: SpawnSweep with 128 slots, 102 / 102 ok, equal to the rows computed in-process") {
		Metro *m = metro();
		if (m == nullptr) {
			return;
		}
		auto live = start_live(m->level, { .walkers = 2 });
		REQUIRE(wait_spawned(*live));

		auto world = CollisionWorld::create(m->level->static_world);
		REQUIRE(world.ok());
		SoldierMotor motor;
		REQUIRE(motor.init(**world, m->level->motor).ok());
		const auto start = std::chrono::steady_clock::now();
		std::vector<SettleResult> headless;
		for (const SpawnPoint &p : m->level->spawns) {
			headless.push_back(settle_spawn(motor, **world, p, 1, 0));
		}
		const double headless_s = seconds_since(start);
		net::DebugRequest q;
		q.kind = net::DebugKind::SpawnSweep;
		q.id = 61;
		q.ticks = 60;
		const auto sweep_start = std::chrono::steady_clock::now();
		live->client.send_debug(q);
		const std::optional<net::DebugReport> r = wait_report(*live, 61, 30.0);
		const double sweep_s = seconds_since(sweep_start);
		REQUIRE(r.has_value());
		REQUIRE(r->sweep.size() == 102);
		size_t ok = 0, equal = 0;
		for (size_t i = 0; i < r->sweep.size(); ++i) {
			const net::SpawnSweepRow &row = r->sweep[i];
			const SettleResult &h = headless[i];
			const bool same = row.index == m->level->spawns[i].index && row.ok == h.ok && row.point == m->level->spawns[i].position &&
					row.settled == h.state.position && row.lift == h.lift && row.shift == h.shift && row.gap == h.gap &&
					row.grounded == ((h.state.flags & soldier_flags::kGrounded) != 0) && row.overlapping == h.overlapping;
			CHECK_MESSAGE(same, "point " << i);
			CHECK_MESSAGE(row.ok, "point " << i << " not ok");
			ok += row.ok ? 1 : 0;
			equal += same ? 1 : 0;
		}
		const GameServerStats g = live->server.game_stats();
		std::printf("E3 Metro SpawnSweep: %zu / %zu ok, %zu equal to the in-process rows (headless %.3f s, sweep through the server %.2f s, one batch); "
					"%s; server p99 step %.2f ms\n",
				ok, r->sweep.size(), equal, headless_s, sweep_s, r->text.c_str(), live->server.stats().p99_step_ms);
		CHECK(ok == 102);
		CHECK(equal == 102);
		CHECK(g.step_precondition_failures == 0);
		live->client.stop();
		live->server.stop();
	}

	TEST_CASE("host.session: Metro: install unchanged (layout.toc, cas.cat, the .sb / .toc files)") {
		Metro *m = metro();
		if (m == nullptr) {
			return;
		}
		const core::SnapshotDiff diff = core::diff_snapshots(m->before, m->reader->snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
		std::printf("install.unchanged: %s (%zu files)\n", diff.clean() ? "clean" : diff.summary().c_str(), m->before.files.size());
		std::error_code ec;
		std::filesystem::remove_all(m->cache_root, ec);
	}
}

namespace m1_present_ref {

float lerp(float p_a, float p_b, float p_t) {
	return p_a + (p_b - p_a) * p_t;
}

float hermite(float p_p0, float p_m0, float p_p1, float p_m1, float p_u) {
	const float u2 = p_u * p_u, u3 = u2 * p_u;
	return (2 * u3 - 3 * u2 + 1) * p_p0 + (u3 - 2 * u2 + p_u) * p_m0 + (-2 * u3 + 3 * u2) * p_p1 + (u3 - u2) * p_m1;
}

float eye_of(const PresentationFrame &p_frame, const net::OtherSoldier &p_o) {
	const size_t from = std::min<size_t>(uint8_t(p_o.pose), kPoseCount - 1);
	const size_t to = std::min<size_t>(uint8_t(p_o.target_pose), kPoseCount - 1);
	return lerp(p_frame.pose_eye_heights[from], p_frame.pose_eye_heights[to], float(p_o.transition) / 255.0f);
}

PresentedSoldier present_sample(const PresentationFrame &p_frame, const net::OtherSoldier &p_o) {
	PresentedSoldier s;
	s.slot = p_o.slot;
	s.generation = p_o.generation;
	s.position = p_o.position;
	s.velocity = p_o.velocity;
	s.yaw = yaw_radians(p_o.yaw);
	s.pitch = pitch_radians(p_o.pitch);
	s.pose = p_o.pose;
	s.eye_height = eye_of(p_frame, p_o);
	s.mode = p_o.mode;
	s.flags = p_o.flags;
	return s;
}

size_t present_others(const PresentationFrame &p_frame, uint64_t p_now_ns, std::span<PresentedSoldier> r_out, uint64_t *r_extrapolated) {
	constexpr double kMsPerTick = 1000.0 / double(kSimHz);
	const double dt_ns = p_now_ns >= p_frame.publish_ns ? double(p_now_ns - p_frame.publish_ns) : -double(p_frame.publish_ns - p_now_ns);
	const double render = p_frame.render_ticks_at_publish + dt_ns * p_frame.ticks_per_ns;
	const double max_extra_ticks = std::max(0.0, double(p_frame.extrapolate_max_ms)) / kMsPerTick;
	size_t n = 0;
	const uint32_t count = std::min<uint32_t>(p_frame.other_count, uint32_t(p_frame.others.size()));
	for (uint32_t i = 0; i < count && n < r_out.size(); ++i) {
		const OtherTrack &t = p_frame.others[i];
		const uint32_t used = std::min<uint32_t>(t.count, uint32_t(kOtherSamples));
		if (used == 0) {
			continue;
		}
		uint32_t a = 0;
		while (a + 1 < used && double(t.samples[a + 1].server_tick) <= render) {
			++a;
		}
		const OtherSample &sa = t.samples[a];
		PresentedSoldier out;
		if (render <= double(sa.server_tick)) {
			out = present_sample(p_frame, sa.soldier);
		} else if (a + 1 < used) {
			const OtherSample &sb = t.samples[a + 1];
			const double span = double(sb.server_tick - sa.server_tick);
			const float u = float(std::clamp((render - double(sa.server_tick)) / span, 0.0, 1.0));
			const float dt = float(span) * kTickSeconds;
			out = present_sample(p_frame, sa.soldier);
			for (size_t k = 0; k < 3; ++k) {
				out.position[k] = hermite(sa.soldier.position[k], sa.soldier.velocity[k] * dt, sb.soldier.position[k], sb.soldier.velocity[k] * dt, u);
				out.velocity[k] = lerp(sa.soldier.velocity[k], sb.soldier.velocity[k], u);
			}
			const int16_t dyaw = int16_t(uint16_t(sb.soldier.yaw - sa.soldier.yaw));
			out.yaw = yaw_radians(uint16_t(sa.soldier.yaw + int32_t(std::lround(float(dyaw) * u))));
			out.pitch = lerp(pitch_radians(sa.soldier.pitch), pitch_radians(sb.soldier.pitch), u);
			const net::OtherSoldier &discrete = u >= 1.0f ? sb.soldier : sa.soldier;
			out.pose = discrete.pose;
			out.mode = discrete.mode;
			out.flags = discrete.flags;
			out.generation = discrete.generation;
			out.eye_height = lerp(eye_of(p_frame, sa.soldier), eye_of(p_frame, sb.soldier), u);
		} else {
			const double ahead = std::min(render - double(sa.server_tick), max_extra_ticks);
			const float secs = float(ahead) * kTickSeconds;
			out = present_sample(p_frame, sa.soldier);
			for (size_t k = 0; k < 3; ++k) {
				out.position[k] = sa.soldier.position[k] + sa.soldier.velocity[k] * secs;
			}
			out.extrapolated = true;
			if (r_extrapolated != nullptr) {
				++*r_extrapolated;
			}
		}
		r_out[n++] = out;
	}
	return n;
}

bool same_m1_fields(const PresentedSoldier &p_a, const PresentedSoldier &p_b) {
	auto bits = [](const auto &p_x, const auto &p_y) { return std::memcmp(&p_x, &p_y, sizeof(p_x)) == 0; };
	return p_a.slot == p_b.slot && p_a.generation == p_b.generation && bits(p_a.position, p_b.position) && bits(p_a.velocity, p_b.velocity) &&
			bits(p_a.yaw, p_b.yaw) && bits(p_a.pitch, p_b.pitch) && p_a.pose == p_b.pose && bits(p_a.eye_height, p_b.eye_height) && p_a.mode == p_b.mode &&
			p_a.flags == p_b.flags && p_a.extrapolated == p_b.extrapolated;
}

}

TEST_SUITE("host-session-unit") {
	TEST_CASE("host.session: M2a cl R1: present_others through net::sample_other draws what M1's body drew (bit for bit, 200,000 cases)") {
		std::mt19937_64 rng(0xC1A1);
		std::uniform_real_distribution<float> pos(-300.0f, 300.0f), vel(-8.0f, 8.0f);
		std::uniform_int_distribution<int> any(0, 65535), small(0, 255), pose(0, 2), count(0, 3), gap(1, 4);
		std::uniform_real_distribution<double> offset(-3.0, 30.0);
		auto f = std::make_unique<PresentationFrame>();
		f->publish_ns = 5'000'000'000ull;
		f->ticks_per_ns = 60e-9;
		f->extrapolate_max_ms = 250.0f;
		size_t cases = 0, equal = 0, extrapolated = 0, held = 0, bracketed = 0, before = 0;
		for (int round = 0; round < 50'000; ++round) {
			f->other_count = 4;
			uint32_t base = 1000u + uint32_t(any(rng));
			for (uint32_t i = 0; i < 4; ++i) {
				OtherTrack &t = f->others[i];
				t.slot = uint8_t(i + 1);
				t.count = uint8_t(count(rng));
				uint32_t tick = base + uint32_t(gap(rng));
				for (uint32_t k = 0; k < t.count; ++k) {
					net::OtherSoldier o;
					o.slot = t.slot;
					o.generation = uint8_t(small(rng));
					o.position = { pos(rng), pos(rng) * 0.1f, pos(rng) };
					o.velocity = { vel(rng), vel(rng) * 0.2f, vel(rng) };
					o.yaw = uint16_t(any(rng));
					o.pitch = uint16_t(any(rng));
					o.pose = Pose(pose(rng));
					o.target_pose = Pose(pose(rng));
					o.transition = uint8_t(small(rng));
					o.mode = MoveMode(small(rng) % 3);
					o.flags = uint8_t(small(rng));
					t.samples[k].server_tick = tick;
					t.samples[k].soldier = o;
					tick += uint32_t(gap(rng));
				}
			}
			f->render_ticks_at_publish = double(base) + offset(rng);
			const uint64_t now = f->publish_ns + uint64_t(any(rng)) * 1000ull;
			std::array<PresentedSoldier, 4> a{}, b{};
			uint64_t ea = 0, eb = 0;
			const size_t na = m1_present_ref::present_others(*f, now, a, &ea);
			const size_t nb = present_others(*f, now, b, &eb);
			CHECK(na == nb);
			CHECK(ea == eb);
			const double render = f->render_ticks_at_publish + double(now - f->publish_ns) * f->ticks_per_ns;
			for (size_t i = 0; i < std::min(na, nb); ++i) {
				++cases;
				const bool same = m1_present_ref::same_m1_fields(a[i], b[i]);
				equal += same ? 1u : 0u;
				if (!same && cases - equal <= 3) {
					MESSAGE("case " << cases << " slot " << int(a[i].slot) << " differs");
				}
				const OtherTrack *t = nullptr;
				for (uint32_t k = 0; k < 4; ++k) {
					t = f->others[k].slot == a[i].slot ? &f->others[k] : t;
				}
				if (a[i].extrapolated) {
					++extrapolated;
					held += render - double(t->samples[t->count - 1].server_tick) > 15.0 ? 1u : 0u;
				} else if (render <= double(t->samples[0].server_tick)) {
					++before;
				} else {
					++bracketed;
				}
			}
		}
		CHECK(cases > 100'000);
		CHECK(equal == cases);
		CHECK(bracketed > 10'000);
		CHECK(extrapolated > 10'000);
		CHECK(held > 1'000);
		CHECK(before > 1'000);
		std::printf("R1: %zu soldiers drawn, %zu equal to M1's body in every M1 field (bracketed %zu, before the oldest %zu, extrapolated %zu of which "
					"held past 250 ms %zu)\n",
				cases, equal, bracketed, before, extrapolated, held);
	}
}

namespace {

struct Pipe {
	struct Sent {
		uint32_t peer = 0;
		uint8_t channel = 0;
		std::vector<uint8_t> bytes;
	};

	mutable std::mutex mutex;
	std::condition_variable to_client_ready;
	std::deque<net::Host::Event> to_server;
	std::map<uint32_t, std::deque<Sent>> to_client;
	std::vector<Sent> sent;
	std::vector<std::pair<uint32_t, size_t>> disconnects;
	std::set<uint32_t> connected;
	std::atomic<uint64_t> services{ 0 };

	void connect(uint32_t p_peer) {
		std::lock_guard lock(mutex);
		connected.insert(p_peer);
		net::Host::Event e;
		e.type = net::Host::EventType::Connect;
		e.peer = p_peer;
		to_server.push_back(std::move(e));
	}
	void client_send(uint32_t p_peer, uint8_t p_channel, std::vector<uint8_t> p_bytes) {
		std::lock_guard lock(mutex);
		net::Host::Event e;
		e.type = net::Host::EventType::Receive;
		e.peer = p_peer;
		e.channel = p_channel;
		e.data = std::move(p_bytes);
		to_server.push_back(std::move(e));
	}
	bool to_server_empty() const {
		std::lock_guard lock(mutex);
		return to_server.empty();
	}
	size_t count_sent(uint32_t p_peer, net::MessageType p_type) const {
		std::lock_guard lock(mutex);
		size_t n = 0;
		for (const Sent &s : sent) {
			n += s.peer == p_peer && net::peek_type(s.bytes) == p_type ? 1 : 0;
		}
		return n;
	}
	std::optional<net::MatchInfo> match_info(uint32_t p_peer) const {
		std::lock_guard lock(mutex);
		for (const Sent &s : sent) {
			if (s.peer == p_peer && net::peek_type(s.bytes) == net::MessageType::MatchInfo) {
				core::Result<net::MatchInfo> m = net::decode_match_info(s.bytes);
				return m.ok() ? std::optional<net::MatchInfo>(*m) : std::nullopt;
			}
		}
		return std::nullopt;
	}
};

class PipeServerPort final : public ServerNetPort {
public:
	explicit PipeServerPort(std::shared_ptr<Pipe> p_pipe) : pipe_(std::move(p_pipe)) {}

	int service(net::Host::Event &r_event) override {
		pipe_->services.fetch_add(1);
		std::lock_guard lock(pipe_->mutex);
		if (pipe_->to_server.empty()) {
			return 0;
		}
		r_event = std::move(pipe_->to_server.front());
		pipe_->to_server.pop_front();
		return 1;
	}
	bool send(uint32_t p_peer, uint8_t p_channel, std::span<const uint8_t> p_bytes, net::Host::SendMode) override {
		std::lock_guard lock(pipe_->mutex);
		if (pipe_->connected.count(p_peer) == 0) {
			return false;
		}
		Pipe::Sent s{ p_peer, p_channel, std::vector<uint8_t>(p_bytes.begin(), p_bytes.end()) };
		pipe_->sent.push_back(s);
		pipe_->to_client[p_peer].push_back(std::move(s));
		pipe_->to_client_ready.notify_all();
		return true;
	}
	void disconnect_later(uint32_t p_peer) override {
		std::lock_guard lock(pipe_->mutex);
		pipe_->disconnects.push_back({ p_peer, pipe_->sent.size() });
		pipe_->connected.erase(p_peer);
		net::Host::Event e;
		e.type = net::Host::EventType::Disconnect;
		e.peer = p_peer;
		pipe_->to_server.push_back(std::move(e));
	}
	void flush() override {}
	void disconnect_all() override {
		std::lock_guard lock(pipe_->mutex);
		pipe_->connected.clear();
	}
	size_t connected_peers() const override {
		std::lock_guard lock(pipe_->mutex);
		return pipe_->connected.size();
	}
	uint16_t bound_port() const override { return 0; }
	net::PeerCounters peer_counters(uint32_t) const override { return {}; }
	size_t peer_capacity() const override { return 0; }

private:
	std::shared_ptr<Pipe> pipe_;
};

class PipeClientTransport final : public ClientTransport {
public:
	PipeClientTransport(std::shared_ptr<Pipe> p_pipe, uint32_t p_peer) : pipe_(std::move(p_pipe)), peer_(p_peer) {}

	int service(uint32_t p_wait_ms, net::Host::Event &r_event) override {
		std::unique_lock lock(pipe_->mutex);
		std::deque<Pipe::Sent> &in = pipe_->to_client[peer_];
		pipe_->to_client_ready.wait_for(lock, std::chrono::milliseconds(p_wait_ms), [&] { return !in.empty(); });
		if (in.empty()) {
			return 0;
		}
		r_event = net::Host::Event();
		r_event.type = net::Host::EventType::Receive;
		r_event.peer = peer_;
		r_event.channel = in.front().channel;
		r_event.data = std::move(in.front().bytes);
		in.pop_front();
		return 1;
	}
	void send(uint8_t p_channel, std::span<const uint8_t> p_bytes, net::Host::SendMode) override {
		pipe_->client_send(peer_, p_channel, std::vector<uint8_t>(p_bytes.begin(), p_bytes.end()));
	}
	void flush() override {}
	void disconnect_all() override {}
	uint32_t rtt_ms() const override { return 0; }

private:
	std::shared_ptr<Pipe> pipe_;
	uint32_t peer_ = 0;
};

bool wait_until(const std::function<bool()> &p_done) {
	const auto start = std::chrono::steady_clock::now();
	while (!p_done()) {
		if (seconds_since(start) > 10.0) {
			return false;
		}
		std::this_thread::sleep_for(16ms);
	}
	return true;
}

std::shared_ptr<GameServerConfig> pipe_game() {
	auto game = std::make_shared<GameServerConfig>();
	game->level = synthetic_level();
	game->max_slots = 8;
	return game;
}

}

TEST_SUITE("host-session-unit") {
	TEST_CASE("host.session: a GameServer whose NetThread is fed by a fake Host keeps answering while the tick is "
			  "stalled") {
		auto pipe = std::make_shared<Pipe>();
		ServerConfig sc;
		sc.game = pipe_game();
		sc.transport = std::make_shared<PipeServerPort>(pipe);
		ServerHost server;
		std::string err;
		REQUIRE_MESSAGE(server.start(sc, &err), err);
		CHECK(server.port() == 0);
		pipe->connect(0);
		REQUIRE(wait_until([&] { return server.stats().ticks >= 5 && pipe->to_server_empty(); }));

		server.debug_stall(500);
		REQUIRE(wait_until([&] { return server.stats().tick_stalls == 1; }));
		const uint64_t stalled_tick = server.stats().ticks;
		const uint64_t services = pipe->services.load();
		net::Hello hello;
		hello.name = "stalled";
		pipe->client_send(0, net::kChannelReliable, net::encode_hello(hello));
		REQUIRE(wait_until([&] { return pipe->services.load() >= services + 3 && pipe->to_server_empty(); }));
		CHECK(server.stats().ticks == stalled_tick);
		CHECK(pipe->count_sent(0, net::MessageType::MatchInfo) == 0);

		REQUIRE(wait_until([&] { return pipe->count_sent(0, net::MessageType::MatchInfo) == 1; }));
		CHECK(server.stats().ticks > stalled_tick);
		const std::optional<net::MatchInfo> match = pipe->match_info(0);
		REQUIRE(match.has_value());
		CHECK(match->slot == 0);

		pipe->connect(1);
		net::Hello stranger;
		stranger.protocol = net::kProtocolVersion + 1;
		pipe->client_send(1, net::kChannelReliable, net::encode_hello(stranger));
		REQUIRE(wait_until([&] {
			std::lock_guard lock(pipe->mutex);
			return !pipe->disconnects.empty();
		}));
		{
			std::lock_guard lock(pipe->mutex);
			REQUIRE(pipe->disconnects.size() == 1);
			CHECK(pipe->disconnects[0].first == 1);
			const size_t at = pipe->disconnects[0].second;
			REQUIRE(at >= 1);
			CHECK(pipe->sent[at - 1].peer == 1);
			CHECK(net::peek_type(pipe->sent[at - 1].bytes) == net::MessageType::Kick);
		}
		CHECK(wait_until([&] { return server.stats().peers == 1; }));
		CHECK(server.game_stats().clients == 1);
		CHECK(server.stats().step_errors == 0);
		server.stop();
	}

	TEST_CASE("host.session: a ClientSession started with no level connects, attaches a level, then plays") {
		auto pipe = std::make_shared<Pipe>();
		ServerConfig sc;
		sc.game = pipe_game();
		sc.transport = std::make_shared<PipeServerPort>(pipe);
		ServerHost server;
		std::string err;
		REQUIRE_MESSAGE(server.start(sc, &err), err);
		pipe->connect(0);

		ClientSession client;
		ClientSessionConfig cc;
		cc.name = "lobby";
		const core::Status started = client.start(cc, nullptr, std::make_unique<PipeClientTransport>(pipe, 0));
		REQUIRE_MESSAGE(started.ok(), (started.ok() ? std::string() : started.error().message));
		net::MatchInfo held;
		REQUIRE(wait_until([&] { return client.lobby_match(held); }));
		CHECK(held.slot == 0);
		CHECK(held.level == synthetic_level()->level);
		CHECK(held.motor_hash == motor_params_hash(synthetic_level()->motor));
		REQUIRE(wait_until([&] { return pipe->count_sent(0, net::MessageType::Snapshot) >= 3; }));
		auto frame = std::make_unique<PresentationFrame>();
		CHECK_FALSE(client.read_presentation(*frame));
		CHECK(client.stats().connected);
		CHECK(server.game_stats().slots.at(0).used);
		CHECK(server.game_stats().slots.at(0).life == 0);

		const core::Status attached = client.attach_level(synthetic_level());
		REQUIRE_MESSAGE(attached.ok(), (attached.ok() ? std::string() : attached.error().message));
		CHECK_FALSE(client.attach_level(synthetic_level()).ok());

		InputSample input;
		REQUIRE(wait_until([&] {
			client.read_presentation(*frame);
			return frame->valid && frame->own_curr.life > 0;
		}));
		const SoldierState spawn = frame->own_curr;
		input.life = spawn.life;
		input.yaw = yaw_radians(spawn.yaw);
		input.move_forward = 1.0f;
		REQUIRE(wait_until([&] {
			client.set_input(input);
			client.read_presentation(*frame);
			return frame->server_own_valid && dist(frame->server_own.position, spawn.position) > 1.0f &&
					dist(frame->own_curr.position, spawn.position) > 1.0f;
		}));
		const ClientStats stats = client.stats();
		CHECK(stats.connected);
		CHECK(stats.slot == 0);
		CHECK(stats.spawns == 1);
		CHECK(stats.commands_sent > 0);
		CHECK(server.game_stats().slots.at(0).commands_applied > 0);
		client.stop();
		server.stop();
	}
}

TEST_SUITE("host-server-netthread") {
	TEST_CASE("host.server: the tick thread blocked 6 s with peer timeouts of 2-4 s: the client stays connected and "
			  "the next snapshot arrives") {
		ServerConfig sc;
		sc.game = pipe_game();
		sc.peer_timeout_min_ms = 2000;
		sc.peer_timeout_max_ms = 4000;
		ServerHost server;
		std::string err;
		REQUIRE_MESSAGE(server.start(sc, &err), err);
		ClientSessionConfig cc;
		cc.port = server.port();
		cc.peer_timeout_min_ms = 2000;
		cc.peer_timeout_max_ms = 4000;
		ClientSession client;
		const core::Status started = client.start(cc, synthetic_level());
		REQUIRE_MESSAGE(started.ok(), (started.ok() ? std::string() : started.error().message));
		auto frame = std::make_unique<PresentationFrame>();
		REQUIRE(wait_until([&] {
			client.read_presentation(*frame);
			return frame->valid && frame->own_curr.life > 0;
		}));

		server.debug_stall(6000);
		REQUIRE(wait_until([&] { return server.stats().tick_stalls == 1; }));
		const auto stall_start = std::chrono::steady_clock::now();
		bool stayed = true;
		uint64_t early = 0, late = 0;
		while (seconds_since(stall_start) < 5.5) {
			const ClientStats s = client.stats();
			stayed = stayed && s.connected;
			if (early == 0 && seconds_since(stall_start) > 0.5) {
				early = s.snapshots_received;
			}
			late = s.snapshots_received;
			std::this_thread::sleep_for(16ms);
		}
		CHECK(stayed);
		CHECK(early > 0);
		CHECK(late == early);
		REQUIRE(wait_until([&] { return client.stats().snapshots_received > late; }));
		CHECK(client.stats().connected);
		CHECK(server.stats().peers == 1);
		CHECK(server.game_stats().clients == 1);
		client.stop();
		server.stop();
	}
}
