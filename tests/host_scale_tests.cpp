// Suites host-scale-unit (no data) and host-scale (install; exits 77 without one): spike S1 server scale
// (docs/design/s1.md §1 G1 / G3, §6, §7, §8, §10).
//
// host-scale-unit: walker bots on the real GameServer over synthetic worlds: 64 walkers for 60 s reach goals and walk,
// the run reproduces (a hash of every state), the staggered thinks per tick, the per-tick parts sum to the whole
// (encode 0 between snapshots), the PathService budget, the spawn filter (excluded points, teams merged, synthetic
// points), a bot outside the main component walks goals of its own component, stuck detection on a door the motor
// cannot pass (repath, then a new goal), the tail split names an injected slow part, the initial spawns run in init.
// host-scale: run_scale_bench on MP_Subway and MP_007 ConquestLarge0 (G1 p99 <= 8 ms with the validity condition, G3
// numbers), the observers = 16 row on Metro (reported), install unchanged. RUN_SERIAL; timings are noisy on a shared
// machine and the acceptance step reruns this on a quiet one.
#include <doctest/doctest.h>

#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/mode_areas.h"
#include "bf3/host/bot_fill.h"
#include "bf3/host/game_server.h"
#include "bf3/host/level_load.h"
#include "bf3/host/scale_bench.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/nav.h"
#include "bf3/sim/nav_query.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/spawn.h"
#include "bf3/sim/static_world.h"
#include "test_support.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace bf3;
using namespace bf3::host;
using namespace bf3::sim;
namespace gameplay = bf3::core::gameplay;

