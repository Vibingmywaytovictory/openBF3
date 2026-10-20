#pragma once

#include <doctest/doctest.h>

#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/host/client_session.h"
#include "bf3/host/game_server.h"
#include "bf3/host/level_load.h"
#include "bf3/host/server_host.h"
#include "bf3/net/loopback_ping.h"
#include "bf3/net/netsim.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/spawn.h"
#include "bf3/sim/static_world.h"
#include "test_support.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace bf3::test::gunplay {

inline double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

inline std::string read_text(const std::filesystem::path &p_path) {
	std::ifstream in(p_path, std::ios::binary);
	std::ostringstream s;
	s << in.rdbuf();
	return s.str();
}

inline void add_quad(std::vector<sim::StaticTriangle> &r_out, const sim::Vec3 &p_a, const sim::Vec3 &p_b, const sim::Vec3 &p_c, const sim::Vec3 &p_d) {
	sim::StaticTriangle t;
	t.v = { p_a, p_b, p_c };
	r_out.push_back(t);
	t.v = { p_a, p_c, p_d };
	r_out.push_back(t);
}

#ifdef OPENBF3_MOTOR_TUNING_CFG

inline sim::MotorTuning shipped_motor_tuning() {
	const std::string text = read_text(OPENBF3_MOTOR_TUNING_CFG);
	REQUIRE_MESSAGE(!text.empty(), OPENBF3_MOTOR_TUNING_CFG);
	core::Result<sim::MotorTuning> t = sim::parse_motor_tuning(text);
	REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
	return *t;
}