namespace {

// ---- synthetic soldier (Metro's numbers, as host_session_tests.cpp) ---------------------------------------------

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

// ---- synthetic geometry ------------------------------------------------------------------------------------------

void add_quad(std::vector<StaticTriangle> &r_out, const Vec3 &p_a, const Vec3 &p_b, const Vec3 &p_c, const Vec3 &p_d) {
	StaticTriangle t;
	t.v = { p_a, p_b, p_c };
	r_out.push_back(t);
	t.v = { p_a, p_c, p_d };
	r_out.push_back(t);
}

void add_floor(std::vector<StaticTriangle> &r_out, float p_x0, float p_z0, float p_x1, float p_z1, float p_y) {
	for (float x = p_x0; x < p_x1; x += 16.0f) {
		for (float z = p_z0; z < p_z1; z += 16.0f) {
			const float xe = std::min(x + 16.0f, p_x1), ze = std::min(z + 16.0f, p_z1);
			add_quad(r_out, { x, p_y, z }, { x, p_y, ze }, { xe, p_y, ze }, { xe, p_y, z });
		}
	}
}

// Top and sides, no bottom (a block standing on the ground).
void add_block(std::vector<StaticTriangle> &r_out, const Vec3 &p_lo, const Vec3 &p_hi) {
	const float x0 = p_lo[0], y0 = p_lo[1], z0 = p_lo[2], x1 = p_hi[0], y1 = p_hi[1], z1 = p_hi[2];
	add_quad(r_out, { x0, y1, z0 }, { x0, y1, z1 }, { x1, y1, z1 }, { x1, y1, z0 });
	add_quad(r_out, { x0, y0, z0 }, { x0, y0, z1 }, { x0, y1, z1 }, { x0, y1, z0 });
	add_quad(r_out, { x1, y0, z0 }, { x1, y1, z0 }, { x1, y1, z1 }, { x1, y0, z1 });
	add_quad(r_out, { x0, y0, z0 }, { x0, y1, z0 }, { x1, y1, z0 }, { x1, y0, z0 });
	add_quad(r_out, { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 });
}

// A wall along x = p_x (0.3 m thick, 3 m tall) over [z0, z1] with doors (z ranges).
void add_wall_x(std::vector<StaticTriangle> &r_out, float p_x, float p_z0, float p_z1, const std::vector<std::array<float, 2>> &p_doors) {
	float z = p_z0;
	for (const std::array<float, 2> &d : p_doors) {
		add_block(r_out, { p_x, 0, z }, { p_x + 0.3f, 3.0f, d[0] });
		z = d[1];
	}
	add_block(r_out, { p_x, 0, z }, { p_x + 0.3f, 3.0f, p_z1 });
}

std::vector<NavTileKey> tiles_range(int32_t p_x0, int32_t p_z0, int32_t p_x1, int32_t p_z1) {
	std::vector<NavTileKey> out;
	for (int32_t x = p_x0; x <= p_x1; ++x) {
		for (int32_t z = p_z0; z <= p_z1; ++z) {
			out.push_back({ x, z });
		}
	}
	std::sort(out.begin(), out.end());
	return out;
}

// A synthetic level: the motor's world (p_motor_tris), the nav baked from p_nav_tris (normally the same), spawn
// points and flags; the BotLevel built the way load_bot_level builds it.
struct Synthetic {
	std::shared_ptr<const SharedLevel> shared;
	std::shared_ptr<BotLevel> bots;
};

Synthetic make_synthetic(const std::vector<StaticTriangle> &p_motor_tris, const std::vector<StaticTriangle> &p_nav_tris, const std::vector<NavTileKey> &p_tiles,
		const std::vector<SpawnPoint> &p_spawns, const std::vector<Vec3> &p_flags) {
	Synthetic out;
	core::Result<std::shared_ptr<const StaticWorldShapes>> shapes = build_static_world(std::span<const StaticTriangle>(p_motor_tris));
	REQUIRE_MESSAGE(shapes.ok(), (shapes.ok() ? std::string() : shapes.error().message));
	const MotorParams motor = synthetic_params();
	out.shared = synthetic_shared_level(*shapes, p_spawns, motor);
	auto bl = std::make_shared<BotLevel>();
	bl->level = "synthetic";
	bl->mode = "test";
	NavBakeOptions o;
	o.params = nav_params_from_motor(motor);
	o.link_shapes = *shapes;
	core::Result<std::shared_ptr<const NavWorld>> nav = bake_nav_world(std::span<const StaticTriangle>(p_nav_tris), p_tiles, o);
	REQUIRE_MESSAGE(nav.ok(), (nav.ok() ? std::string() : nav.error().message));
	bl->nav = *nav;
	bl->components = nav_components(*bl->nav);
	bl->main_component = bl->components.largest;
	bl->region = p_tiles;
	std::vector<core::level::ModeObjectiveDef> objectives;
	for (const Vec3 &f : p_flags) {
		core::level::ModeObjectiveDef d;
		d.kind = core::level::ModeObjectiveDef::Kind::Flag;
		d.position = f;
		objectives.push_back(d);
	}
	core::Result<std::unique_ptr<CollisionWorld>> world = CollisionWorld::create(*shapes);
	REQUIRE(world.ok());
	SoldierMotor m;
	REQUIRE(m.init(**world, motor).ok());
	BotRowsResult rows = bot_point_rows(*bl->nav, bl->components, bl->main_component, objectives, p_spawns, m, **world);
	bl->points = rows.points;
	bl->objectives = rows.objectives;
	core::Result<std::unique_ptr<NavQuery>> q = NavQuery::create(bl->nav);
	REQUIRE(q.ok());
	bl->spawns = make_bot_spawns(bl->points, p_spawns, bl->components, bl->main_component, **q, m, **world, 64, 1);
	out.bots = bl;
	return out;
}

// The arena: 256 m x 256 m, a wall at x = 128 with three 2 m doors, a 3 m high island [196, 236]^2 (its own SCC: it
// drops to the floor, nothing leads up), 40 spawn points on the floor (teams 1 and 2) and 4 on the island (team 1).
std::vector<StaticTriangle> arena() {
	std::vector<StaticTriangle> t;
	// Floor around the island's footprint (no floor under it: Recast sees surfaces, not volumes).
	add_floor(t, 0, 0, 256, 196, 0.0f);
	add_floor(t, 0, 196, 196, 256, 0.0f);
	add_floor(t, 236, 196, 256, 256, 0.0f);
	add_floor(t, 196, 236, 236, 256, 0.0f);
	add_wall_x(t, 127.9f, 0.1f, 255.9f, { { 40.0f, 42.0f }, { 120.0f, 122.0f }, { 200.0f, 202.0f } });
	add_block(t, { 196, 0, 196 }, { 236, 3.0f, 236 });
	return t;
}

std::vector<SpawnPoint> arena_spawns(bool p_main, bool p_island, int p_main_team = -1) {
	std::vector<SpawnPoint> out;
	if (p_main) {
		for (int i = 0; i < 40; ++i) {
			SpawnPoint p;
			p.position = { 12.0f + 24.0f * float(i % 5) + (i >= 20 ? 128.0f : 0.0f), 0.05f, 12.0f + 40.0f * float((i % 20) / 5) };
			p.yaw = uint16_t(i * 1777u);
			p.team = p_main_team >= 0 ? p_main_team : 1 + (i % 2);
			p.index = uint32_t(out.size());
			out.push_back(p);
		}
	}
	if (p_island) {
		for (int i = 0; i < 4; ++i) {
			SpawnPoint p;
			p.position = { 206.0f + 20.0f * float(i % 2), 3.05f, 206.0f + 20.0f * float(i / 2) };
			p.team = 1;
			p.index = uint32_t(out.size());
			out.push_back(p);
		}
	}
	return out;
}

void connect_observer(GameServer &r_server, uint32_t p_peer, const SharedLevel &p_level) {
	r_server.on_connect(p_peer);
	net::Hello hello;
	hello.name = "observer";
	const std::vector<uint8_t> h = net::encode_hello(hello);
	r_server.on_receive(p_peer, net::kChannelReliable, h);
	net::Ready ready;
	ready.source_digest = p_level.source_digest;
	const std::vector<uint8_t> r = net::encode_ready(ready);
	r_server.on_receive(p_peer, net::kChannelReliable, r);
}

uint64_t fnv(uint64_t p_h, const void *p_data, size_t p_size) {
	const uint8_t *b = static_cast<const uint8_t *>(p_data);
	for (size_t i = 0; i < p_size; ++i) {
		p_h = (p_h ^ b[i]) * 0x100000001B3ull;
	}
	return p_h;
}

struct ArenaRun {
	uint64_t hash = 0xcbf29ce484222325ull;
	std::vector<double> metres;
	BotFillStats bots;
	uint32_t thinks_min = UINT32_MAX, thinks_max = 0;
	uint32_t iterations_max = 0;
	uint64_t parts_ok = 0, parts_ticks = 0, encode_between = 0, encode_nonzero_between = 0;
	uint32_t alive_after_init = 0;
	uint64_t ticks_with_spawns = 0;
	bool any_on_island_at_spawn = false;
};

ArenaRun run_arena(const Synthetic &p_level, uint32_t p_bots, double p_seconds, uint64_t p_seed, bool p_spawn_in_main, uint32_t p_iterations = 4096) {
	ArenaRun out;
	GameServer server;
	GameServerConfig config;
	config.level = p_level.shared;
	config.max_slots = 128;
	config.seed = p_seed;
	auto fill = std::make_shared<BotFillConfig>();
	fill->level = p_level.bots;
	fill->count = p_bots;
	fill->spawn_in_main = p_spawn_in_main;
	fill->paths.iterations_per_tick = p_iterations;
	config.bots = fill;
	REQUIRE(server.init(config).ok());
	{
		const GameServerStats s = server.stats();
		out.alive_after_init = s.soldiers;
		for (const SlotStats &slot : s.slots) {
			if (slot.used && !slot.remote && slot.state.position[1] > 2.0f) {
				out.any_on_island_at_spawn = true;
			}
		}
	}
	connect_observer(server, 7, *p_level.shared);
	std::vector<Vec3> last(p_bots);
	std::vector<uint32_t> life(p_bots, 0);
	out.metres.assign(p_bots, 0.0);
	for (uint32_t b = 0; b < p_bots; ++b) {
		last[b] = server.soldier(b)->position;
		life[b] = server.soldier(b)->life;
	}
	std::vector<OutDatagram> sent;
	const uint64_t ticks = uint64_t(p_seconds * kSimHz);
	for (uint64_t t = 0; t < ticks; ++t) {
		server.tick(sent);
		sent.clear();
		const GameServerTickTimes &tt = server.last_tick_times();
		const double parts = tt.commands_ms + tt.sources_ms + tt.step_ms + tt.rules_ms + tt.encode_ms;
		++out.parts_ticks;
		out.parts_ok += (parts <= tt.tick_ms * 1.0001 + 1e-6 && parts >= 0.95 * tt.tick_ms) ? 1 : 0;
		if (tt.tick % 2 != 0) {
			++out.encode_between;
			out.encode_nonzero_between += tt.encode_ms != 0.0 || tt.snapshots != 0 ? 1 : 0;
		}
		out.ticks_with_spawns += tt.spawns > 0 ? 1 : 0;
		const GameServerStats s = server.stats();
		if (t >= 60) {
			out.thinks_min = std::min(out.thinks_min, s.bots.thinks_last_tick);
			out.thinks_max = std::max(out.thinks_max, s.bots.thinks_last_tick);
		}
		out.iterations_max = std::max(out.iterations_max, s.bots.path_iterations_last_tick);
		for (uint32_t b = 0; b < p_bots; ++b) {
			const SoldierState *st = server.soldier(b);
			REQUIRE(st != nullptr);
			out.hash = fnv(out.hash, &st->position, sizeof(st->position));
			out.hash = fnv(out.hash, &st->yaw, sizeof(st->yaw));
			if (st->life == life[b]) {
				const float dx = st->position[0] - last[b][0], dz = st->position[2] - last[b][2];
				out.metres[b] += std::sqrt(dx * dx + dz * dz);
			}
			last[b] = st->position;
			life[b] = st->life;
		}
	}
	out.bots = server.stats().bots;
	return out;
}

} // namespace