inline core::gameplay::StatePoseSpeed motor_speed(core::gameplay::SoldierPose p_pose, float p_v, float p_f, float p_b, float p_side, float p_accel, float p_decel,
		float p_sprint_gain, float p_sprint) {
	core::gameplay::StatePoseSpeed s;
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

inline core::gameplay::SoldierDef synthetic_soldier() {
	using core::gameplay::SoldierPose;
	core::gameplay::SoldierDef d;
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
	const std::vector<core::gameplay::CurvePoint> throttle = { { 0, 0 }, { 0.1f, 0 }, { 1, 1 } };
	core::gameplay::PoseDef stand;
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
	core::gameplay::PoseDef crouch = stand;
	crouch.pose = SoldierPose::Crouch;
	crouch.height = 1.15f;
	crouch.eye_height = 1.0f;
	crouch.box_min_expand = { 0.1f, 0.0f, 0.6f };
	crouch.box_max_expand = { 0.2f, 0.1f, 0.4f };
	crouch.transition_to = {};
	crouch.transition_to[0] = 0.3f;
	crouch.transition_to[2] = 0.5f;
	core::gameplay::PoseDef prone = stand;
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
	core::gameplay::MovementStateDef ground;
	ground.type = "OnGroundStateData";
	ground.poses = { motor_speed(SoldierPose::Stand, 4, 1, 0.75f, 0.75f, 0.35f, -15, 0.25f, 1.625f), motor_speed(SoldierPose::Crouch, 2, 1, 0.8f, 0.8f, 0.35f, -15, 1, 0),
		motor_speed(SoldierPose::Prone, 0.9f, 1, 0.5f, 0.7f, 0.35f, -15, 1, 0) };
	ground.scalars = { { "AllowedDistanceFromGround", 0.5 }, { "GroundHugging", 1 } };
	core::gameplay::MovementStateDef jump;
	jump.type = "JumpStateData";
	jump.poses = { motor_speed(SoldierPose::Stand, 4, 1, 1, 1, 0, 0, 0, 1.2f) };
	jump.scalars = { { "JumpHeight", 0.6 } };
	core::gameplay::MovementStateDef air;
	air.type = "InAirStateData";
	for (SoldierPose p : { SoldierPose::Stand, SoldierPose::Crouch, SoldierPose::Prone }) {
		air.poses.push_back(motor_speed(p, 6, 1, 0.5f, 0.5f, 0.1f, 0, 0.1f, 1.5f));
	}
	air.scalars = { { "FreeFallVelocity", 16 } };
	core::gameplay::MovementStateDef falling;
	falling.type = "FallingStateData";
	for (SoldierPose p : { SoldierPose::Stand, SoldierPose::Crouch, SoldierPose::Prone }) {
		falling.poses.push_back(motor_speed(p, 15, 1, 1, 1, 0.05f, 0, 0, 1));
	}
	d.states = { ground, jump, air, falling };
	d.fall_damage_vertical = { { 0, 0 }, { 13.9f, 0 }, { 14, 20 }, { 22, 40 }, { 30, 100 } };
	d.fall_damage_horizontal = d.fall_damage_vertical;
	return d;
}

inline std::shared_ptr<const host::SharedLevel> synthetic_floor() {
	static std::shared_ptr<const host::SharedLevel> s_level;
	if (s_level) {
		return s_level;
	}
	std::vector<sim::StaticTriangle> t;
	for (float x = -120.0f; x < 120.0f; x += 8.0f) {
		for (float z = -120.0f; z < 120.0f; z += 8.0f) {
			add_quad(t, { x, 0, z }, { x, 0, z + 8.0f }, { x + 8.0f, 0, z + 8.0f }, { x + 8.0f, 0, z });
		}
	}
	core::Result<std::shared_ptr<const sim::StaticWorldShapes>> shapes = sim::build_static_world(std::span<const sim::StaticTriangle>(t));
	REQUIRE_MESSAGE(shapes.ok(), (shapes.ok() ? std::string() : shapes.error().message));
	std::vector<sim::SpawnPoint> points;
	for (uint32_t i = 0; i < 12; ++i) {
		sim::SpawnPoint p;
		const float a = float(i) * 0.5235988f;
		p.position = { 40.0f * std::sin(a), 0.05f, 40.0f * std::cos(a) };
		p.yaw = uint16_t(i * 5461u);
		p.index = i;
		points.push_back(p);
	}
	core::Result<sim::MotorParams> params = sim::make_motor_params(synthetic_soldier(), shipped_motor_tuning());
	REQUIRE_MESSAGE(params.ok(), (params.ok() ? std::string() : params.error().message));
	if (!shapes.ok() || !params.ok()) {
		return nullptr;
	}
	s_level = host::synthetic_shared_level(*shapes, points, *params);
	return s_level;
}

struct Metro {
	std::unique_ptr<core::InstallReader> reader;
	std::shared_ptr<core::Install> install;
	core::InstallSnapshot before;
	std::shared_ptr<const host::SharedLevel> level;
	std::filesystem::path cache_root;
	double seconds = 0;
};

inline Metro *metro() {
	static std::unique_ptr<Metro> s_metro;
	static bool s_tried = false;
	if (s_tried) {
		if (s_metro == nullptr) {
			bf3::test::skip_install_case("no BF3 install (see the first gunplay case)");
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
	m->cache_root = std::filesystem::temp_directory_path() /
			("openbf3-gunplay-" + std::to_string(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())));
	std::error_code ec;
	std::filesystem::remove_all(m->cache_root, ec);
	core::Cache cache;
	core::CacheOptions co;
	co.root = m->cache_root;
	co.guard = m->reader->write_guard();
	std::string detail;
	REQUIRE_MESSAGE(cache.open(std::move(co), &detail) == core::CacheStatus::Ok, detail);
	host::SharedLoadOptions options;
	options.cache = &cache;
	options.tuning = shipped_motor_tuning();
	host::LoadProgress progress;
	const auto start = std::chrono::steady_clock::now();
	core::Result<host::LevelDatabase> db = host::open_level_database(m->install, "MP_Subway", "TeamDeathMatch0", 0, &progress);
	REQUIRE_MESSAGE(db.ok(), (db.ok() ? std::string() : db.error().message));
	core::Result<std::shared_ptr<const host::SharedLevel>> level = host::load_shared_level(*db, options, &progress);
	REQUIRE_MESSAGE(level.ok(), (level.ok() ? std::string() : level.error().message));
	m->seconds = seconds_since(start);
	m->level = *level;
	cache.close();
	std::printf("gunplay fixture: Metro loaded in %.2f s (%zu spawns)\n", m->seconds, m->level->spawns.size());
	s_metro = std::move(m);
	return s_metro.get();
}

inline void check_install_unchanged(Metro &r_metro) {
	const core::SnapshotDiff diff = core::diff_snapshots(r_metro.before, r_metro.reader->snapshot());
	CHECK_MESSAGE(diff.clean(), diff.summary());
	std::printf("install.unchanged: %s (%zu files)\n", diff.clean() ? "clean" : diff.summary().c_str(), r_metro.before.files.size());
	std::error_code ec;
	std::filesystem::remove_all(r_metro.cache_root, ec);
}
#endif

struct SessionOptions {
	std::string netsim = "off";
	uint32_t max_slots = uint32_t(net::kMaxSlots);
	uint32_t walkers = 0;
	bool dev_commands = true;
	uint64_t seed = 1;
	int32_t clock_offset = 0;
	std::string client_mechanics_cfg;

	std::function<void(host::GameServerConfig &)> configure_server;
	std::function<void(host::ClientSessionConfig &)> configure_client;
};

struct Session {
	std::shared_ptr<const host::SharedLevel> level;
	host::ServerHost server;
	host::ClientSession client;
	host::InputSample input;
	std::unique_ptr<host::PresentationFrame> frame = std::make_unique<host::PresentationFrame>();
	bool adopt = true;
	float adopt_yaw = NAN;
	uint32_t next_debug_id = 1;

	~Session() {
		client.stop();
		server.stop();
	}
};

inline std::unique_ptr<Session> start_session(std::shared_ptr<const host::SharedLevel> p_level, const SessionOptions &p_options = {}) {
	auto s = std::make_unique<Session>();
	s->level = p_level;
	auto game = std::make_shared<host::GameServerConfig>();
	game->level = p_level;
	game->max_slots = p_options.max_slots;
	game->walkers = p_options.walkers;
	game->dev_commands = p_options.dev_commands;
	game->seed = p_options.seed;
	if (p_options.configure_server) {
		p_options.configure_server(*game);
	}
	host::ServerConfig sc;
	sc.game = game;
	std::string err;
	const bool started = s->server.start(sc, &err);
	REQUIRE_MESSAGE(started, err);
	if (!started) {
		return nullptr;
	}
	host::ClientSessionConfig cc;
	cc.port = s->server.port();
	core::Result<net::NetSimConfig> ns = net::parse_netsim(p_options.netsim);
	REQUIRE_MESSAGE(ns.ok(), (ns.ok() ? std::string() : ns.error().message));
	if (!ns.ok()) {
		return nullptr;
	}
	cc.netsim = *ns;
	cc.netsim.seed = p_options.seed * 7919u + 1u;
	cc.request_dev_commands = true;
	cc.debug_hash_compare = true;
	cc.initial_clock_offset_ticks = p_options.clock_offset;
	if (!p_options.client_mechanics_cfg.empty()) {
		cc.mechanics_cfg = p_options.client_mechanics_cfg;
	}
	if (p_options.configure_client) {
		p_options.configure_client(cc);
	}
	const core::Status st = s->client.start(cc, p_level);
	REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
	if (!st.ok()) {
		return nullptr;
	}
	return s;
}

inline bool poll(Session &r_session) {
	const bool fresh = r_session.client.read_presentation(*r_session.frame);
	const host::PresentationFrame &f = *r_session.frame;
	if (r_session.adopt && f.valid && f.own_curr.life > r_session.input.life) {
		r_session.input.life = f.own_curr.life;
		r_session.input.yaw = std::isnan(r_session.adopt_yaw) ? sim::yaw_radians(f.own_curr.yaw) : r_session.adopt_yaw;
		r_session.input.pitch = sim::pitch_radians(f.own_curr.pitch);
	}
	r_session.client.set_input(r_session.input);
	return fresh;
}

inline void run_for(Session &r_session, double p_seconds, const std::function<void(bool, uint64_t, double)> &p_each = {}, int p_sleep_ms = 4) {
	const auto start = std::chrono::steady_clock::now();
	for (;;) {
		const double t = seconds_since(start);
		if (t >= p_seconds) {
			break;
		}
		const bool fresh = poll(r_session);
		if (p_each) {
			p_each(fresh, net::monotonic_ns(), t);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(p_sleep_ms));
	}
}

inline bool wait_spawned(Session &r_session, double p_timeout_s = 5.0) {
	const auto start = std::chrono::steady_clock::now();
	while (seconds_since(start) < p_timeout_s) {
		poll(r_session);
		if (r_session.frame->valid && r_session.frame->own_curr.life > 0 && r_session.input.life == r_session.frame->own_curr.life) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}
	return false;
}

inline std::optional<net::DebugReport> wait_report(Session &r_session, uint32_t p_id, double p_timeout_s = 10.0) {
	const auto start = std::chrono::steady_clock::now();
	while (seconds_since(start) < p_timeout_s) {
		poll(r_session);
		net::DebugReport r;
		while (r_session.client.take_debug_report(r)) {
			if (r.id == p_id) {
				return r;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}
	return std::nullopt;
}

inline std::optional<net::DebugReport> dev_request(Session &r_session, net::DebugRequest p_request, double p_timeout_s = 10.0) {
	p_request.id = r_session.next_debug_id++;
	r_session.client.send_debug(p_request);
	return wait_report(r_session, p_request.id, p_timeout_s);
}

inline std::optional<net::DebugReport> teleport(Session &r_session, const sim::Vec3 &p_position, uint16_t p_yaw, double p_timeout_s = 10.0) {
	net::DebugRequest q;
	q.kind = net::DebugKind::Teleport;
	q.position = p_position;
	q.yaw = p_yaw;
	return dev_request(r_session, q, p_timeout_s);
}

inline std::optional<net::DebugReport> spawn_at(Session &r_session, uint32_t p_index, double p_timeout_s = 10.0) {
	net::DebugRequest q;
	q.kind = net::DebugKind::SpawnAt;
	q.index = p_index;
	return dev_request(r_session, q, p_timeout_s);
}

inline std::optional<net::DebugReport> trace(Session &r_session, uint32_t p_ticks, double p_timeout_s = 0.0) {
	net::DebugRequest q;
	q.kind = net::DebugKind::Trace;
	q.ticks = p_ticks;
	return dev_request(r_session, q, p_timeout_s > 0.0 ? p_timeout_s : 10.0 + double(p_ticks) / double(sim::kSimHz));
}

inline std::optional<net::DebugReport> spawn_target(Session &r_session, uint8_t p_slot, const sim::Vec3 &p_position, uint16_t p_yaw,
		sim::Pose p_pose = sim::Pose::Stand, double p_timeout_s = 10.0) {
	net::DebugRequest q;
	q.kind = net::DebugKind::SpawnTarget;
	q.slot = p_slot;
	q.position = p_position;
	q.yaw = p_yaw;
	q.pose = p_pose;
	return dev_request(r_session, q, p_timeout_s);
}

inline std::optional<net::DebugReport> target_info(Session &r_session, uint8_t p_slot, double p_timeout_s = 10.0) {
	net::DebugRequest q;
	q.kind = net::DebugKind::TargetInfo;
	q.slot = p_slot;
	return dev_request(r_session, q, p_timeout_s);
}

inline sim::Vec3 own_eye(const host::PresentationFrame &p_frame) {
	const float t = float(p_frame.tick_fraction);
	sim::Vec3 e;
	for (size_t k = 0; k < 3; ++k) {
		e[k] = p_frame.own_prev.position[k] + (p_frame.own_curr.position[k] - p_frame.own_prev.position[k]) * t;
	}
	e[1] += p_frame.eye_height_prev + (p_frame.eye_height_curr - p_frame.eye_height_prev) * t;
	return e;
}

}

#include "bf3/host/gunplay.h"
#include "bf3/host/target_bots.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/weapons/hit_target.h"
#include "bf3/sim/weapons/weapon_input.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <mutex>

namespace bf3::test::gunplay {

#ifdef OPENBF3_MOTOR_TUNING_CFG

inline Metro *metro_gunplay() {
	static std::unique_ptr<Metro> s_metro;
	static bool s_tried = false;
	if (s_tried) {
		if (s_metro == nullptr) {
			bf3::test::skip_install_case("no BF3 install (see the first gunplay case)");
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
	m->cache_root = std::filesystem::temp_directory_path() /
			("openbf3-gunplay-sv-" + std::to_string(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())));
	std::error_code ec;
	std::filesystem::remove_all(m->cache_root, ec);
	core::Cache cache;
	core::CacheOptions co;
	co.root = m->cache_root;
	co.guard = m->reader->write_guard();
	std::string detail;
	REQUIRE_MESSAGE(cache.open(std::move(co), &detail) == core::CacheStatus::Ok, detail);
	host::SharedLoadOptions options;
	options.cache = &cache;
	options.tuning = shipped_motor_tuning();
	options.gunplay = true;
	host::LoadProgress progress;
	const auto start = std::chrono::steady_clock::now();
	core::Result<host::LevelDatabase> db = host::open_level_database(m->install, "MP_Subway", "TeamDeathMatch0", 0, &progress);
	REQUIRE_MESSAGE(db.ok(), (db.ok() ? std::string() : db.error().message));
	core::Result<std::shared_ptr<const host::SharedLevel>> level = host::load_shared_level(*db, options, &progress);
	REQUIRE_MESSAGE(level.ok(), (level.ok() ? std::string() : level.error().message));
	m->seconds = seconds_since(start);
	m->level = *level;
	cache.close();
	const host::GunplayLevel *g = m->level->gunplay.get();
	REQUIRE(g != nullptr);
	std::printf("gunplay fixture (sv): Metro with gunplay loaded in %.2f s (gunplay %.2f s: weapons %.2f, kits %.2f, capsules %.2f; %zu defs, "
				"capsule table %s, %zu notes)\n",
			m->seconds, m->level->seconds_gunplay, g->seconds_weapons, g->seconds_kits, g->seconds_capsules, g->defs.size(),
			g->capsule_standin ? "STAND-IN" : "from the retail clips", g->notes.size());
	s_metro = std::move(m);
	return s_metro.get();
}
#endif

inline uint32_t select_slot_field(uint8_t p_slot) {
	return uint32_t(p_slot + 1u) << sim::weapons::weapon_buttons::kSlotShift;
}
inline uint32_t fire_mode_field(uint8_t p_counter) {
	return (uint32_t(p_counter) << sim::weapons::weapon_buttons::kFireModeShift) & sim::weapons::weapon_buttons::kFireModeMask;
}

inline void aim_at(const sim::Vec3 &p_from, const sim::Vec3 &p_to, float &r_yaw, float &r_pitch) {
	const double dx = double(p_to[0]) - double(p_from[0]), dy = double(p_to[1]) - double(p_from[1]), dz = double(p_to[2]) - double(p_from[2]);
	r_yaw = float(std::atan2(dx, dz));
	r_pitch = float(std::atan2(dy, std::sqrt(dx * dx + dz * dz)));
}

struct WeaponTickRecord {
	uint8_t slot = 0;
	sim::Command command;
	sim::weapons::SoldierWeaponsState after;
	std::vector<sim::weapons::ShotEvent> shots;
	std::vector<sim::weapons::TimedWeaponEvent> timed;
	uint32_t events = 0;
};
struct ImpactRecord {
	uint32_t tick = 0;
	sim::weapons::ImpactEvent impact;
};
struct DamageRecord {
	uint32_t tick = 0;
	sim::weapons::DamageEvent event;
	bool applied = false;
	float health_after = 0;
};

class GunplayRecorder final : public host::GunplayObserver {
public:
	static constexpr size_t kMaxRecords = size_t(1) << 20;

	explicit GunplayRecorder(uint8_t p_watch = 0xFF) :
			watch_(p_watch) {}

	void weapon_tick(uint8_t p_slot, const sim::Command &p_command, const sim::weapons::SoldierWeaponsState &p_after,
			const sim::weapons::WeaponTickOutput &p_out) override {
		std::lock_guard<std::mutex> lock(mutex_);
		if (watch_ != 0xFF && p_slot != watch_) {
			return;
		}
		if (ticks_.size() >= kMaxRecords) {
			++dropped_;
			return;
		}
		WeaponTickRecord r;
		r.slot = p_slot;
		r.command = p_command;
		r.after = p_after;
		r.shots.assign(p_out.shots.begin(), p_out.shots.begin() + std::min<size_t>(p_out.shot_count, p_out.shots.size()));
		r.timed.assign(p_out.timed.begin(), p_out.timed.begin() + std::min<size_t>(p_out.timed_count, p_out.timed.size()));
		r.events = p_out.events;
		ticks_.push_back(std::move(r));
	}
	void projectiles(uint32_t p_tick, const sim::weapons::ProjectileStepOutput &p_out, std::span<const sim::weapons::Projectile> p_live) override {
		(void)p_live;
		std::lock_guard<std::mutex> lock(mutex_);
		for (const sim::weapons::ImpactEvent &i : p_out.impacts) {
			if (impacts_.size() >= kMaxRecords) {
				++dropped_;
				break;
			}
			impacts_.push_back({ p_tick, i });
		}
	}
	void damage(uint32_t p_tick, const sim::weapons::DamageEvent &p_event, bool p_applied, float p_health_after) override {
		std::lock_guard<std::mutex> lock(mutex_);
		if (damage_.size() >= kMaxRecords) {
			++dropped_;
			return;
		}
		damage_.push_back({ p_tick, p_event, p_applied, p_health_after });
	}

	void set_watch(uint8_t p_slot) {
		std::lock_guard<std::mutex> lock(mutex_);
		watch_ = p_slot;
	}
	void clear() {
		std::lock_guard<std::mutex> lock(mutex_);
		ticks_.clear();
		impacts_.clear();
		damage_.clear();
	}
	std::vector<WeaponTickRecord> ticks() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return ticks_;
	}
	std::vector<sim::weapons::ShotEvent> shots(uint8_t p_slot) const {
		std::lock_guard<std::mutex> lock(mutex_);
		std::vector<sim::weapons::ShotEvent> out;
		for (const WeaponTickRecord &r : ticks_) {
			if (r.slot == p_slot) {
				out.insert(out.end(), r.shots.begin(), r.shots.end());
			}
		}
		return out;
	}
	size_t shot_count(uint8_t p_slot) const {
		std::lock_guard<std::mutex> lock(mutex_);
		size_t n = 0;
		for (const WeaponTickRecord &r : ticks_) {
			n += r.slot == p_slot ? r.shots.size() : 0;
		}
		return n;
	}
	std::optional<WeaponTickRecord> last(uint8_t p_slot) const {
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto it = ticks_.rbegin(); it != ticks_.rend(); ++it) {
			if (it->slot == p_slot) {
				return *it;
			}
		}
		return std::nullopt;
	}
	std::vector<ImpactRecord> impacts() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return impacts_;
	}
	std::vector<DamageRecord> damage() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return damage_;
	}
	uint64_t dropped() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return dropped_;
	}

private:
	mutable std::mutex mutex_;
	uint8_t watch_ = 0xFF;
	std::vector<WeaponTickRecord> ticks_;
	std::vector<ImpactRecord> impacts_;
	std::vector<DamageRecord> damage_;
	uint64_t dropped_ = 0;
};

inline bool poll_until(Session &r_session, const std::function<bool()> &p_done, double p_timeout_s) {
	const auto start = std::chrono::steady_clock::now();
	while (seconds_since(start) < p_timeout_s) {
		poll(r_session);
		if (p_done()) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return p_done();
}

inline uint32_t send_request(Session &r_session, net::DebugRequest p_request) {
	p_request.id = r_session.next_debug_id++;
	r_session.client.send_debug(p_request);
	return p_request.id;
}

inline bool weapon_ready(const GunplayRecorder &p_recorder, uint8_t p_slot) {
	const std::optional<WeaponTickRecord> r = p_recorder.last(p_slot);
	return r && r->after.current().phase == sim::weapons::WeaponPhase::Ready;
}

}