TEST_SUITE("host-scale-unit") {
	TEST_CASE("host.scale.unit: 64 walkers on the GameServer for 60 s: goals reached, everyone walks, reproducible, staggered thinks, tick parts") {
		const std::vector<StaticTriangle> t = arena();
		const Synthetic level = make_synthetic(t, t, tiles_range(0, 0, 3, 3), arena_spawns(true, true), { { 60, 0, 60 }, { 190, 0, 60 }, { 60, 0, 190 } });
		// The spawn filter: the island's 4 points are another component.
		CHECK(level.bots->spawns.level_points == 44);
		CHECK(level.bots->spawns.in_main == 40);
		CHECK(level.bots->spawns.excluded == 4);
		CHECK(!level.bots->spawns.teams_merged);
		CHECK(level.bots->spawns.synthetic == 0);
		CHECK(level.bots->objectives.size() == 3 + 40);
		const ArenaRun a = run_arena(level, 64, 60.0, 11, true);
		std::vector<double> m = a.metres;
		std::sort(m.begin(), m.end());
		const sim::bots::WalkerStats &w = a.bots.walkers;
		std::printf("host.scale.unit arena: metres min %.0f median %.0f max %.0f; goals started %llu reached %llu abandoned %llu; paths %llu partial %llu "
					"failed %llu; stuck %llu; sprint ticks %llu crouch ticks %llu; thinks per tick %u..%u; path iterations per tick max %u\n",
				m.front(), m[m.size() / 2], m.back(), (unsigned long long)w.goals_started, (unsigned long long)w.goals_reached,
				(unsigned long long)w.goals_abandoned, (unsigned long long)w.path_requests, (unsigned long long)w.path_partial,
				(unsigned long long)w.path_failed, (unsigned long long)w.stuck_events, (unsigned long long)w.sprint_ticks,
				(unsigned long long)w.crouch_ticks, a.thinks_min, a.thinks_max, a.iterations_max);
		CHECK(a.alive_after_init == 64); // the initial spawns ran inside init
		CHECK(!a.any_on_island_at_spawn);
		CHECK(a.ticks_with_spawns == 1); // the observer's own spawn after its Ready; no bot died
		CHECK(m.front() >= 60.0); // every bot walked
		CHECK(w.goals_reached >= 64);
		CHECK(double(w.goals_abandoned) <= 0.1 * double(w.goals_started));
		CHECK(w.starts_outside_main == 0);
		CHECK(w.sprint_ticks > 0);
		CHECK(w.crouch_ticks > 0);
		CHECK(a.bots.bots == 64);
		// Staggered thinks: 64 bots over a 6-tick period are 10 or 11 per tick.
		CHECK(a.thinks_min >= 10);
		CHECK(a.thinks_max <= 11);
		CHECK(a.iterations_max <= 4096);
		// Parts sum to the whole within 5 % (allowing 1 % of ticks preempted between two timers on a shared machine).
		CHECK(double(a.parts_ok) >= 0.99 * double(a.parts_ticks));
		CHECK(a.encode_between > 0);
		CHECK(a.encode_nonzero_between == 0);
		// Reproducible: the same seed walks the same way.
		const ArenaRun b = run_arena(level, 64, 60.0, 11, true);
		CHECK(a.hash == b.hash);
		const ArenaRun c = run_arena(level, 64, 10.0, 12, true);
		CHECK(c.hash != a.hash);
	}

	TEST_CASE("host.scale.unit: the spawn filter merges teams without a point of their own and makes synthetic points when none is in main") {
		const std::vector<StaticTriangle> t = arena();
		const Synthetic one_team = make_synthetic(t, t, tiles_range(0, 0, 3, 3), arena_spawns(true, true, 1), {});
		CHECK(one_team.bots->spawns.teams_merged);
		CHECK(one_team.bots->spawns.in_main == 40);
		for (const SpawnPoint &p : one_team.bots->spawns.points) {
			CHECK(p.team == 0);
		}
		const Synthetic island_only = make_synthetic(t, t, tiles_range(0, 0, 3, 3), arena_spawns(false, true), {});
		CHECK(island_only.bots->spawns.in_main == 0);
		CHECK(island_only.bots->spawns.excluded == 4);
		CHECK(island_only.bots->spawns.synthetic == 64);
		REQUIRE(island_only.bots->spawns.points.size() == 64);
		for (const SpawnPoint &p : island_only.bots->spawns.points) {
			CHECK(p.position[1] < 1.0f); // on the floor (the main component), not on the island
			CHECK(p.team == 0);
			CHECK(p.index > 0xFFFF0000u);
		}
		// The bots then spawn at the synthetic points and walk.
		const ArenaRun r = run_arena(island_only, 16, 20.0, 3, true);
		CHECK(!r.any_on_island_at_spawn);
		CHECK(r.bots.walkers.goals_reached > 0);
	}

	TEST_CASE("host.scale.unit: a bot outside the main component draws goals from its own component (starts_outside_main) and keeps walking") {
		const std::vector<StaticTriangle> t = arena();
		const Synthetic level = make_synthetic(t, t, tiles_range(0, 0, 3, 3), arena_spawns(false, true), {});
		// spawn_in_main = false: the bots spawn where humans would, here only on the island.
		const ArenaRun r = run_arena(level, 4, 40.0, 5, false);
		CHECK(r.any_on_island_at_spawn);
		const sim::bots::WalkerStats &w = r.bots.walkers;
		std::printf("host.scale.unit island: starts outside main %llu, goals started %llu reached %llu abandoned %llu, metres min %.0f\n",
				(unsigned long long)w.starts_outside_main, (unsigned long long)w.goals_started, (unsigned long long)w.goals_reached,
				(unsigned long long)w.goals_abandoned, *std::min_element(r.metres.begin(), r.metres.end()));
		CHECK(w.starts_outside_main >= 4);
		CHECK(w.goals_reached >= 4);
		CHECK(*std::min_element(r.metres.begin(), r.metres.end()) >= 20.0);
	}

	TEST_CASE("host.scale.unit: stuck on a door the motor cannot pass (the nav has no blocker): repath, then a new goal") {
		// Two rooms joined by one 3 m door; the motor's world blocks the door, the nav's does not.
		std::vector<StaticTriangle> nav_tris;
		add_floor(nav_tris, 0, 0, 128, 64, 0.0f);
		add_wall_x(nav_tris, 63.9f, 0.1f, 63.9f, { { 30.0f, 33.0f } });
		std::vector<StaticTriangle> motor_tris = nav_tris;
		add_block(motor_tris, { 63.0f, 0, 29.9f }, { 65.2f, 3.0f, 33.1f });
		std::vector<SpawnPoint> spawns;
		for (int i = 0; i < 4; ++i) {
			SpawnPoint p;
			p.position = { 10.0f + 10.0f * float(i), 0.05f, 20.0f };
			p.team = 1 + (i % 2);
			p.index = uint32_t(i);
			spawns.push_back(p);
		}
		const Synthetic level = make_synthetic(motor_tris, nav_tris, tiles_range(0, 0, 1, 0), spawns, { { 100, 0, 32 } });
		const ArenaRun r = run_arena(level, 2, 60.0, 9, true);
		const sim::bots::WalkerStats &w = r.bots.walkers;
		std::printf("host.scale.unit dead end: stuck %llu, repaths %llu, goals started %llu reached %llu abandoned %llu\n", (unsigned long long)w.stuck_events,
				(unsigned long long)w.repaths, (unsigned long long)w.goals_started, (unsigned long long)w.goals_reached, (unsigned long long)w.goals_abandoned);
		CHECK(w.stuck_events >= 4);
		CHECK(w.repaths >= 3);
		CHECK(w.goals_abandoned >= 1);
		CHECK(w.goals_started >= 3); // after an abandoned goal the bot picks the next one
	}

	TEST_CASE("host.scale.unit: PathService never spends more than its budget; 64 requests at once count ticks at the budget") {
		const std::vector<StaticTriangle> t = arena();
		const Synthetic level = make_synthetic(t, t, tiles_range(0, 0, 3, 3), arena_spawns(true, false), {});
		core::Result<std::unique_ptr<NavQuery>> q = NavQuery::create(level.bots->nav);
		REQUIRE(q.ok());
		sim::bots::PathService::Options o;
		o.iterations_per_tick = 256;
		sim::bots::PathService paths(**q, o);
		uint64_t rng = 3;
		for (uint32_t slot = 0; slot < 64; ++slot) {
			NavPolyRef a = 0, b = 0;
			Vec3 pa{}, pb{};
			REQUIRE((*q)->random_point(rng, level.bots->components, level.bots->main_component, &a, &pa));
			REQUIRE((*q)->random_point(rng, level.bots->components, level.bots->main_component, &b, &pb));
			paths.request(slot, a, pa, b, pb);
		}
		CHECK(paths.pending() == 64);
		uint32_t answered = 0;
		std::vector<NavPolyRef> polys;
		for (int tick = 0; tick < 10000 && answered < 64; ++tick) {
			paths.run();
			CHECK(paths.stats().iterations_last <= 256);
			for (uint32_t slot = 0; slot < 64; ++slot) {
				const NavPathStatus st = paths.take(slot, polys);
				if (st != NavPathStatus::Idle) {
					++answered;
					CHECK((st == NavPathStatus::Done || st == NavPathStatus::Partial));
				}
			}
		}
		CHECK(answered == 64);
		CHECK(paths.stats().ticks_at_budget >= 1);
		CHECK(paths.stats().pending_max == 64);
		CHECK(paths.pending() == 0);
	}

	TEST_CASE("host.scale.unit: the tail split names an injected slow part; nearest-rank percentiles") {
		std::vector<GameServerTickTimes> ticks(1000);
		for (size_t i = 0; i < ticks.size(); ++i) {
			GameServerTickTimes &t = ticks[i];
			t.commands_ms = 0.01;
			t.sources_ms = 0.3;
			t.step_ms = 1.0;
			t.encode_ms = i % 2 == 0 ? 0.2 : 0.0;
			if (i % 50 == 7) {
				t.sources_ms = 6.0; // an injected slow brain tick, 2 % of the ticks (odd: no encode)
			}
			t.tick_ms = t.commands_ms + t.sources_ms + t.step_ms + t.encode_ms + 0.01;
		}
		std::vector<double> v;
		for (const GameServerTickTimes &t : ticks) {
			v.push_back(t.tick_ms);
		}
		const Percentiles p = percentiles_of(v);
		CHECK(p.p99 == doctest::Approx(7.02));
		const TailShares tail = tail_shares(ticks, p.p99);
		CHECK(tail.ticks == 20);
		CHECK(tail.sources > 0.8);
		CHECK(tail.step < 0.2);
		CHECK(tail.other < 0.01);
		const Percentiles q = percentiles_of({ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 });
		CHECK(q.p50 == 5);
		CHECK(q.p95 == 10);
		CHECK(q.max == 10);
		CHECK(q.mean == doctest::Approx(5.5));
	}
}

// ---- host-scale: Metro and Caspian (install; exits 77 without one) -----------------------------------------------

namespace {

struct Install {
	std::unique_ptr<core::InstallReader> reader;
	std::shared_ptr<core::Install> install;
	core::InstallSnapshot before;
};

Install *install() {
	static std::unique_ptr<Install> s_install;
	static bool s_tried = false;
	if (s_tried) {
		if (s_install == nullptr) {
			bf3::test::skip_install_case("no BF3 install (see the first host-scale case)");
		}
		return s_install.get();
	}
	s_tried = true;
	auto m = std::make_unique<Install>();
	m->reader = std::make_unique<core::InstallReader>();
	if (!m->reader->locate({})) {
		bf3::test::skip_install_case(m->reader->describe().c_str());
		return nullptr;
	}
	m->before = m->reader->snapshot();
	core::Result<std::shared_ptr<core::Install>> opened = core::Install::open(*m->reader);
	REQUIRE_MESSAGE(opened.ok(), (opened.ok() ? std::string() : opened.error().message));
	m->install = *opened;
	s_install = std::move(m);
	return s_install.get();
}

ScaleReport bench(Install &r_install, const char *p_level, uint32_t p_observers, double p_seconds, double p_paced) {
	ScaleBenchOptions o;
	o.level = p_level;
	o.mode = "ConquestLarge0";
	o.walkers = 64;
	o.observers = p_observers;
	o.seconds = p_seconds;
	o.paced_seconds = p_paced;
	o.tuning_text = read_text(OPENBF3_MOTOR_TUNING_CFG);
	core::Result<ScaleReport> r = run_scale_bench(r_install.install, o);
	REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
	std::printf("---- host.scale %s ConquestLarge0, %u observer(s) ----\n%s", p_level, p_observers, format_scale_report(*r).c_str());
	std::fflush(stdout);
	return *r;
}

void gate(const ScaleReport &p_r) {
	CHECK(p_r.ticks == 7200);
	CHECK_MESSAGE(p_r.valid, "the bots did not really walk (G1 validity, docs §1)");
#if !defined(OPENBF3_TIMING_REPORT_ONLY)
	CHECK_MESSAGE(p_r.tick_ms.p99 <= kScaleGateP99Ms, "G1: p99 tick ", p_r.tick_ms.p99, " ms > ", kScaleGateP99Ms, " ms");
#endif
	// G3: the six numbers are measured.
	CHECK(p_r.motor_steps > 0);
	CHECK(p_r.queries_per_step > 0.0);
	CHECK(p_r.queries_per_second > 0.0);
	CHECK(p_r.us_per_step > 0.0);
	CHECK(p_r.us_sweep.mean > 0.0);
	CHECK(p_r.us_overlap.mean > 0.0);
	CHECK(p_r.respawns_in_window == 0);
}

} // namespace

TEST_SUITE("host-scale") {
	TEST_CASE("host.scale: G1 / G3 on MP_Subway ConquestLarge0 (64 walkers, 120 s of sim time)") {
		Install *m = install();
		if (m == nullptr) {
			return;
		}
		gate(bench(*m, "MP_Subway", 1, 120.0, 30.0));
	}

	TEST_CASE("host.scale: G1 / G3 on MP_007 ConquestLarge0 (64 walkers, 120 s of sim time)") {
		Install *m = install();
		if (m == nullptr) {
			return;
		}
		gate(bench(*m, "MP_007", 1, 120.0, 30.0));
	}

	TEST_CASE("host.scale: the observers = 16 encode row on MP_Subway (reported, not gated)") {
		Install *m = install();
		if (m == nullptr) {
			return;
		}
		const ScaleReport r = bench(*m, "MP_Subway", 16, 30.0, 0.0);
		CHECK(r.ticks == 1800);
	}

	TEST_CASE("host.scale: install unchanged (layout.toc, cas.cat, the .sb / .toc files)") {
		Install *m = install();
		if (m == nullptr) {
			return;
		}
		const core::SnapshotDiff diff = core::diff_snapshots(m->before, m->reader->snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
		std::printf("install.unchanged: %s (%zu files)\n", diff.clean() ? "clean" : diff.summary().c_str(), m->before.files.size());
	}
}
