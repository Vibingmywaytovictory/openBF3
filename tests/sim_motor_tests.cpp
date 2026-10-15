// Soldier motor, spawning and the prediction catch-up (bf3_sim soldier_motor.h, spawn.h, prediction.h;
// docs/design/m1.md §6, §9.2, §10, §14).
//
// Suite sim-motor-unit (no install): synthetic static worlds (flat floors, ramps, steps, walls, ceilings, banks)
// built with build_static_world(span). The E1 numbers (speeds, the jump apex, the six transition tick counts, the
// fall-damage thresholds), the solver behaviour of §6.1/§6.7 (slopes, steps, walls, ceilings, drops, slides),
// determinism (two worlds, interleaved soldiers, save/restore, a fresh motor), sim::catch_up against the server's
// own stepping, settle and the course searches, the tuning file and the params.
// Suite sim-motor (install, exits 77 without one): the same on Metro's real static world (the soup through T1's
// build_static_world): make_motor_params from Metro's soldier with the shipped tuning, E1a-E1d along courses
// find_clear_heading / find_drop_spot pick, settle_spawn on all 102 TDM points, install unchanged.
#include <doctest/doctest.h>

#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/collision_soup.h"
#include "bf3/core/level/spawn_points.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/command.h"
#include "bf3/sim/prediction.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/spawn.h"
#include "bf3/sim/static_world.h"
#include "test_support.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace bf3;
using namespace bf3::sim;
namespace gameplay = bf3::core::gameplay;

namespace {

// ---- synthetic geometry -------------------------------------------------------------------------------------

void add_tri(std::vector<StaticTriangle> &r_out, const Vec3 &p_a, const Vec3 &p_b, const Vec3 &p_c, StaticLayer p_layer = StaticLayer::Collision) {
	StaticTriangle t;
	t.v = { p_a, p_b, p_c };
	t.layer = p_layer;
	r_out.push_back(t);
}

// A quad a-b-c-d (in order around it); the world collides with both faces.
void add_quad(std::vector<StaticTriangle> &r_out, const Vec3 &p_a, const Vec3 &p_b, const Vec3 &p_c, const Vec3 &p_d,
		StaticLayer p_layer = StaticLayer::Collision) {
	add_tri(r_out, p_a, p_b, p_c, p_layer);
	add_tri(r_out, p_a, p_c, p_d, p_layer);
}

// Flat floor over [x0, x1] x [z0, z1] at height y, in 8 m squares (no huge triangles).
void add_floor(std::vector<StaticTriangle> &r_out, float p_x0, float p_z0, float p_x1, float p_z1, float p_y) {
	for (float x = p_x0; x < p_x1; x += 8.0f) {
		for (float z = p_z0; z < p_z1; z += 8.0f) {
			const float xe = std::min(x + 8.0f, p_x1);
			const float ze = std::min(z + 8.0f, p_z1);
			add_quad(r_out, { x, p_y, z }, { x, p_y, ze }, { xe, p_y, ze }, { xe, p_y, z });
		}
	}
}

// An axis-aligned box [lo, hi] (all six faces).
void add_box(std::vector<StaticTriangle> &r_out, const Vec3 &p_lo, const Vec3 &p_hi, StaticLayer p_layer = StaticLayer::Collision) {
	const float x0 = p_lo[0], y0 = p_lo[1], z0 = p_lo[2], x1 = p_hi[0], y1 = p_hi[1], z1 = p_hi[2];
	add_quad(r_out, { x0, y1, z0 }, { x0, y1, z1 }, { x1, y1, z1 }, { x1, y1, z0 }, p_layer); // top
	add_quad(r_out, { x0, y0, z0 }, { x1, y0, z0 }, { x1, y0, z1 }, { x0, y0, z1 }, p_layer); // bottom
	add_quad(r_out, { x0, y0, z0 }, { x0, y0, z1 }, { x0, y1, z1 }, { x0, y1, z0 }, p_layer); // -x
	add_quad(r_out, { x1, y0, z0 }, { x1, y1, z0 }, { x1, y1, z1 }, { x1, y0, z1 }, p_layer); // +x
	add_quad(r_out, { x0, y0, z0 }, { x0, y1, z0 }, { x1, y1, z0 }, { x1, y0, z0 }, p_layer); // -z
	add_quad(r_out, { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 }, p_layer); // +z
}

// A ramp rising along +Z from (x0..x1, y0 at z0) at p_degrees, p_length metres long (horizontal), then a flat top.
void add_ramp(std::vector<StaticTriangle> &r_out, float p_x0, float p_x1, float p_z0, float p_y0, float p_degrees, float p_length, float p_top_length) {
	const float rise = p_length * std::tan(p_degrees * kPi / 180.0f);
	const float z1 = p_z0 + p_length;
	add_quad(r_out, { p_x0, p_y0, p_z0 }, { p_x0, p_y0 + rise, z1 }, { p_x1, p_y0 + rise, z1 }, { p_x1, p_y0, p_z0 });
	add_quad(r_out, { p_x0, p_y0 + rise, z1 }, { p_x0, p_y0 + rise, z1 + p_top_length }, { p_x1, p_y0 + rise, z1 + p_top_length },
			{ p_x1, p_y0 + rise, z1 });
}

std::shared_ptr<const StaticWorldShapes> build(const std::vector<StaticTriangle> &p_triangles) {
	core::Result<std::shared_ptr<const StaticWorldShapes>> r = build_static_world(std::span<const StaticTriangle>(p_triangles));
	REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
	return *r;
}

std::unique_ptr<CollisionWorld> world_of(std::shared_ptr<const StaticWorldShapes> p_shapes) {
	core::Result<std::unique_ptr<CollisionWorld>> r = CollisionWorld::create(std::move(p_shapes));
	REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
	return std::move(*r);
}

// ---- the soldier: Metro's numbers (bf3dump soldier, 2026-10-15) as a synthetic SoldierDef --------------------

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
	stand.box_min_expand = { 0.2f, 0.0f, 0.1f }; // read, unused by the M1 motor (only prone's footprint is)
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

// The shipped tuning file (docs §6.11: the tests check the game's numbers).
MotorTuning shipped_tuning() {
	const std::string text = read_text(OPENBF3_MOTOR_TUNING_CFG);
	REQUIRE_MESSAGE(!text.empty(), OPENBF3_MOTOR_TUNING_CFG);
	core::Result<MotorTuning> t = parse_motor_tuning(text);
	REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
	return *t;
}

MotorParams params_with(const MotorTuning &p_tuning) {
	core::Result<MotorParams> p = make_motor_params(synthetic_soldier(), p_tuning);
	REQUIRE_MESSAGE(p.ok(), (p.ok() ? std::string() : p.error().message));
	return *p;
}

MotorParams instant_params() {
	MotorTuning t = shipped_tuning();
	t.acceleration_model = GainModel::Instant;
	t.deceleration_model = GainModel::Instant;
	t.sprint_model = GainModel::Instant;
	return params_with(t);
}

struct Rig {
	std::unique_ptr<CollisionWorld> world;
	std::unique_ptr<SoldierMotor> motor;
};

Rig rig_of(const std::vector<StaticTriangle> &p_triangles, const MotorParams &p_params) {
	Rig r;
	r.world = world_of(build(p_triangles));
	r.motor = std::make_unique<SoldierMotor>();
	const core::Status s = r.motor->init(*r.world, p_params);
	REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
	return r;
}

std::vector<StaticTriangle> flat_world(float p_half = 200.0f) {
	std::vector<StaticTriangle> t;
	add_floor(t, -p_half, -p_half, p_half, p_half, 0.0f);
	return t;
}

Command move_command(Tick p_tick, int8_t p_forward, int8_t p_right, uint16_t p_yaw, Pose p_pose = Pose::Stand, uint32_t p_buttons = 0) {
	Command c;
	c.tick = p_tick;
	c.sequence = p_tick;
	c.move_forward = p_forward;
	c.move_right = p_right;
	c.yaw = p_yaw;
	c.pose = p_pose;
	c.buttons = p_buttons;
	return c;
}

// Steps p_ticks commands made by p_make(tick) and records every state.
std::vector<SoldierState> run(const SoldierMotor &p_motor, SoldierState &r_state, uint32_t p_ticks, const std::function<Command(Tick)> &p_make) {
	std::vector<SoldierState> out;
	out.reserve(p_ticks);
	for (uint32_t i = 0; i < p_ticks; ++i) {
		const Command c = p_make(r_state.tick + 1);
		p_motor.step(r_state, c);
		out.push_back(r_state);
	}
	return out;
}

// A soldier settled at rest at p_feet (dropped from the padding, then 30 idle ticks).
SoldierState at_rest(const SoldierMotor &p_motor, const Vec3 &p_feet, uint16_t p_yaw = 0, Tick p_tick = 1000) {
	SoldierState s = p_motor.spawn_state({ p_feet[0], p_feet[1] + p_motor.params().tuning.character_padding, p_feet[2] }, p_yaw, 1, p_tick, false);
	run(p_motor, s, 30, [&](Tick t) { return idle_command(t, p_yaw, 0, Pose::Stand); });
	return s;
}

float horizontal(const Vec3 &p_v) {
	return std::sqrt(p_v[0] * p_v[0] + p_v[2] * p_v[2]);
}

bool grounded(const SoldierState &p_s) {
	return (p_s.flags & soldier_flags::kGrounded) != 0;
}

// A walled 60 m arena inside one 64 m tile (no tile border under it) with a ramp, steps, a ceiling, boxes and a
// 55 degree bank: the busy course of the determinism, interleaving and catch-up tests.
std::vector<StaticTriangle> course_world() {
	std::vector<StaticTriangle> t;
	add_floor(t, 1.0f, 1.0f, 61.0f, 61.0f, 0.0f);
	add_box(t, { 0.5f, 0.0f, 0.5f }, { 1.0f, 4.0f, 61.5f });
	add_box(t, { 61.0f, 0.0f, 0.5f }, { 61.5f, 4.0f, 61.5f });
	add_box(t, { 0.5f, 0.0f, 0.5f }, { 61.5f, 4.0f, 1.0f });
	add_box(t, { 0.5f, 0.0f, 61.0f }, { 61.5f, 4.0f, 61.5f });
	add_ramp(t, 40.0f, 50.0f, 10.0f, 0.0f, 20.0f, 8.0f, 4.0f);
	for (int i = 0; i < 4; ++i) {
		add_box(t, { 10.0f, 0.0f, 40.0f + 0.3f * float(i) }, { 20.0f, 0.2f * float(i + 1), 50.0f });
	}
	add_box(t, { 22.0f, 1.5f, 10.0f }, { 32.0f, 1.7f, 18.0f }); // a low ceiling
	add_box(t, { 45.0f, 0.0f, 45.0f }, { 47.0f, 1.0f, 47.0f });
	add_box(t, { 30.0f, 0.0f, 50.0f }, { 31.0f, 3.0f, 58.0f });
	const float tan55 = std::tan(55.0f * kPi / 180.0f);
	add_quad(t, { 52.0f, 4.0f, 30.0f }, { 60.0f, 4.0f, 30.0f }, { 60.0f, 0.0f, 30.0f + 4.0f / tan55 }, { 52.0f, 0.0f, 30.0f + 4.0f / tan55 });
	return t;
}

// Random but reproducible input: runs, strafes, turns, sprints, jumps and pose requests.
Command busy_command(std::mt19937 &r_rng, Tick p_tick) {
	Command c;
	c.tick = p_tick;
	c.sequence = p_tick;
	const uint32_t r = r_rng();
	// Mostly forward along a heading that changes every 2 s (so the soldier crosses the arena and meets its walls,
	// ramps, steps and ceiling), with strafes and some jitter on top.
	const int8_t forward[] = { 127, 127, 127, 127, 64, 0, -127, 127 };
	const int8_t side[] = { 0, 0, 0, 0, 127, -127, 64, -40 };
	c.move_forward = forward[r % 8];
	c.move_right = side[(r >> 3) % 8];
	uint32_t segment = (p_tick / 120u) * 0x9E3779B1u;
	segment ^= segment >> 15;
	c.yaw = uint16_t(segment * 0x2C1B3C6Du + ((r >> 8) & 0x1FFu));
	c.pitch = int16_t(int32_t((r >> 4) & 0xFFFu) - 2048);
	c.buttons = ((r >> 20) % 3 == 0 ? buttons::kSprint : 0u) | ((r >> 22) % 45 == 0 ? buttons::kJump : 0u);
	const uint32_t phase = (p_tick / 150u) % 5u;
	c.pose = phase == 1 ? Pose::Crouch : (phase == 3 ? Pose::Prone : Pose::Stand);
	return c;
}

// The hash chain of 10,000 busy steps on a fresh world and motor (the same in every process of this binary).
uint64_t busy_course_chain() {
	Rig r = rig_of(course_world(), params_with(shipped_tuning()));
	SoldierState s = r.motor->spawn_state({ 30.0f, 0.02f, 30.0f }, 0, 1, 10, true);
	std::mt19937 rng(20261015u);
	uint64_t chain = 0;
	for (int i = 0; i < 10000; ++i) {
		r.motor->step(s, busy_command(rng, s.tick + 1));
		chain = chain * 0x100000001B3ull ^ soldier_state_hash(s);
	}
	return chain;
}

} // namespace

TEST_SUITE("sim-motor-unit") {
	TEST_CASE("sim.motor: a soldier comes to rest on a flat floor and stays there bit for bit") {
		Rig r = rig_of(flat_world(), instant_params());
		SoldierState s = r.motor->spawn_state({ 1.0f, 0.5f, 2.0f }, 0, 1, 0, false);
		const std::vector<SoldierState> drop = run(*r.motor, s, 120, [](Tick t) { return idle_command(t, 0, 0, Pose::Stand); });
		REQUIRE(grounded(s));
		std::printf("rest: feet y %.7f (floor 0), mode %d, flags 0x%x\n", double(s.position[1]), int(s.mode), unsigned(s.flags));
		CHECK(std::fabs(s.position[1]) <= 0.021f);
		const SoldierState rest = s;
		run(*r.motor, s, 600, [](Tick t) { return idle_command(t, 0, 0, Pose::Stand); });
		CHECK(s.position == rest.position);
		CHECK(s.velocity == Vec3{ 0.0f, 0.0f, 0.0f });
		CHECK(s.mode == MoveMode::OnGround);
	}

	TEST_CASE("sim.motor: E1a steady speeds on a flat floor (walk 4.0, sprint 6.5, crouch 2.0, prone 0.9; +-1 %)") {
		struct Course {
			const char *name;
			Pose pose;
			uint32_t buttons;
			float expected;
		};
		const Course courses[] = { { "walk", Pose::Stand, 0, 4.0f }, { "sprint", Pose::Stand, buttons::kSprint, 6.5f }, { "crouch", Pose::Crouch, 0, 2.0f },
			{ "prone", Pose::Prone, 0, 0.9f } };
		for (const bool instant : { true, false }) {
			const MotorParams params = instant ? instant_params() : params_with(shipped_tuning());
			Rig r = rig_of(flat_world(400.0f), params);
			for (const Course &c : courses) {
				SoldierState s = at_rest(*r.motor, { 0.0f, 0.0f, -300.0f });
				// Take the pose first (the longest transition is 63 ticks), then walk forward along +Z.
				run(*r.motor, s, 80, [&](Tick t) { return idle_command(t, 0, 0, c.pose); });
				REQUIRE(s.pose == c.pose);
				const Vec3 start = s.position;
				std::vector<SoldierState> trace = run(*r.motor, s, 240, [&](Tick t) { return move_command(t, kAxisMax, 0, 0, c.pose, c.buttons); });
				float lo = 1e9f, hi = 0.0f;
				for (size_t i = 120; i < 240; ++i) {
					const float v = horizontal(trace[i].velocity);
					lo = std::min(lo, v);
					hi = std::max(hi, v);
				}
				const float mean = (trace[239].position[2] - trace[119].position[2]) / (120.0f * kTickSeconds);
				// The tick at which the speed first reaches 99 % of the target.
				size_t reach = 0;
				while (reach < trace.size() && horizontal(trace[reach].velocity) < 0.99f * c.expected) {
					++reach;
				}
				std::printf("E1a %s %s: speed %.5f..%.5f m/s, mean %.5f (expected %.2f), 99 %% at tick %zu, drift x %.2e\n", instant ? "instant" : "default",
						c.name, double(lo), double(hi), double(mean), double(c.expected), reach + 1, double(s.position[0] - start[0]));
				CHECK(lo >= 0.99f * c.expected);
				CHECK(hi <= 1.01f * c.expected);
				CHECK(std::fabs(mean - c.expected) <= 0.01f * c.expected);
				CHECK(std::fabs(s.position[0] - start[0]) < 1e-3f);
				CHECK(grounded(s));
				CHECK(((s.flags & soldier_flags::kSprinting) != 0) == (c.buttons != 0));
				if (!instant) {
					// The default gains reach 99 % by tick 22 (AccelerationGain 0.35) and 33 (SprintGain 0.25), well before
					// the Metro window (ticks 60-120) and the synthetic one (120-240).
					CHECK(reach + 1 <= (c.buttons != 0 ? 40u : 30u));
				}
			}
		}
	}

	TEST_CASE("sim.motor: E1a direction factors and diagonals (back 0.75, strafe 0.75, unit-length input)") {
		Rig r = rig_of(flat_world(400.0f), instant_params());
		struct Dir {
			int8_t f, rgt;
			float expected;
			float dx, dz; // expected direction at yaw 0 (right = -X)
		};
		const float d = 0.70710678f;
		const Dir dirs[] = { { -127, 0, 3.0f, 0, -1 }, { 0, 127, 3.0f, -1, 0 }, { 0, -127, 3.0f, 1, 0 }, { 127, 127, 4.0f * 0.5f + 3.0f * 0.5f, -d, d } };
		for (const Dir &dir : dirs) {
			SoldierState s = at_rest(*r.motor, { 0.0f, 0.0f, 0.0f });
			run(*r.motor, s, 60, [&](Tick t) { return move_command(t, dir.f, dir.rgt, 0); });
			const float v = horizontal(s.velocity);
			CHECK(v == doctest::Approx(dir.expected).epsilon(1e-4));
			CHECK(s.velocity[0] / v == doctest::Approx(dir.dx).epsilon(1e-4));
			CHECK(s.velocity[2] / v == doctest::Approx(dir.dz).epsilon(1e-4));
		}
		// Yaw a quarter turn: forward is +X.
		SoldierState s = at_rest(*r.motor, { 0.0f, 0.0f, 0.0f }, 16384);
		run(*r.motor, s, 60, [&](Tick t) { return move_command(t, kAxisMax, 0, 16384); });
		CHECK(s.velocity[0] == doctest::Approx(4.0f).epsilon(1e-4));
		CHECK(std::fabs(s.velocity[2]) < 1e-4f);
	}

	TEST_CASE("sim.motor: deceleration stops a walker exactly (linear 15 m/s^2) and the rest is bit-stable") {
		Rig r = rig_of(flat_world(), params_with(shipped_tuning()));
		SoldierState s = at_rest(*r.motor, { 0.0f, 0.0f, 0.0f });
		run(*r.motor, s, 120, [&](Tick t) { return move_command(t, kAxisMax, 0, 0); });
		const std::vector<SoldierState> stop = run(*r.motor, s, 60, [&](Tick t) { return idle_command(t, 0, 0, Pose::Stand); });
		size_t still = 0;
		while (still < stop.size() && horizontal(stop[still].velocity) > 0.0f) {
			++still;
		}
		std::printf("stop: 4 m/s -> 0 in %zu ticks\n", still + 1);
		CHECK(still + 1 <= 18); // 4 / 15 s = 16 ticks
		CHECK(stop.back().velocity == Vec3{ 0.0f, 0.0f, 0.0f });
		CHECK(stop.back().position == stop[still].position);
	}

	TEST_CASE("sim.motor: E1b jump apex 0.6 m (+-1 mm; the take-off tick is airborne, not 0.657 m), landing, the jump penalty") {
		Rig r = rig_of(flat_world(), params_with(shipped_tuning()));
		SoldierState s = at_rest(*r.motor, { 0.0f, 0.0f, 0.0f });
		const float take_off = s.position[1];
		const Tick jump_tick = s.tick + 1;
		const std::vector<SoldierState> trace = run(*r.motor, s, 90, [&](Tick t) {
			Command c = move_command(t, 0, 0, 0);
			c.buttons = t == jump_tick ? buttons::kJump : 0;
			return c;
		});
		REQUIRE((trace[0].events & soldier_events::kJumped) != 0);
		CHECK(trace[0].mode == MoveMode::Jumping);
		CHECK_FALSE(grounded(trace[0]));
		float apex = -1.0f;
		size_t apex_at = 0;
		size_t landed_at = 0;
		for (size_t i = 0; i < trace.size(); ++i) {
			if (trace[i].position[1] - take_off > apex) {
				apex = trace[i].position[1] - take_off;
				apex_at = i;
			}
			if (landed_at == 0 && (trace[i].events & soldier_events::kLanded) != 0) {
				landed_at = i;
			}
		}
		std::printf("E1b apex %.6f m at tick %zu, landed at tick %zu, impact %.4f m/s\n", double(apex), apex_at + 1, landed_at + 1,
				double(trace[landed_at].impact_speed));
		CHECK(std::fabs(apex - 0.6f) <= 0.001f);
		CHECK(trace[apex_at + 1].mode == MoveMode::InAir);
		REQUIRE(landed_at > 0);
		CHECK(trace[landed_at].impact_speed == doctest::Approx(std::sqrt(2.0f * 9.81f * 0.6f)).epsilon(1e-3));
		CHECK(trace[landed_at].health == 100.0f);
		CHECK(trace[landed_at].mode == MoveMode::OnGround);
		CHECK(std::fabs(trace.back().position[1] - take_off) < 1e-6f);

		// A second jump inside JumpPenaltyTime (1 s) reaches 0.3 x 0.6 m; one after it the full height again.
		const auto apex_of = [&](uint32_t p_wait) {
			SoldierState q = at_rest(*r.motor, { 0.0f, 0.0f, 0.0f });
			run(*r.motor, q, 1, [&](Tick t) { return move_command(t, 0, 0, 0, Pose::Stand, buttons::kJump); });
			run(*r.motor, q, p_wait, [&](Tick t) { return move_command(t, 0, 0, 0); });
			REQUIRE(grounded(q));
			const float base = q.position[1];
			const Tick second_tick = q.tick + 1;
			float top = 0.0f;
			const std::vector<SoldierState> second =
					run(*r.motor, q, 80, [&](Tick t) { return move_command(t, 0, 0, 0, Pose::Stand, t == second_tick ? buttons::kJump : 0); });
			for (const SoldierState &e : second) {
				top = std::max(top, e.position[1] - base);
			}
			return top;
		};
		const float penalised = apex_of(50);
		const float free = apex_of(70);
		std::printf("E1b penalised apex %.5f m, after the penalty %.5f m\n", double(penalised), double(free));
		CHECK(penalised == doctest::Approx(0.18f).epsilon(0.01));
		CHECK(std::fabs(free - 0.6f) <= 0.001f);
	}

	TEST_CASE("sim.motor: E1c the six pose transitions take 18 / 42 / 18 / 30 / 42 / 63 ticks (finished - started + 1)") {
		Rig r = rig_of(flat_world(), params_with(shipped_tuning()));
		struct Change {
			Pose from, to;
			uint32_t ticks;
		};
		const Change changes[] = { { Pose::Stand, Pose::Crouch, 18 }, { Pose::Stand, Pose::Prone, 42 }, { Pose::Crouch, Pose::Stand, 18 },
			{ Pose::Crouch, Pose::Prone, 30 }, { Pose::Prone, Pose::Stand, 42 }, { Pose::Prone, Pose::Crouch, 63 } };
		for (const Change &c : changes) {
			SoldierState s = at_rest(*r.motor, { 0.0f, 0.0f, 0.0f });
			run(*r.motor, s, 90, [&](Tick t) { return idle_command(t, 0, 0, c.from); });
			REQUIRE(s.pose == c.from);
			REQUIRE(s.transition_ticks == 0);
			const std::vector<SoldierState> trace = run(*r.motor, s, 120, [&](Tick t) { return idle_command(t, 0, 0, c.to); });
			int64_t started = -1, finished = -1;
			for (size_t i = 0; i < trace.size(); ++i) {
				if (started < 0 && (trace[i].events & soldier_events::kPoseStarted) != 0) {
					started = int64_t(trace[i].tick);
					CHECK(trace[i].transition_ticks == c.ticks - 1);
					CHECK(trace[i].transition_total == c.ticks);
					CHECK(trace[i].target_pose == c.to);
				} else if (started >= 0 && finished < 0 && trace[i].transition_ticks == 0 && (trace[i].events & soldier_events::kPoseFinished) != 0) {
					finished = int64_t(trace[i].tick);
					CHECK(trace[i].pose == c.to);
				}
			}
			REQUIRE(started >= 0);
			REQUIRE(finished >= 0);
			std::printf("E1c %d -> %d: %lld ticks\n", int(c.from), int(c.to), static_cast<long long>(finished - started + 1));
			CHECK(finished - started + 1 == int64_t(c.ticks));
			CHECK(started == int64_t(trace.front().tick)); // starts on the tick of the pose command
		}
	}

	TEST_CASE("sim.motor: the eye height blends linearly over a transition") {
		Rig r = rig_of(flat_world(), params_with(shipped_tuning()));
		SoldierState s = at_rest(*r.motor, { 0.0f, 0.0f, 0.0f });
		CHECK(r.motor->eye_height(s) == doctest::Approx(1.6f));
		const std::vector<SoldierState> trace = run(*r.motor, s, 20, [&](Tick t) { return idle_command(t, 0, 0, Pose::Crouch); });
		CHECK(r.motor->eye_height(trace[0]) == doctest::Approx(1.6f + (1.0f - 1.6f) * (1.0f / 18.0f)));
		CHECK(r.motor->eye_height(trace[8]) == doctest::Approx(1.6f + (1.0f - 1.6f) * (9.0f / 18.0f)));
		CHECK(r.motor->eye_height(trace[17]) == doctest::Approx(1.0f));
	}

	TEST_CASE("sim.motor: E1d fall damage: 9.8 m -> 0 hp, 10.1 m -> 20-21 hp, 46 m -> dead; impact = sqrt(2 g h), fall = h") {
		Rig r = rig_of(flat_world(), params_with(shipped_tuning()));
		struct Drop {
			float h;
			float min_damage, max_damage;
			bool dies;
		};
		const Drop drops[] = { { 9.8f, 0, 0, false }, { 10.1f, 20, 21, false }, { 46.0f, 100, 100, true } };
		for (const Drop &d : drops) {
			// A Teleport: at rest, feet h above the floor (the feet rest on the floor: Jolt keeps the padding between the
			// shape and the floor, not under the feet), no spawn protection.
			SoldierState s = r.motor->spawn_state({ 3.0f, d.h, 4.0f }, 0, 2, 5000, false);
			const float start = s.position[1];
			SoldierState landed;
			bool seen = false;
			bool falling = false;
			for (int i = 0; i < 600 && !seen; ++i) {
				r.motor->step(s, idle_command(s.tick + 1, 0, 0, Pose::Stand));
				falling = falling || s.mode == MoveMode::Falling;
				if ((s.events & soldier_events::kLanded) != 0) {
					landed = s;
					seen = true;
				}
			}
			REQUIRE(seen);
			const float fall = start - landed.position[1];
			const float expected = std::sqrt(2.0f * 9.81f * d.h);
			std::printf("E1d %.1f m: fell %.5f m, impact %.5f m/s (sqrt(2gh) %.5f), damage %.3f, health %.3f, mode %d, Falling seen %d\n", double(d.h),
					double(fall), double(landed.impact_speed), double(expected), double(landed.damage), double(landed.health), int(landed.mode), falling ? 1 : 0);
			CHECK(std::fabs(fall - d.h) <= 0.001f);
			CHECK(std::fabs(landed.impact_speed - expected) <= 1e-3f);
			CHECK(landed.damage >= d.min_damage);
			CHECK(landed.damage <= d.max_damage);
			CHECK(((landed.events & soldier_events::kDied) != 0) == d.dies);
			CHECK((landed.mode == MoveMode::Dead) == d.dies);
			CHECK(falling == (d.h > 13.0f)); // InAir -> Falling below -16 m/s (a 13 m fall)
			if (d.dies) {
				// A dead soldier only advances its tick.
				SoldierState next = landed;
				r.motor->step(next, move_command(landed.tick + 1, kAxisMax, 0, 0, Pose::Stand, buttons::kJump));
				CHECK(next.tick == landed.tick + 1);
				CHECK(next.position == landed.position);
				CHECK(next.events == 0);
			}
		}
		// Spawn protection blocks the damage (the 10.1 m drop with protection running).
		SoldierState s = r.motor->spawn_state({ 3.0f, 10.1f, 4.0f }, 0, 2, 5000, true);
		run(*r.motor, s, 100, [&](Tick t) { return idle_command(t, 0, 0, Pose::Stand); });
		CHECK(s.health == 100.0f);
		CHECK(s.impact_speed > 13.9f);
	}

	TEST_CASE("sim.motor: a 49 degree ramp is walkable, a 51 degree ramp is not") {
		for (const float degrees : { 49.0f, 51.0f }) {
			std::vector<StaticTriangle> t;
			add_floor(t, -20.0f, -20.0f, 20.0f, 2.0f, 0.0f);
			add_ramp(t, -20.0f, 20.0f, 2.0f, 0.0f, degrees, 6.0f, 10.0f);
			Rig r = rig_of(t, instant_params());
			SoldierState s = at_rest(*r.motor, { 0.0f, 0.0f, 0.0f });
			float top = 0.0f;
			bool steep_seen = false;
			const std::vector<SoldierState> trace = run(*r.motor, s, 360, [&](Tick tick) { return move_command(tick, kAxisMax, 0, 0); });
			for (const SoldierState &e : trace) {
				top = std::max(top, e.position[1]);
				steep_seen = steep_seen || (e.flags & soldier_flags::kSteepGround) != 0;
			}
			std::printf("ramp %.0f deg: highest feet %.3f m, end z %.3f, steep seen %d\n", double(degrees), double(top), double(s.position[2]), steep_seen ? 1 : 0);
			if (degrees < 50.0f) {
				CHECK(top > 6.0f); // reached the top (rise 6.9 m)
				CHECK(grounded(s));
			} else {
				CHECK(top < 0.7f); // walk-stairs may lift it onto the foot of the ramp, never up it
				CHECK(s.position[2] < 3.0f);
			}
		}
	}

	TEST_CASE("sim.motor: a steady walk up a 10 degree ramp keeps the target speed (the grounded clip uses the ground plane)") {
		std::vector<StaticTriangle> t;
		add_floor(t, -20.0f, -20.0f, 20.0f, 2.0f, 0.0f);
		add_ramp(t, -20.0f, 20.0f, 2.0f, 0.0f, 10.0f, 60.0f, 10.0f);
		Rig r = rig_of(t, params_with(shipped_tuning()));
		SoldierState s = at_rest(*r.motor, { 0.0f, 0.0f, -10.0f });
		const std::vector<SoldierState> trace = run(*r.motor, s, 600, [&](Tick tick) { return move_command(tick, kAxisMax, 0, 0); });
		float lo = 1e9f;
		size_t on_ramp = 0;
		for (size_t i = 200; i < 600; ++i) {
			if (trace[i].position[1] > 0.5f && trace[i].position[2] < 55.0f) {
				lo = std::min(lo, horizontal(trace[i].velocity));
				++on_ramp;
				CHECK(grounded(trace[i]));
			}
		}
		std::printf("10 deg ramp: lowest state speed %.5f m/s over %zu ramp ticks\n", double(lo), on_ramp);
		CHECK(on_ramp > 200);
		CHECK(lo >= 0.99f * 4.0f);
	}

	TEST_CASE("sim.motor: steps of 0.29 m are climbed, 0.45 m block; the measured limit is printed") {
		const auto climbs = [](float p_height) {
			std::vector<StaticTriangle> t;
			add_floor(t, -20.0f, -20.0f, 20.0f, 20.0f, 0.0f);
			add_box(t, { -20.0f, 0.0f, 2.0f }, { 20.0f, p_height, 12.0f });
			Rig r = rig_of(t, instant_params());
			SoldierState s = at_rest(*r.motor, { 0.0f, 0.0f, 0.0f });
			run(*r.motor, s, 180, [&](Tick tick) { return move_command(tick, kAxisMax, 0, 0); });
			return s.position[1] > p_height - 0.01f && s.position[2] > 4.0f;
		};
		CHECK(climbs(0.29f));
		CHECK(climbs(0.30f));
		CHECK_FALSE(climbs(0.45f));
		float limit = 0.29f;
		for (float h = 0.30f; h <= 0.45f; h += 0.01f) {
			if (!climbs(h)) {
				break;
			}
			limit = h;
		}
		std::printf("step limit: climbs up to %.2f m (StepHeight 0.3 + the capsule's rounded bottom)\n", double(limit));
		CHECK(limit >= 0.30f);
		CHECK(limit < 0.45f);
	}

	TEST_CASE("sim.motor: pushing into a steep wall for 60 ticks neither climbs nor jitters") {
		std::vector<StaticTriangle> t;
		add_floor(t, -20.0f, -20.0f, 20.0f, 20.0f, 0.0f);
		add_box(t, { -20.0f, 0.0f, 2.0f }, { 20.0f, 3.0f, 4.0f });
		Rig r = rig_of(t, params_with(shipped_tuning()));
		SoldierState s = at_rest(*r.motor, { 0.0f, 0.0f, 0.0f });
		run(*r.motor, s, 60, [&](Tick tick) { return move_command(tick, kAxisMax, 0, 0); });
		const std::vector<SoldierState> push = run(*r.motor, s, 60, [&](Tick tick) { return move_command(tick, kAxisMax, 0, 0); });
		float zlo = 1e9f, zhi = -1e9f, ylo = 1e9f, yhi = -1e9f;
		for (const SoldierState &e : push) {
			zlo = std::min(zlo, e.position[2]);
			zhi = std::max(zhi, e.position[2]);
			ylo = std::min(ylo, e.position[1]);
			yhi = std::max(yhi, e.position[1]);
			CHECK(grounded(e));
		}
		std::printf("wall push: z %.6f..%.6f, y %.6f..%.6f, end speed %.6f\n", double(zlo), double(zhi), double(ylo), double(yhi),
				double(horizontal(s.velocity)));
		CHECK(zhi - zlo < 0.001f);
		CHECK(yhi < 0.001f);
		CHECK(zhi < 2.0f - 0.35f + 0.001f);
		CHECK(horizontal(s.velocity) < 0.01f); // the clip: the realised (zero) displacement, not 4 m/s into the wall
	}

	TEST_CASE("sim.motor: pushing diagonally into a wall keeps a velocity parallel to it") {
		std::vector<StaticTriangle> t;
		add_floor(t, -40.0f, -40.0f, 40.0f, 40.0f, 0.0f);
		add_box(t, { 2.0f, 0.0f, -40.0f }, { 4.0f, 3.0f, 40.0f }); // a wall face at x = 2
		Rig r = rig_of(t, instant_params());
		// Yaw -45 degrees looks between +Z and -X... use yaw +45 degrees: forward (0.707, 0, 0.707), towards the wall.
		SoldierState s = at_rest(*r.motor, { 0.0f, 0.0f, -20.0f }, 8192);
		const std::vector<SoldierState> trace = run(*r.motor, s, 120, [&](Tick tick) { return move_command(tick, kAxisMax, 0, 8192); });
		const SoldierState &e = trace.back();
		std::printf("diagonal wall: x %.5f, v (%.6f, %.6f, %.6f)\n", double(e.position[0]), double(e.velocity[0]), double(e.velocity[1]),
				double(e.velocity[2]));
		CHECK(e.position[0] < 2.0f - 0.35f + 0.001f);
		CHECK(std::fabs(e.velocity[0]) <= 1e-3f);
		CHECK(e.velocity[2] > 2.0f);
	}

	TEST_CASE("sim.motor: walking off 0.4 m stays grounded (ground hugging), off 0.6 m goes InAir") {
		for (const float drop : { 0.4f, 0.6f }) {
			std::vector<StaticTriangle> t;
			add_floor(t, -20.0f, -20.0f, 20.0f, 4.0f, 0.0f);
			add_floor(t, -20.0f, 4.0f, 20.0f, 30.0f, -drop);
			add_quad(t, { -20.0f, 0.0f, 4.0f }, { 20.0f, 0.0f, 4.0f }, { 20.0f, -drop, 4.0f }, { -20.0f, -drop, 4.0f });
			Rig r = rig_of(t, instant_params());
			SoldierState s = at_rest(*r.motor, { 0.0f, 0.0f, 0.0f });
			const std::vector<SoldierState> trace = run(*r.motor, s, 120, [&](Tick tick) { return move_command(tick, kAxisMax, 0, 0); });
			bool air = false;
			for (const SoldierState &e : trace) {
				air = air || !grounded(e);
			}
			std::printf("walk off %.1f m: airborne seen %d, end y %.4f z %.4f\n", double(drop), air ? 1 : 0, double(s.position[1]), double(s.position[2]));
			CHECK(air == (drop > 0.5f));
			// On the lower floor at its height, but for single-tick lifts of 1-3 mm where the path crosses an active
			// edge (here a quad's diagonal on a 64 m tile border, which the soup does not weld): the next tick sticks
			// back down, and the horizontal speed is not affected.
			size_t at_floor = 0;
			for (size_t i = trace.size() - 20; i < trace.size(); ++i) {
				CHECK(std::fabs(trace[i].position[1] + drop) < 0.005f);
				at_floor += std::fabs(trace[i].position[1] + drop) < 1e-4f ? 1 : 0;
				CHECK(grounded(trace[i]));
			}
			CHECK(at_floor >= 18);
		}
	}

	TEST_CASE("sim.motor: a 1.5 s slide down a 55 degree bank lands without damage and never enters Falling") {
		std::vector<StaticTriangle> t;
		// The bank falls along +Z from y 10 at z 0 to y 0 at z 10 / tan 55 = 7.0; the soldier starts 1 m down it, so the
		// frictionless slide (g sin 55 along the bank) takes about 1.6 s.
		const float tan55 = std::tan(55.0f * kPi / 180.0f);
		const float run_z = 10.0f / tan55;
		add_floor(t, -20.0f, run_z, 20.0f, 40.0f, 0.0f);
		add_quad(t, { -20.0f, 10.0f, 0.0f }, { 20.0f, 10.0f, 0.0f }, { 20.0f, 0.0f, run_z }, { -20.0f, 0.0f, run_z });
		Rig r = rig_of(t, params_with(shipped_tuning()));
		SoldierState s = r.motor->spawn_state({ 0.0f, 10.0f - 1.0f * tan55 + 0.3f, 1.0f }, 0, 1, 100, false);
		const std::vector<SoldierState> trace = run(*r.motor, s, 240, [&](Tick tick) { return idle_command(tick, 0, 0, Pose::Stand); });
		float fastest = 0.0f;
		bool falling = false, steep = false, damaged = false;
		size_t landed_at = 0;
		for (size_t i = 0; i < trace.size(); ++i) {
			fastest = std::min(fastest, trace[i].velocity[1]);
			falling = falling || trace[i].mode == MoveMode::Falling;
			steep = steep || (trace[i].flags & soldier_flags::kSteepGround) != 0;
			damaged = damaged || (trace[i].events & soldier_events::kDamaged) != 0;
			if (landed_at == 0 && grounded(trace[i]) && trace[i].position[1] < 0.01f) {
				landed_at = i;
			}
		}
		std::printf("55 deg slide: down in %zu ticks, fastest v_y %.3f m/s, steep seen %d, landing impact %.3f\n", landed_at + 1, double(fastest), steep ? 1 : 0,
				double(trace[landed_at].impact_speed));
		CHECK(steep);
		CHECK_FALSE(falling);
		CHECK_FALSE(damaged);
		CHECK(landed_at > 60);
		CHECK(fastest > -13.9f);
		CHECK(s.health == 100.0f);
		CHECK(grounded(s));
	}

	TEST_CASE("sim.motor: a jump under a 2.0 m ceiling turns down on the contact tick") {
		std::vector<StaticTriangle> t;
		add_floor(t, -20.0f, -20.0f, 20.0f, 20.0f, 0.0f);
		add_box(t, { -20.0f, 2.0f, -20.0f }, { 20.0f, 2.2f, 20.0f });
		Rig r = rig_of(t, params_with(shipped_tuning()));
		SoldierState s = at_rest(*r.motor, { 0.0f, 0.0f, 0.0f });
		const Tick jump = s.tick + 1;
		const std::vector<SoldierState> trace = run(*r.motor, s, 60, [&](Tick tick) { return move_command(tick, 0, 0, 0, Pose::Stand, tick == jump ? buttons::kJump : 0); });
		size_t top = 0;
		for (size_t i = 1; i < trace.size(); ++i) {
			if (trace[i].position[1] > trace[top].position[1]) {
				top = i;
			}
		}
		size_t landed = 0;
		while (landed < trace.size() && !(landed > 0 && grounded(trace[landed]))) {
			++landed;
		}
		std::printf("ceiling jump: top %.4f m at tick %zu, v_y there %.4f, landed at tick %zu\n", double(trace[top].position[1]), top + 1,
				double(trace[top].velocity[1]), landed + 1);
		CHECK(trace[top].position[1] < 0.2f + 0.001f);
		CHECK(trace[top].velocity[1] <= 0.0f);
		CHECK(trace[top].mode == MoveMode::InAir);
		CHECK(landed < 25); // a free 0.6 m jump lands after 42 ticks
	}

	TEST_CASE("sim.motor: standing up under a low ceiling is refused (kStandBlocked) until there is room") {
		std::vector<StaticTriangle> t;
		add_floor(t, -20.0f, -20.0f, 20.0f, 40.0f, 0.0f);
		add_box(t, { -20.0f, 1.5f, 5.0f }, { 20.0f, 1.7f, 15.0f });
		Rig r = rig_of(t, instant_params());
		SoldierState s = at_rest(*r.motor, { 0.0f, 0.0f, 0.0f });
		run(*r.motor, s, 40, [&](Tick tick) { return idle_command(tick, 0, 0, Pose::Crouch); });
		REQUIRE(s.pose == Pose::Crouch);
		// Crouch-walk under the ceiling (z 5..15), stop at z 10.
		for (int i = 0; i < 600 && s.position[2] < 10.0f; ++i) {
			r.motor->step(s, move_command(s.tick + 1, kAxisMax, 0, 0, Pose::Crouch));
		}
		run(*r.motor, s, 20, [&](Tick tick) { return idle_command(tick, 0, 0, Pose::Crouch); });
		const std::vector<SoldierState> refused = run(*r.motor, s, 30, [&](Tick tick) { return idle_command(tick, 0, 0, Pose::Stand); });
		CHECK((refused[0].events & soldier_events::kPoseRefused) != 0);
		CHECK((refused[1].events & soldier_events::kPoseRefused) == 0); // reported once
		for (const SoldierState &e : refused) {
			CHECK((e.flags & soldier_flags::kStandBlocked) != 0);
			CHECK(e.pose == Pose::Crouch);
			CHECK(e.target_pose == Pose::Crouch);
		}
		// Still asking for Stand while walking out: the transition starts once the stand shape fits.
		const std::vector<SoldierState> out = run(*r.motor, s, 240, [&](Tick tick) { return move_command(tick, kAxisMax, 0, 0, Pose::Stand); });
		size_t started = 0;
		while (started < out.size() && (out[started].events & soldier_events::kPoseStarted) == 0) {
			++started;
		}
		REQUIRE(started < out.size());
		std::printf("stand-blocked: stand starts at z %.3f (the ceiling ends at 15 + radius)\n", double(out[started].position[2]));
		CHECK(out[started].position[2] > 15.0f);
		CHECK(s.pose == Pose::Stand);
		CHECK((s.flags & soldier_flags::kStandBlocked) == 0);
	}

	TEST_CASE("sim.motor: prone needs the footprint free; a prone soldier cannot turn its body into a wall") {
		std::vector<StaticTriangle> t;
		add_floor(t, -20.0f, -20.0f, 20.0f, 20.0f, 0.0f);
		add_box(t, { -20.0f, 0.0f, 1.0f }, { 20.0f, 2.0f, 3.0f }); // a wall 1 m ahead (+Z)
		Rig r = rig_of(t, instant_params());
		// Facing the wall: the footprint reaches 1.1 m ahead, into it: refused.
		SoldierState s = at_rest(*r.motor, { 0.0f, 0.0f, 0.0f }, 0);
		std::vector<SoldierState> trace = run(*r.motor, s, 5, [&](Tick tick) { return idle_command(tick, 0, 0, Pose::Prone); });
		CHECK((trace[0].events & soldier_events::kPoseRefused) != 0);
		CHECK(s.pose == Pose::Stand);
		// Facing away (the wall 1 m behind, the footprint 1.3 m): refused too; facing sideways (+X): free.
		s = at_rest(*r.motor, { 0.0f, 0.0f, 0.0f }, 32768);
		trace = run(*r.motor, s, 5, [&](Tick tick) { return idle_command(tick, 32768, 0, Pose::Prone); });
		CHECK((trace[0].events & soldier_events::kPoseRefused) != 0);
		s = at_rest(*r.motor, { 0.0f, 0.0f, 0.0f }, 16384);
		trace = run(*r.motor, s, 60, [&](Tick tick) { return idle_command(tick, 16384, 0, Pose::Prone); });
		CHECK((trace[0].events & soldier_events::kPoseStarted) != 0);
		CHECK(s.pose == Pose::Prone);
		// Turning to face the wall is refused (the yaw stays), turning away along the wall is allowed.
		run(*r.motor, s, 5, [&](Tick tick) { return idle_command(tick, 0, 0, Pose::Prone); });
		CHECK(s.yaw == 16384);
		run(*r.motor, s, 5, [&](Tick tick) { return idle_command(tick, 49152, 0, Pose::Prone); });
		CHECK(s.yaw == 49152);
	}




	TEST_CASE("sim.motor: the shipped motor_tuning.cfg parses and names every key; format / parse round trip; errors") {
		const std::string text = read_text(OPENBF3_MOTOR_TUNING_CFG);
		const MotorTuning shipped = shipped_tuning();
		CHECK(shipped == MotorTuning()); // M1 closes on the research defaults (orchestrator decision on §16 Q3)
		// Every key of format_motor_tuning appears as a key line of the shipped file.
		const std::string all = format_motor_tuning(MotorTuning());
		size_t keys = 0;
		for (size_t at = 0; at < all.size();) {
			const size_t end = all.find('\n', at);
			const std::string line = all.substr(at, end - at);
			const std::string key = line.substr(0, line.find(' '));
			CHECK_MESSAGE(text.find("\n" + key + " = ") != std::string::npos, key);
			++keys;
			at = end + 1;
		}
		CHECK(keys == 19);
		// Round trip with every field changed.
		MotorTuning t;
		t.gravity = 9.80665f;
		t.reference_hz = 33.3f;
		t.acceleration_model = GainModel::Linear;
		t.deceleration_model = GainModel::Instant;
		t.sprint_model = GainModel::Linear;
		t.air_control_scale = 0.25f;
		t.transition_uses_slower_pose = false;
		t.jump_penalty_scales_height = false;
		t.sprint_stands_up = false;
		t.prone_footprint_check = false;
		t.prone_footprint_height = 0.4f;
		t.character_padding = 0.03f;
		t.predictive_contact_distance = 0.15f;
		t.penetration_recovery_speed = 0.5f;
		t.pose_change_max_penetration = 0.02f;
		t.max_collision_iterations = 7;
		t.max_constraint_iterations = 20;
		t.enhanced_internal_edge_removal = false;
		t.min_landing_speed = 0.75f;
		core::Result<MotorTuning> back = parse_motor_tuning(format_motor_tuning(t));
		REQUIRE(back.ok());
		CHECK(*back == t);
		// Comments, blanks, CRLF and spacing.
		core::Result<MotorTuning> spaced = parse_motor_tuning("# c\r\n\r\n  gravity=9.5   # trailing\r\n\tsprint_model =\tinstant\r\n");
		REQUIRE(spaced.ok());
		CHECK(spaced->gravity == 9.5f);
		CHECK(spaced->sprint_model == GainModel::Instant);
		CHECK(parse_motor_tuning("").ok());
		// Errors name the line.
		const char *bad[] = { "gravity = 9.81\nnope = 1\n", "gravity = abc\n", "gravity = 9.81 m\n", "gravity = 1e30\n", "gravity = nan\n", "gravity\n",
			"gravity = 9\ngravity = 9\n", "acceleration_model = quadratic\n", "sprint_stands_up = yes\n", "max_collision_iterations = -1\n",
			"max_collision_iterations = 0\n", "character_padding = 0\n", " = 3\n" };
		for (const char *b : bad) {
			const core::Result<MotorTuning> r = parse_motor_tuning(b);
			CHECK_MESSAGE(!r.ok(), b);
			if (!r.ok()) {
				CHECK(r.error().message.find("line") != std::string::npos);
			}
		}
		// Hostile bytes never crash and never loop: random text and random mutations of the shipped file.
		std::mt19937 rng(77u);
		for (int i = 0; i < 2000; ++i) {
			std::string m = text;
			const int edits = 1 + int(rng() % 8);
			for (int e = 0; e < edits && !m.empty(); ++e) {
				m[rng() % m.size()] = char(rng() & 0xFF);
			}
			(void)parse_motor_tuning(m);
			std::string junk(size_t(rng() % 200), '\0');
			for (char &c : junk) {
				c = char(rng() & 0xFF);
			}
			(void)parse_motor_tuning(junk);
		}
	}

	TEST_CASE("sim.motor: make_motor_params from a synthetic SoldierDef; motor_params_hash covers every tuning key") {
		const MotorParams p = params_with(MotorTuning());
		CHECK(p.radius == 0.35f);
		CHECK(p.jump_height == 0.6f);
		CHECK(p.jump_penalty_ticks == 60);
		CHECK(p.immortal_ticks == 120);
		CHECK(p.free_fall_velocity == 16.0f);
		CHECK(p.allowed_distance_from_ground == 0.5f);
		CHECK(p.ground_hugging);
		CHECK(p.fall_damage_vertical.count == 5);
		CHECK(eval(p.fall_damage_vertical, 13.9f) == 0.0f);
		CHECK(eval(p.fall_damage_vertical, 14.0f) == 20.0f);
		CHECK(eval(p.fall_damage_vertical, 18.0f) == doctest::Approx(30.0f));
		CHECK(eval(p.fall_damage_vertical, 99.0f) == 100.0f);
		const PoseParams &stand = p.poses[size_t(Pose::Stand)];
		const PoseParams &crouch = p.poses[size_t(Pose::Crouch)];
		const PoseParams &prone = p.poses[size_t(Pose::Prone)];
		CHECK(stand.transition_ticks == std::array<uint16_t, 3>{ 0, 18, 42 });
		CHECK(crouch.transition_ticks == std::array<uint16_t, 3>{ 18, 0, 30 });
		CHECK(prone.transition_ticks == std::array<uint16_t, 3>{ 42, 63, 0 });
		CHECK(stand.on_ground.velocity == 4.0f);
		CHECK(stand.on_ground.sprint_multiplier == 1.625f);
		CHECK(crouch.on_ground.velocity == 2.0f);
		CHECK(prone.on_ground.velocity == 0.9f);
		CHECK(stand.on_ground.acceleration_factor == doctest::Approx(1.0 - std::pow(0.65, 0.5)).epsilon(1e-6));
		CHECK(stand.on_ground.deceleration_factor == doctest::Approx(15.0f / 60.0f).epsilon(1e-6));
		CHECK(stand.on_ground.sprint_factor == doctest::Approx(1.0 - std::pow(0.75, 0.5)).epsilon(1e-6));
		CHECK(stand.in_air.velocity == 6.0f);
		CHECK(stand.in_air.deceleration_factor == 0.0f);
		CHECK(prone.box_min_expand == std::array<float, 3>{ 0.3f, 0.0f, 1.3f });
		// Every tuning key changes the hash.
		const uint64_t base = motor_params_hash(p);
		CHECK(base == motor_params_hash(params_with(MotorTuning())));
		const std::string text = format_motor_tuning(MotorTuning());
		size_t changed = 0, keys = 0;
		for (size_t at = 0; at < text.size();) {
			const size_t end = text.find('\n', at);
			const std::string line = text.substr(at, end - at);
			at = end + 1;
			const std::string key = line.substr(0, line.find(' '));
			const std::string value = line.substr(line.find('=') + 2);
			std::string other;
			if (value == "true" || value == "false") {
				other = value == "true" ? "false" : "true";
			} else if (value == "linear" || value == "instant" || value == "fraction_per_reference_tick") {
				other = value == "linear" ? "instant" : "linear";
			} else if (key == "max_collision_iterations" || key == "max_constraint_iterations") {
				other = "9";
			} else {
				other = std::to_string(std::stof(value) * 0.9f);
			}
			core::Result<MotorTuning> t = parse_motor_tuning(key + " = " + other + "\n");
			REQUIRE_MESSAGE(t.ok(), key);
			++keys;
			changed += motor_params_hash(params_with(*t)) != base ? 1 : 0;
		}
		CHECK(changed == keys);
		// Missing data fails.
		gameplay::SoldierDef d = synthetic_soldier();
		d.states.erase(d.states.begin()); // OnGround
		CHECK_FALSE(make_motor_params(d).ok());
		d = synthetic_soldier();
		d.fall_damage_vertical.clear();
		CHECK_FALSE(make_motor_params(d).ok());
		d = synthetic_soldier();
		d.poses.pop_back();
		CHECK_FALSE(make_motor_params(d).ok());
		d = synthetic_soldier();
		d.radius = std::nanf("");
		CHECK_FALSE(make_motor_params(d).ok());
		d = synthetic_soldier();
		d.fall_damage_vertical.resize(9);
		CHECK_FALSE(make_motor_params(d).ok());
	}

	TEST_CASE("sim.motor: a fresh motor's first step equals a used motor's; sequence and flags never change a state") {
		const std::vector<StaticTriangle> t = flat_world();
		Rig used = rig_of(t, params_with(shipped_tuning()));
		// Use the first motor for a while (jumps, walls, poses).
		SoldierState warm = at_rest(*used.motor, { 5.0f, 0.0f, 5.0f });
		run(*used.motor, warm, 200, [&](Tick tick) { return move_command(tick, kAxisMax, 20, uint16_t(tick * 97), Pose::Stand, tick % 50 == 0 ? buttons::kJump : 0); });
		SoldierState a = used.motor->spawn_state({ -3.0f, 0.02f, 1.0f }, 1234, 3, 77, false);
		SoldierState b = a;
		Rig fresh = rig_of(t, params_with(shipped_tuning()));
		for (int i = 0; i < 120; ++i) {
			Command c = move_command(a.tick + 1, int8_t(i % 3 == 0 ? 127 : -50), int8_t(i % 5), uint16_t(1234 + i * 40), i > 60 ? Pose::Crouch : Pose::Stand,
					i == 30 ? buttons::kJump : 0);
			Command c2 = c;
			c2.sequence = 999999u - uint32_t(i);
			c2.flags = command_flags::kRepeated | command_flags::kLocalSource;
			used.motor->step(a, c);
			fresh.motor->step(b, c2);
			REQUIRE(soldier_state_hash(a) == soldier_state_hash(b));
		}
		CHECK(a == b);
	}

	TEST_CASE("sim.motor: a refused pose leaves the next step as a fresh motor's (the fit check is on the CollisionWorld)") {
		std::vector<StaticTriangle> t;
		add_floor(t, 1.0f, 1.0f, 60.0f, 60.0f, 0.0f);
		add_box(t, { 1.0f, 1.5f, 10.0f }, { 60.0f, 1.7f, 30.0f });
		Rig a = rig_of(t, instant_params());
		Rig b = rig_of(t, instant_params());
		SoldierState s = at_rest(*a.motor, { 20.0f, 0.0f, 5.0f });
		run(*a.motor, s, 40, [&](Tick tick) { return idle_command(tick, 0, 0, Pose::Crouch); });
		for (int i = 0; i < 600 && s.position[2] < 20.0f; ++i) {
			a.motor->step(s, move_command(s.tick + 1, kAxisMax, 0, 0, Pose::Crouch));
		}
		// The refused stand on motor A, then the same next step on A and on the unused motor B.
		a.motor->step(s, move_command(s.tick + 1, kAxisMax, 0, 0, Pose::Stand));
		REQUIRE((s.events & soldier_events::kPoseRefused) != 0);
		SoldierState sa = s, sb = s;
		const Command next = move_command(s.tick + 1, kAxisMax, 0, 0, Pose::Stand);
		a.motor->step(sa, next);
		b.motor->step(sb, next);
		CHECK(soldier_state_hash(sa) == soldier_state_hash(sb));
	}

	TEST_CASE("sim.motor: determinism: two worlds, 10,000 steps of a busy course, equal hash every tick; save / restore") {
		const std::vector<StaticTriangle> t = course_world();
		Rig a = rig_of(t, params_with(shipped_tuning()));
		Rig b = rig_of(t, params_with(shipped_tuning()));
		SoldierState sa = a.motor->spawn_state({ 30.0f, 0.02f, 30.0f }, 0, 1, 10, true);
		SoldierState sb = sa;
		std::mt19937 rng(20261015u);
		std::vector<Command> log;
		uint64_t chain = 0;
		std::array<uint32_t, kMoveModeCount> modes{};
		std::array<uint32_t, kPoseCount> poses{};
		uint32_t jumps = 0, landings = 0, refused = 0;
		Vec3 lo = sa.position, hi = sa.position;
		const auto start = std::chrono::steady_clock::now();
		for (int i = 0; i < 10000; ++i) {
			const Command c = busy_command(rng, sa.tick + 1);
			log.push_back(c);
			a.motor->step(sa, c);
			b.motor->step(sb, c);
			REQUIRE(soldier_state_hash(sa) == soldier_state_hash(sb));
			chain = chain * 0x100000001B3ull ^ soldier_state_hash(sa);
			++modes[size_t(sa.mode)];
			++poses[size_t(sa.pose)];
			jumps += (sa.events & soldier_events::kJumped) != 0 ? 1 : 0;
			landings += (sa.events & soldier_events::kLanded) != 0 ? 1 : 0;
			refused += (sa.events & soldier_events::kPoseRefused) != 0 ? 1 : 0;
			for (int k = 0; k < 3; ++k) {
				lo[size_t(k)] = std::min(lo[size_t(k)], sa.position[size_t(k)]);
				hi[size_t(k)] = std::max(hi[size_t(k)], sa.position[size_t(k)]);
			}
		}
		std::printf("busy course: modes ground %u jump %u air %u fall %u dead %u; poses %u/%u/%u; %u jumps, %u landings, %u refusals; "
					"x %.1f..%.1f y %.2f..%.2f z %.1f..%.1f\n",
				modes[0], modes[1], modes[2], modes[3], modes[8], poses[0], poses[1], poses[2], jumps, landings, refused, double(lo[0]), double(hi[0]),
				double(lo[1]), double(hi[1]), double(lo[2]), double(hi[2]));
		CHECK(modes[size_t(MoveMode::Dead)] == 0);
		CHECK(jumps > 20);
		CHECK(poses[size_t(Pose::Prone)] > 1000);
		CHECK(hi[1] > 0.5f);
		const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 20000.0;
		std::printf("determinism: 10,000 steps, chain %016llx, %.1f us per step (noisy: other builds run on this machine)\n",
				static_cast<unsigned long long>(chain), us);
		CHECK(chain == busy_course_chain());
		// Save at tick 300, run 300, restore, rerun: equal.
		SoldierState s = a.motor->spawn_state({ 30.0f, 0.02f, 30.0f }, 0, 1, 10, true);
		for (int i = 0; i < 300; ++i) {
			a.motor->step(s, log[size_t(i)]);
		}
		const SoldierState saved = s;
		std::vector<uint64_t> first;
		for (int i = 300; i < 600; ++i) {
			a.motor->step(s, log[size_t(i)]);
			first.push_back(soldier_state_hash(s));
		}
		s = saved;
		for (int i = 300; i < 600; ++i) {
			b.motor->step(s, log[size_t(i)]); // the other world's motor, after its own 10,000 steps
			REQUIRE(soldier_state_hash(s) == first[size_t(i - 300)]);
		}
	}

	TEST_CASE("sim.motor: determinism across processes (the busy course's hash chain in a child process)") {
		wchar_t exe[1024] = {};
		REQUIRE(GetModuleFileNameW(nullptr, exe, 1024) > 0);
		const std::wstring command = L"\"\"" + std::wstring(exe) + L"\" --test-case=\"sim.motor: busy course chain (child)\" --no-version 2>&1\"";
		FILE *pipe = _wpopen(command.c_str(), L"r");
		REQUIRE(pipe != nullptr);
		std::string out;
		char buffer[512];
		while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
			out += buffer;
		}
		const int rc = _pclose(pipe);
		const size_t at = out.find("BUSY CHAIN ");
		REQUIRE_MESSAGE(at != std::string::npos, out);
		const unsigned long long child = std::strtoull(out.c_str() + at + 11, nullptr, 16);
		std::printf("child process chain %016llx (rc %d)\n", child, rc);
		CHECK(rc == 0);
		CHECK(child == static_cast<unsigned long long>(busy_course_chain()));
	}

	TEST_CASE("sim.motor: busy course chain (child)") {
		std::printf("BUSY CHAIN %016llx\n", static_cast<unsigned long long>(busy_course_chain()));
	}

	TEST_CASE("sim.motor: interleaving: one motor stepping soldiers in turn gives each its isolated run") {
		const std::vector<StaticTriangle> t = course_world();
		const MotorParams params = params_with(shipped_tuning());
		Rig shared = rig_of(t, params);
		constexpr int kSoldiers = 8;
		std::vector<std::unique_ptr<SoldierMotor>> own;
		std::vector<SoldierState> together, alone;
		std::vector<std::mt19937> rngs;
		for (int i = 0; i < kSoldiers; ++i) {
			own.push_back(std::make_unique<SoldierMotor>());
			REQUIRE(own.back()->init(*shared.world, params).ok());
			// Mixed starts: some in the air, some on the ramp, poses differ through the commands.
			const Vec3 at = { 12.0f + 5.0f * float(i), i % 3 == 0 ? 3.0f : 0.02f, 8.0f + 3.0f * float(i % 4) };
			together.push_back(shared.motor->spawn_state(at, uint16_t(i * 8000), uint32_t(i + 1), 50, false));
			alone.push_back(together.back());
			rngs.emplace_back(1000u + uint32_t(i));
		}
		std::vector<std::mt19937> rngs2 = rngs;
		for (int tick = 0; tick < 1200; ++tick) {
			for (int i = 0; i < kSoldiers; ++i) {
				Command c = busy_command(rngs[size_t(i)], together[size_t(i)].tick + 1);
				shared.motor->step(together[size_t(i)], c);
				Command c2 = busy_command(rngs2[size_t(i)], alone[size_t(i)].tick + 1);
				c2.sequence += 17; // a different sequence and flags (the server's repeats) must not matter
				c2.flags = command_flags::kRepeated;
				own[size_t(i)]->step(alone[size_t(i)], c2);
				REQUIRE_MESSAGE(soldier_state_hash(together[size_t(i)]) == soldier_state_hash(alone[size_t(i)]), "soldier " << i << " tick " << tick);
			}
		}
		std::set<int> poses, modes;
		for (const SoldierState &s : together) {
			poses.insert(int(s.pose));
			modes.insert(int(s.mode));
		}
		std::printf("interleaving: 8 soldiers x 1200 ticks equal; end poses %zu kinds, modes %zu kinds\n", poses.size(), modes.size());
	}

	TEST_CASE("sim.motor: hostile states only advance the tick; unknown enum values do not crash") {
		Rig r = rig_of(flat_world(), instant_params());
		SoldierState s = at_rest(*r.motor, { 0.0f, 0.0f, 0.0f });
		for (int k = 0; k < 6; ++k) {
			SoldierState h = s;
			switch (k) {
				case 0:
					h.position[1] = std::nanf("");
					break;
				case 1:
					h.velocity[0] = INFINITY;
					break;
				case 2:
					h.position[0] = 1e30f;
					break;
				case 3:
					h.pose = Pose(7);
					break;
				case 4:
					h.target_pose = Pose(200);
					break;
				case 5:
					h.velocity[2] = -1e9f;
					break;
			}
			const SoldierState before = h;
			r.motor->step(h, move_command(h.tick + 1, kAxisMax, 0, 0, Pose(9), 0xFFFFFFu));
			CHECK(h.tick == before.tick + 1);
			CHECK(std::memcmp(h.position.data(), before.position.data(), sizeof(Vec3)) == 0);
		}
		// A command asking for an unknown pose keeps the pose; -128 on an axis reads as -127.
		SoldierState q = s;
		Command c = move_command(q.tick + 1, -128, -128, 0, Pose(3));
		r.motor->step(q, c);
		CHECK(q.pose == Pose::Stand);
		CHECK(horizontal(q.velocity) == doctest::Approx(4.0f * 0.75f).epsilon(1e-4));
	}

	TEST_CASE("sim.motor: PredictionRing find / newest_at_or_below / discard_after, at the ring's wrap") {
		for (const uint32_t capacity : { 128u, 100u, 1u }) {
			PredictionRing ring(capacity);
			CHECK(ring.capacity() == capacity);
			CHECK(ring.find(0) == nullptr);
			CHECK(ring.newest_at_or_below(1000) == nullptr);
			const auto put = [&](Tick p_tick) {
				PredictionEntry e;
				e.command.tick = p_tick;
				e.state.tick = p_tick;
				e.sent = true;
				ring.put(e);
			};
			put(5);
			CHECK(ring.find(5) != nullptr);
			CHECK(ring.find(5 + capacity) == nullptr); // same slot, other tick: never returned
			put(5 + capacity);
			CHECK(ring.find(5) == nullptr);
			CHECK(ring.find(5 + capacity) != nullptr);
			ring.clear();
			CHECK(ring.find(5 + capacity) == nullptr);
			if (capacity < 16) {
				continue;
			}
			// Ticks capacity - 8 .. capacity + 7 wrap round the slot array.
			const Tick lo = capacity - 8, hi = capacity + 7;
			for (Tick t = lo; t <= hi; ++t) {
				put(t);
			}
			for (Tick t = lo; t <= hi; ++t) {
				REQUIRE(ring.find(t) != nullptr);
				CHECK(ring.find(t)->state.tick == t);
			}
			CHECK(ring.newest_at_or_below(hi + 50)->state.tick == hi);
			CHECK(ring.newest_at_or_below(lo)->state.tick == lo);
			CHECK(ring.newest_at_or_below(lo - 1) == nullptr);
			CHECK(ring.newest_at_or_below(hi + capacity) == nullptr); // more than capacity ticks back: not searched
			ring.discard_after(capacity + 2);
			CHECK(ring.find(capacity + 2) != nullptr);
			CHECK(ring.find(capacity + 3) == nullptr);
			CHECK(ring.find(hi) == nullptr);
			CHECK(ring.find(lo) != nullptr);
			CHECK(ring.newest_at_or_below(hi)->state.tick == capacity + 2);
			ring.discard_after(0);
			CHECK(ring.newest_at_or_below(hi) == nullptr);
			// Tick 0 and the ticks just above it.
			put(0);
			CHECK(ring.newest_at_or_below(3)->state.tick == 0);
		}
	}

	TEST_CASE("sim.motor: catch_up from a mismatch, a forward and a backward reset, a 3 s stall and a spawn equals the server's stepping") {
		const std::vector<StaticTriangle> t = course_world();
		Rig server = rig_of(t, params_with(shipped_tuning()));
		Rig client = rig_of(t, params_with(shipped_tuning()));
		std::mt19937 rng(4242u);
		// The client's commands for every tick it predicted; the server receives all but a lost few.
		const SoldierState spawn = server.motor->spawn_state({ 30.0f, 0.02f, 30.0f }, 0, 1, 100, true);
		std::map<Tick, Command> sent;
		for (Tick tick = spawn.tick + 1; tick < spawn.tick + 600; ++tick) {
			Command c = busy_command(rng, tick);
			c.sequence = tick * 3;
			sent[tick] = c;
		}
		// The server: a client command when it arrived, else the last one repeated (hold_command before any).
		const auto server_run = [&](const SoldierState &p_from, Tick p_to, const std::set<Tick> &p_lost, const std::set<Tick> &p_unsent) {
			std::map<Tick, SoldierState> out;
			SoldierState s = p_from;
			Command last = hold_command(p_from);
			out[s.tick] = s;
			for (Tick tick = p_from.tick + 1; tick <= p_to; ++tick) {
				const bool have = sent.count(tick) != 0 && p_lost.count(tick) == 0 && p_unsent.count(tick) == 0;
				const Command c = have ? sent[tick] : repeated_command(last, tick);
				server.motor->step(s, c);
				out[tick] = s;
				last = c;
			}
			return out;
		};
		// The client predicted ticks spawn+1 .. C-1 with its sent commands (from the spawn state, as after Spawned).
		const auto client_ring = [&](Tick p_until, const std::set<Tick> &p_unsent) {
			PredictionRing ring(128);
			catch_up(*client.motor, spawn, ring, spawn.tick + 1);
			SoldierState s = spawn;
			for (Tick tick = spawn.tick + 1; tick < p_until; ++tick) {
				const bool real = p_unsent.count(tick) == 0;
				const Command c = real ? sent[tick] : repeated_command(ring.find(tick - 1)->command, tick);
				client.motor->step(s, c);
				ring.put({ c, s, real });
			}
			return ring;
		};
		const auto check_ring = [&](const PredictionRing &p_ring, Tick p_from, Tick p_until, const std::map<Tick, SoldierState> &p_server, const char *p_what) {
			for (Tick tick = p_from; tick < p_until; ++tick) {
				const PredictionEntry *e = p_ring.find(tick);
				REQUIRE_MESSAGE(e != nullptr, p_what << " tick " << tick);
				CHECK(e->command.tick == tick); // the step precondition held for every stored entry
				CHECK(e->state.tick == tick);
				REQUIRE_MESSAGE(soldier_state_hash(e->state) == soldier_state_hash(p_server.at(tick)), p_what << " tick " << tick);
			}
		};

		SUBCASE("mismatch: the server lost three commands, the snapshot at T differs, catch_up(S, C) re-derives T..C-1") {
			const std::set<Tick> lost = { 140, 141, 142 };
			const Tick T = 150, C = 160;
			PredictionRing ring = client_ring(C, {});
			const std::map<Tick, SoldierState> srv = server_run(spawn, C - 1, lost, {});
			REQUIRE(soldier_state_hash(ring.find(T)->state) != soldier_state_hash(srv.at(T)));
			const CatchUpStats st = catch_up(*client.motor, srv.at(T), ring, C);
			CHECK(st.steps == C - 1 - T);
			CHECK(st.synthesized == 0);
			check_ring(ring, T, C, srv, "mismatch");
		}
		SUBCASE("forward clock reset: no entry at C - 1, the base fill synthesises the repeats the server runs") {
			const Tick C_old = 200, C_new = 230;
			const std::set<Tick> unsent = [&] {
				std::set<Tick> u;
				for (Tick tick = C_old; tick < C_new; ++tick) {
					u.insert(tick);
				}
				return u;
			}();
			PredictionRing ring = client_ring(C_old, {});
			REQUIRE(ring.find(C_new - 1) == nullptr);
			const PredictionEntry *base = ring.newest_at_or_below(C_new - 1);
			REQUIRE(base != nullptr);
			REQUIRE(base->state.tick == C_old - 1);
			const SoldierState from = base->state;
			const CatchUpStats st = catch_up(*client.motor, from, ring, C_new);
			CHECK(st.steps == C_new - C_old);
			CHECK(st.synthesized == C_new - C_old);
			const std::map<Tick, SoldierState> srv = server_run(spawn, C_new - 1, {}, unsent);
			check_ring(ring, C_old - 1, C_new, srv, "forward");
			CHECK_FALSE(ring.find(C_new - 1)->sent);
			CHECK(ring.find(C_new - 1)->command == repeated_command(sent[C_old - 1], C_new - 1));
		}
		SUBCASE("backward clock reset: the entries after the new C - 1 are dropped and re-predicted with the newer commands") {
			const Tick C_old = 260, C_new = 245;
			PredictionRing ring = client_ring(C_old, {});
			ring.discard_after(C_new - 1);
			CHECK(ring.find(C_new) == nullptr);
			REQUIRE(ring.find(C_new - 1) != nullptr);
			// The client re-predicts C_new.. with new commands (the server takes the newer sequences, §7.4).
			SoldierState s = ring.find(C_new - 1)->state;
			for (Tick tick = C_new; tick < C_old; ++tick) {
				Command c = busy_command(rng, tick);
				c.sequence = 100000 + tick;
				sent[tick] = c;
				client.motor->step(s, c);
				ring.put({ c, s, true });
			}
			const std::map<Tick, SoldierState> srv = server_run(spawn, C_old - 1, {}, {});
			check_ring(ring, C_old - 128, C_old, srv, "backward");
		}
		SUBCASE("a 3 s stall: the ring holds nothing near C - 1; catch_up from the snapshots recovers") {
			const Tick last_predicted = 199, C = 200 + 180;
			// The client stalls after 199 and sends nothing for 200..379; the server repeats sent[199] for those ticks,
			// then applies the client's commands again from 380.
			const std::set<Tick> unsent = [&] {
				std::set<Tick> u;
				for (Tick tick = last_predicted + 1; tick < C; ++tick) {
					u.insert(tick);
				}
				return u;
			}();
			PredictionRing ring = client_ring(last_predicted + 1, {});
			REQUIRE(ring.newest_at_or_below(C - 1) == nullptr); // 180 ticks back is past the ring
			const std::map<Tick, SoldierState> srv = server_run(spawn, C + 20, {}, unsent);
			// First snapshot after the stall (T = 375, no ring entry): the catch-up assumes the hold command (prediction.h),
			// so its synthesised ticks may differ from the server's repeats of sent[199] ...
			const CatchUpStats first = catch_up(*client.motor, srv.at(375), ring, C);
			CHECK(first.steps == 4);
			CHECK(ring.find(375)->command == hold_command(srv.at(375)));
			// ... the client predicts on with its real commands, and the next snapshot (T = 385, whose ring entry holds the
			// command the server applied) corrects it: from there the ring equals the server.
			SoldierState s = ring.find(C - 1)->state;
			for (Tick tick = C; tick < C + 10; ++tick) {
				client.motor->step(s, sent[tick]);
				ring.put({ sent[tick], s, true });
			}
			catch_up(*client.motor, srv.at(385), ring, C + 10);
			check_ring(ring, 385, C + 10, srv, "stall, second snapshot");
			// From a base inside the ring's range (older than its newest sent tick): the sent commands still held are
			// used and the rest repeat sent[199], exactly the server's ticks.
			PredictionRing ring2 = client_ring(last_predicted + 1, {});
			catch_up(*client.motor, srv.at(150), ring2, C);
			check_ring(ring2, C - 128, C, srv, "stall from an old base");
		}
		SUBCASE("spawn: an own state with a newer life at the spawn tick N; the ring had the dead soldier's entries") {
			PredictionRing ring = client_ring(300, {});
			const SoldierState respawn = server.motor->spawn_state({ 40.0f, 0.02f, 20.0f }, 4000, 2, 290, true);
			// After a spawn the server runs the client's commands for ticks > N it already holds (the client kept
			// sending while dead), else the hold command's repeats.
			std::map<Tick, SoldierState> srv;
			{
				SoldierState s = respawn;
				Command last = hold_command(respawn);
				srv[s.tick] = s;
				for (Tick tick = respawn.tick + 1; tick < 300; ++tick) {
					const Command c = sent[tick];
					server.motor->step(s, c);
					srv[tick] = s;
					last = c;
				}
				for (Tick tick = 300; tick < 306; ++tick) {
					const Command c = repeated_command(last, tick);
					server.motor->step(s, c);
					srv[tick] = s;
					last = c;
				}
			}
			catch_up(*client.motor, respawn, ring, 306);
			check_ring(ring, 290, 306, srv, "spawn");
			// A spawn on a client that had sent nothing after N: the hold command's repeats.
			PredictionRing empty(128);
			std::map<Tick, SoldierState> hold;
			{
				SoldierState s = respawn;
				Command last = hold_command(respawn);
				hold[s.tick] = s;
				for (Tick tick = respawn.tick + 1; tick < 296; ++tick) {
					last = repeated_command(last, tick);
					server.motor->step(s, last);
					hold[tick] = s;
				}
			}
			const CatchUpStats st = catch_up(*client.motor, respawn, empty, 296);
			CHECK(st.synthesized == 5);
			check_ring(empty, 290, 296, hold, "spawn, hold");
			CHECK(empty.find(290)->command == hold_command(respawn));
		}
		SUBCASE("synthesised entries are re-derived from a changed prev") {
			PredictionRing ring = client_ring(150, {});
			catch_up(*client.motor, ring.find(149)->state, ring, 160); // 150..159 synthesised from sent[149]
			CHECK(ring.find(155)->command == repeated_command(sent[149], 155));
			// The client re-predicted 149 with another command (a backward reset); a catch-up from 148 re-derives the rest.
			Command changed = sent[149];
			changed.move_forward = int8_t(-changed.move_forward);
			changed.yaw = uint16_t(changed.yaw + 5000);
			SoldierState s = ring.find(148)->state;
			client.motor->step(s, changed);
			ring.put({ changed, s, true });
			catch_up(*client.motor, ring.find(148)->state, ring, 160);
			CHECK(ring.find(155)->command == repeated_command(changed, 155));
			CHECK(ring.find(149)->command == changed);
		}
		SUBCASE("a base at or past p_until only stores the base; a gap beyond a minute is bounded") {
			PredictionRing ring(128);
			const CatchUpStats none = catch_up(*client.motor, spawn, ring, spawn.tick);
			CHECK(none.steps == 0);
			CHECK(ring.find(spawn.tick) != nullptr);
			const auto start = std::chrono::steady_clock::now();
			const CatchUpStats bounded = catch_up(*client.motor, spawn, ring, spawn.tick + 4000000000u);
			const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
			CHECK(bounded.steps == 60u * kSimHz);
			CHECK(s < 5.0);
			REQUIRE(ring.find(spawn.tick + 4000000000u - 1) != nullptr);
			CHECK(ring.find(spawn.tick + 4000000000u - 1)->command.tick == spawn.tick + 4000000000u - 1);
		}
	}

	TEST_CASE("sim.motor: settle_spawn on synthetic points; place_spawn + idle ticks + judge_settle equals settle_spawn; stamp_spawn") {
		std::vector<StaticTriangle> t;
		add_floor(t, 1.0f, 1.0f, 61.0f, 61.0f, 0.0f);
		add_box(t, { 20.0f, 0.0f, 10.0f }, { 21.0f, 3.0f, 30.0f }); // a wall face at x = 20
		add_box(t, { 40.0f, 0.0f, 40.0f }, { 50.0f, 0.5f, 50.0f }); // a 0.5 m platform
		add_box(t, { 5.0f, 1.2f, 50.0f }, { 15.0f, 1.4f, 58.0f }); // a crawl space: no room to stand
		Rig r = rig_of(t, params_with(shipped_tuning()));
		const float pad = r.motor->params().tuning.character_padding;
		struct Case {
			Vec3 at;
			bool ok;
			const char *what;
		};
		const Case cases[] = {
			{ { 10.0f, 0.0f, 10.0f }, true, "on the floor" },
			{ { 10.0f, 0.4f, 12.0f }, true, "40 cm above the floor" },
			{ { 10.0f, -0.3f, 14.0f }, true, "30 cm below the floor" },
			{ { 19.85f, 0.0f, 20.0f }, true, "inside the wall's reach (shifted out)" },
			{ { 45.0f, 0.45f, 45.0f }, true, "on the platform, a little low" },
			{ { 10.0f, 0.0f, 54.0f }, false, "under the crawl space" },
			{ { 10.0f, 5.0f, 30.0f }, false, "no floor within 2 m below" },
		};
		for (const Case &c : cases) {
			SpawnPoint p;
			p.position = c.at;
			p.yaw = 12345;
			p.index = 7;
			const SettleResult full = settle_spawn(*r.motor, *r.world, p, 3, 500);
			std::printf("settle %s: ok %d (%s) lift %.3f shift %.3f gap %.4f\n", c.what, full.ok ? 1 : 0, full.failure.c_str(), double(full.lift), double(full.shift),
					double(full.gap));
			CHECK_MESSAGE(full.ok == c.ok, c.what << ": " << full.failure);
			// The three pieces give the same geometric fields, bit for bit, at any start tick and life.
			SettleResult parts = place_spawn(*r.motor, *r.world, p, 9, 77777);
			if (parts.failure.empty()) {
				for (uint32_t i = 0; i < SettleOptions().settle_ticks; ++i) {
					const SoldierState &s = parts.state;
					r.motor->step(parts.state, idle_command(s.tick + 1, s.yaw, s.pitch, s.target_pose));
				}
				judge_settle(*r.motor, *r.world, p, SettleOptions(), parts);
			}
			CHECK(parts.ok == full.ok);
			CHECK(parts.failure == full.failure);
			CHECK(std::memcmp(parts.placed.data(), full.placed.data(), sizeof(Vec3)) == 0);
			CHECK(std::memcmp(parts.state.position.data(), full.state.position.data(), sizeof(Vec3)) == 0);
			CHECK(parts.lift == full.lift);
			CHECK(parts.shift == full.shift);
			CHECK(parts.gap == full.gap);
			CHECK(parts.overlapping == full.overlapping);
			if (!c.ok) {
				continue;
			}
			CHECK(full.gap >= -1e-4f);
			CHECK(full.gap <= pad + 0.01f);
			CHECK_FALSE(full.overlapping);
			// stamp_spawn: the state of a soldier spawning at tick 500 with full protection.
			const SoldierState &s = full.state;
			CHECK(s.tick == 500);
			CHECK(s.life == 3);
			CHECK(s.yaw == 12345);
			CHECK(s.events == soldier_events::kSpawned);
			CHECK(s.protect_ticks == 120);
			CHECK((s.flags & soldier_flags::kSpawnProtected) != 0);
			CHECK((s.flags & soldier_flags::kGrounded) != 0);
			CHECK(s.air_ticks == 0);
			CHECK(s.impact_speed == 0.0f);
			CHECK(s.health == 100.0f);
			CHECK(hold_command(s).yaw == 12345);
			// The server steps it from 501.
			SoldierState next = s;
			r.motor->step(next, repeated_command(hold_command(s), 501));
			CHECK(next.tick == 501);
			CHECK(next.position == s.position);
			CHECK(next.protect_ticks == 119);
		}
	}

	TEST_CASE("sim.motor: choose_spawn: team, protection radius, priority, seeded pick") {
		std::vector<SpawnPoint> points;
		for (int i = 0; i < 6; ++i) {
			SpawnPoint p;
			p.position = { float(i) * 20.0f, 0.0f, 0.0f };
			p.team = i == 0 ? 1 : (i == 1 ? 2 : 0);
			p.priority = i == 5 ? 0.5f : 1.0f;
			p.index = uint32_t(i);
			points.push_back(p);
		}
		// Team 2 never gets point 0 (team 1); point 5 has the lower priority.
		std::set<size_t> picks;
		for (uint64_t seed = 0; seed < 200; ++seed) {
			const size_t k = choose_spawn(points, 2, {}, 10.0f, seed);
			REQUIRE(k < points.size());
			picks.insert(k);
		}
		CHECK(picks == std::set<size_t>{ 1, 2, 3, 4 });
		CHECK(choose_spawn(points, 2, {}, 10.0f, 5) == choose_spawn(points, 2, {}, 10.0f, 5));
		// Live soldiers within 10 m block points; with 1..4 blocked only the low-priority 5 is left.
		const std::vector<Vec3> live = { { 20.0f, 0.0f, 3.0f }, { 40.0f, 0.0f, 0.0f }, { 60.0f, 1.0f, 0.0f }, { 85.0f, 0.0f, 0.0f } };
		CHECK(choose_spawn(points, 2, live, 10.0f, 1) == 5);
		CHECK(choose_spawn(points, 2, live, 1.0f, 1) != 5);
		CHECK(choose_spawn({}, 1, {}, 10.0f, 1) == SIZE_MAX);
		const std::vector<Vec3> all = { { 0, 0, 0 }, { 20, 0, 0 }, { 40, 0, 0 }, { 60, 0, 0 }, { 80, 0, 0 }, { 100, 0, 0 } };
		CHECK(choose_spawn(points, 1, all, 10.0f, 1) == SIZE_MAX);
	}

	TEST_CASE("sim.motor: find_clear_heading and find_drop_spot on a synthetic world") {
		std::vector<StaticTriangle> t;
		add_floor(t, 1.0f, 1.0f, 61.0f, 61.0f, 0.0f);
		add_box(t, { 1.0f, 0.0f, 33.0f }, { 61.0f, 3.0f, 34.0f }); // a wall 3 m north (+Z) of the start
		add_box(t, { 33.0f, 0.0f, 1.0f }, { 34.0f, 3.0f, 33.0f }); // and one 3 m east (+X)
		add_ramp(t, 1.0f, 33.0f, 5.0f, 0.0f, 10.0f, 10.0f, 2.0f); // a ramp to the south (z 5..15)
		add_box(t, { 40.0f, 6.0f, 40.0f }, { 61.0f, 6.2f, 61.0f }); // a roof over the north-east corner
		Rig r = rig_of(t, params_with(shipped_tuning()));
		const Vec3 feet = { 30.0f, 0.0f, 30.0f };
		const std::optional<uint16_t> yaw = find_clear_heading(*r.motor, *r.world, feet, 20.0f);
		REQUIRE(yaw.has_value());
		const float a = yaw_radians(*yaw);
		std::printf("clear heading %.1f deg\n", double(a) * 180.0 / 3.14159265);
		// It must avoid north (wall), east (wall) and south (the ramp at z 5..15 is 15+ m away... south-west runs onto it).
		const Vec3 end = { feet[0] + 20.0f * std::sin(a), 0.0f, feet[2] + 20.0f * std::cos(a) };
		CHECK(end[2] < 33.0f - 0.35f);
		CHECK(end[0] < 33.0f - 0.35f);
		CHECK(end[0] > 1.0f);
		// Walk the course: 20 m of level, straight, unobstructed path.
		SoldierState s = at_rest(*r.motor, feet, *yaw);
		run(*r.motor, s, 300, [&](Tick tick) { return move_command(tick, kAxisMax, 0, *yaw); });
		CHECK(horizontal(s.velocity) == doctest::Approx(4.0f).epsilon(0.01));
		CHECK(std::fabs(s.position[1]) < 1e-3f);
		// No 40 m course from the start: walls north and east, the ramp south, the floor ends west.
		CHECK_FALSE(find_clear_heading(*r.motor, *r.world, feet, 40.0f).has_value());
		// Under the low wall corner the crouch / prone start check fails too.
		CHECK_FALSE(find_clear_heading(*r.motor, *r.world, { 33.2f, 0.0f, 20.0f }, 5.0f).has_value());
		// The drop spot: level, 50 m clear above (not under the roof), x then z ascending from the bounds' corner.
		const std::optional<Vec3> drop = find_drop_spot(*r.motor, *r.world);
		REQUIRE(drop.has_value());
		std::printf("drop spot (%.2f, %.2f, %.2f)\n", double((*drop)[0]), double((*drop)[1]), double((*drop)[2]));
		// The scan runs x then z ascending on a 5 m grid from the bounds' corner (1, 1): x = 1 has no floor half a metre
		// west, (6, 1) none half a metre south, (6, 6) and (6, 11) are on the 10 degree slope, so the first level 1 m
		// patch is the ramp's 2 m top at (6, 16), y = 10 tan 10 deg.
		CHECK((*drop)[0] == doctest::Approx(6.0f));
		CHECK((*drop)[2] == doctest::Approx(16.0f));
		CHECK((*drop)[1] == doctest::Approx(10.0f * std::tan(10.0f * kPi / 180.0f)).epsilon(1e-4));
		const bool under_roof = (*drop)[0] > 40.0f && (*drop)[2] > 40.0f;
		CHECK_FALSE(under_roof);
		CHECK_FALSE(find_drop_spot(*r.motor, *r.world, 50.0f, 0.0f).has_value());
		CHECK_FALSE(find_drop_spot(*r.motor, *r.world, std::nanf("")).has_value());
		// The E1d drop there: the fall is exactly h.
		SoldierState d = r.motor->spawn_state({ (*drop)[0], (*drop)[1] + 10.1f, (*drop)[2] }, 0, 1, 10, false);
		run(*r.motor, d, 200, [&](Tick tick) { return idle_command(tick, 0, 0, Pose::Stand); });
		CHECK(d.impact_speed == doctest::Approx(std::sqrt(2.0f * 9.81f * 10.1f)).epsilon(1e-4));
		CHECK(d.health < 81.0f);
	}

	TEST_CASE("sim.motor: to_spawn_points: order, yaw from the forward axis, team and priority") {
		core::level::LevelSpawns spawns;
		for (int i = 0; i < 4; ++i) {
			core::level::SpawnPointDef d;
			d.object = uint32_t(100 + i);
			d.transform.trans = { float(i), 60.0f, -float(i) };
			const float a = float(i) * kPi * 0.5f; // 0, 90, 180, 270 degrees
			d.transform.forward = { std::sin(a), 0.0f, std::cos(a) };
			d.team = i == 3 ? 2 : 0;
			d.priority = i == 2 ? 0.5f : 1.0f;
			spawns.points.push_back(d);
		}
		const std::vector<SpawnPoint> p = to_spawn_points(spawns);
		REQUIRE(p.size() == 4);
		CHECK(p[0].yaw == 0);
		CHECK(p[1].yaw == 16384);
		CHECK(p[2].yaw == 32768);
		CHECK(p[3].yaw == 49152);
		CHECK(p[3].team == 2);
		CHECK(p[2].priority == 0.5f);
		CHECK(p[1].index == 1);
		CHECK(p[2].position == Vec3{ 2.0f, 60.0f, -2.0f });
	}
	TEST_CASE("sim.motor: floor cracks, slopes by walls and edges (found on Metro): settle and the ground hug") {
		std::vector<StaticTriangle> t;
		// Two floors with a 1 cm crack at x = 30 (rays fall through it), a 27 degree ramp with a wall 0.35 m beside
		// the spawn point on it, and a 10 cm slab whose edge a soldier stands on.
		add_floor(t, 1.0f, 1.0f, 30.0f, 61.0f, 0.0f);
		add_floor(t, 30.01f, 1.0f, 61.0f, 61.0f, 0.0f);
		add_ramp(t, 40.0f, 50.0f, 30.0f, 0.0f, 27.0f, 10.0f, 2.0f);
		add_box(t, { 43.65f, 0.0f, 30.0f }, { 44.0f, 8.0f, 42.0f });
		add_box(t, { 10.0f, 0.0f, 10.0f }, { 15.0f, 0.1f, 15.0f });
		Rig r = rig_of(t, params_with(shipped_tuning()));
		const float pad = r.motor->params().tuning.character_padding;
		const float tol = SettleOptions().hover_tolerance;
		const float slope_y = 5.0f * std::tan(27.0f * kPi / 180.0f);
		struct Case {
			Vec3 at;
			const char *what;
			bool shifted;
		};
		const Case cases[] = {
			{ { 30.005f, 0.0f, 20.0f }, "over the crack (the centre ray falls through; the rings find the floor)", true },
			{ { 44.35f, slope_y, 35.0f }, "on the slope, 0.35 m from the wall (lifts at a ring sample)", true },
			{ { 9.8f, 0.0f, 12.0f }, "at the slab's edge (rests on it: the drop gap is 0, the ray's 0.1 m)", false },
		};
		for (const Case &c : cases) {
			SpawnPoint p;
			p.position = c.at;
			const SettleResult s = settle_spawn(*r.motor, *r.world, p, 1, 100);
			std::printf("settle %s: ok %d (%s) lift %.3f shift %.3f gap %.5f, at (%.3f %.4f %.3f)\n", c.what, s.ok ? 1 : 0, s.failure.c_str(), double(s.lift),
					double(s.shift), double(s.gap), double(s.state.position[0]), double(s.state.position[1]), double(s.state.position[2]));
			CHECK_MESSAGE(s.ok, c.what << ": " << s.failure);
			CHECK(std::fabs(s.gap) <= pad + tol);
			CHECK((s.shift > 0.05f) == c.shifted); // the slab-edge soldier slides < 5 mm while settling
		}
		// Walking and sprinting across the crack: grounded on every tick, never InAir (the hug's probe is a sphere,
		// so the crack is no ledge; with a ray probe a crack on Metro, at point 22, left a crouching soldier InAir with
		// InAir speeds for a tick: this synthetic crack keeps Jolt OnGround either way). The far
		// floor's boundary edge lies inside the solver's character padding, so CharacterVirtual lifts the soldier off
		// it for a tick or two (5 mm walking, 11 mm sprinting: the Metro seam bumps, docs §6.1 "As built in T2", an
		// open issue). Bounded here, and the speed is back within 1 % by the end.
		for (const uint32_t b : { 0u, buttons::kSprint }) {
			SoldierState s = at_rest(*r.motor, { 26.0f, 0.0f, 20.0f }, 16384);
			const std::vector<SoldierState> trace = run(*r.motor, s, 90, [&](Tick tick) { return move_command(tick, kAxisMax, 0, 16384, Pose::Stand, b); });
			REQUIRE(trace.back().position[0] > 31.0f);
			const float expected = b != 0 ? 6.5f : 4.0f;
			size_t airborne = 0;
			float lo_y = 0.0f, hi_y = 0.0f, lo_v = 1e9f;
			for (size_t i = 0; i < trace.size(); ++i) {
				const SoldierState &e = trace[i];
				airborne += grounded(e) && e.mode == MoveMode::OnGround ? 0 : 1;
				lo_y = std::min(lo_y, e.position[1]);
				hi_y = std::max(hi_y, e.position[1]);
				lo_v = i >= 35 ? std::min(lo_v, horizontal(e.velocity)) : lo_v; // past the run-up
			}
			std::printf("crossing the crack (%s): %zu ticks not grounded, y %.5f..%.5f, lowest speed %.5f, end speed %.5f\n", b != 0 ? "sprint" : "walk",
					airborne, double(lo_y), double(hi_y), double(lo_v), double(horizontal(trace.back().velocity)));
			CHECK(airborne == 0);
			CHECK(lo_y > -2e-3f);
			CHECK(hi_y < 0.015f);
			CHECK(lo_v >= 0.95f * expected);
			CHECK(horizontal(trace.back().velocity) == doctest::Approx(expected).epsilon(0.01));
		}
	}
}

// ---- sim-motor: Metro's real static world (install; exits 77 without one) -------------------------------------

namespace {

// MP_Subway TeamDeathMatch0 loaded once per process: the soup through T1's build_static_world, the soldier with the
// shipped tuning, the spawn points. The snapshot is taken before the first install read.
struct Metro {
	std::unique_ptr<core::InstallReader> reader;
	std::shared_ptr<core::Install> install;
	core::InstallSnapshot before;
	core::level::LevelCollision collision;
	std::shared_ptr<const StaticWorldShapes> shapes;
	std::unique_ptr<CollisionWorld> world;
	gameplay::SoldierDef soldier;
	MotorParams params;
	std::unique_ptr<SoldierMotor> motor;
	core::level::LevelSpawns spawns;
	std::vector<SpawnPoint> points;
	double seconds_soup = 0, seconds_shapes = 0;
};

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

// nullptr (and the case marked skipped) without an install.
Metro *metro() {
	static std::unique_ptr<Metro> s_metro;
	static bool s_tried = false;
	if (s_tried) {
		if (s_metro == nullptr) {
			bf3::test::skip_install_case("no BF3 install (see the first sim-motor case)");
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
	auto start = std::chrono::steady_clock::now();
	core::Result<core::level::LevelCollision> c = core::level::load_level_collision(m->install, "MP_Subway", "TeamDeathMatch0");
	REQUIRE_MESSAGE(c.ok(), (c.ok() ? std::string() : c.error().message));
	m->collision = std::move(*c);
	m->seconds_soup = seconds_since(start);
	start = std::chrono::steady_clock::now();
	core::Result<std::shared_ptr<const StaticWorldShapes>> shapes = build_static_world(m->collision.soup);
	REQUIRE_MESSAGE(shapes.ok(), (shapes.ok() ? std::string() : shapes.error().message));
	m->shapes = *shapes;
	m->seconds_shapes = seconds_since(start);
	m->world = world_of(m->shapes);
	core::Result<gameplay::SoldierDef> soldier = gameplay::read_soldier_def(m->collision.level->db());
	REQUIRE_MESSAGE(soldier.ok(), (soldier.ok() ? std::string() : soldier.error().message));
	m->soldier = std::move(*soldier);
	core::Result<MotorParams> params = make_motor_params(m->soldier, shipped_tuning());
	REQUIRE_MESSAGE(params.ok(), (params.ok() ? std::string() : params.error().message));
	m->params = *params;
	m->motor = std::make_unique<SoldierMotor>();
	const core::Status s = m->motor->init(*m->world, m->params);
	REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
	core::Result<core::level::LevelSpawns> spawns = core::level::read_level_spawns(*m->collision.world);
	REQUIRE_MESSAGE(spawns.ok(), (spawns.ok() ? std::string() : spawns.error().message));
	m->spawns = std::move(*spawns);
	m->points = to_spawn_points(m->spawns);
	const Vec3 lo = m->shapes->min(), hi = m->shapes->max();
	std::printf("Metro: soup %.2f s, shapes %.2f s, %zu tiles, bounds (%.1f, %.1f, %.1f)..(%.1f, %.1f, %.1f), %zu spawn points\n", m->seconds_soup,
			m->seconds_shapes, m->shapes->tile_count(), double(lo[0]), double(lo[1]), double(lo[2]), double(hi[0]), double(hi[1]), double(hi[2]),
			m->points.size());
	s_metro = std::move(m);
	return s_metro.get();
}

float heading_distance(const Vec3 &p_from, const Vec3 &p_to, uint16_t p_yaw) {
	const float a = yaw_radians(p_yaw);
	return (p_to[0] - p_from[0]) * std::sin(a) + (p_to[2] - p_from[2]) * std::cos(a);
}

} // namespace

TEST_SUITE("sim-motor") {
	TEST_CASE("sim.motor: Metro: make_motor_params from the level's soldier with the shipped tuning equals docs 6.4") {
		Metro *m = metro();
		if (m == nullptr) {
			return;
		}
		const MotorParams &p = m->params;
		CHECK(p.tuning == shipped_tuning());
		CHECK(p.radius == 0.35f);
		CHECK(p.mass == 100.0f);
		CHECK(p.max_ascend_degrees == 50.0f);
		CHECK(p.allowed_distance_from_ground == 0.5f);
		CHECK(p.ground_hugging);
		CHECK(p.jump_height == doctest::Approx(0.6f));
		CHECK(p.jump_penalty_ticks == 60);
		CHECK(p.jump_penalty_factor == doctest::Approx(0.3f));
		CHECK(p.free_fall_velocity == 16.0f);
		CHECK(p.max_health == 100.0f);
		CHECK(p.immortal_ticks == 120);
		CHECK(eval(p.fall_damage_vertical, 13.8f) == 0.0f);
		CHECK(eval(p.fall_damage_vertical, 14.08f) == doctest::Approx(20.2f).epsilon(0.01));
		CHECK(eval(p.fall_damage_vertical, 30.04f) >= 100.0f);
		const PoseParams &stand = p.poses[size_t(Pose::Stand)];
		const PoseParams &crouch = p.poses[size_t(Pose::Crouch)];
		const PoseParams &prone = p.poses[size_t(Pose::Prone)];
		CHECK(stand.height == doctest::Approx(1.8f));
		CHECK(crouch.height == doctest::Approx(1.15f));
		CHECK(prone.height == doctest::Approx(0.3f));
		CHECK(stand.eye_height == doctest::Approx(1.6f));
		CHECK(crouch.eye_height == doctest::Approx(1.0f));
		CHECK(prone.eye_height == doctest::Approx(0.3f));
		for (const PoseParams *q : { &stand, &crouch, &prone }) {
			CHECK(q->step_height == doctest::Approx(0.3f));
			REQUIRE(q->throttle.count == 3);
			CHECK(q->throttle.points[1].x == doctest::Approx(0.1f));
			CHECK(q->throttle.points[1].y == 0.0f);
			CHECK(q->throttle.points[2].x == 1.0f);
			CHECK(q->throttle.points[2].y == 1.0f);
			CHECK(q->on_ground.acceleration_gain == doctest::Approx(0.35f));
			CHECK(q->on_ground.deceleration_gain == doctest::Approx(-15.0f));
			CHECK(q->in_air.velocity == doctest::Approx(6.0f));
			CHECK(q->in_air.backward == doctest::Approx(0.5f));
			CHECK(q->in_air.left == doctest::Approx(0.5f));
			CHECK(q->in_air.acceleration_gain == doctest::Approx(0.1f));
			CHECK(q->in_air.sprint_multiplier == doctest::Approx(1.5f));
			CHECK(q->falling.velocity == doctest::Approx(15.0f));
			CHECK(q->falling.acceleration_gain == doctest::Approx(0.05f));
		}
		CHECK(stand.transition_ticks == std::array<uint16_t, 3>{ 0, 18, 42 });
		CHECK(crouch.transition_ticks == std::array<uint16_t, 3>{ 18, 0, 30 });
		CHECK(prone.transition_ticks == std::array<uint16_t, 3>{ 42, 63, 0 });
		CHECK(stand.on_ground.velocity == doctest::Approx(4.0f));
		CHECK(stand.on_ground.backward == doctest::Approx(0.75f));
		CHECK(stand.on_ground.left == doctest::Approx(0.75f));
		CHECK(stand.on_ground.right == doctest::Approx(0.75f));
		CHECK(stand.on_ground.sprint_gain == doctest::Approx(0.25f));
		CHECK(stand.on_ground.sprint_multiplier == doctest::Approx(1.625f));
		CHECK(crouch.on_ground.velocity == doctest::Approx(2.0f));
		CHECK(crouch.on_ground.backward == doctest::Approx(0.8f));
		CHECK(crouch.on_ground.left == doctest::Approx(0.8f));
		CHECK(crouch.on_ground.sprint_multiplier == 0.0f);
		CHECK(prone.on_ground.velocity == doctest::Approx(0.9f));
		CHECK(prone.on_ground.backward == doctest::Approx(0.5f));
		CHECK(prone.on_ground.left == doctest::Approx(0.7f));
		CHECK(prone.on_ground.sprint_multiplier == 0.0f);
		CHECK(prone.box_min_expand[2] == doctest::Approx(1.3f));
		CHECK(prone.box_max_expand[2] == doctest::Approx(1.1f));
		// The synthetic soldier of sim-motor-unit is these numbers: the hashes of the two params (every field) agree.
		const uint64_t metro_hash = motor_params_hash(p);
		const uint64_t synthetic_hash = motor_params_hash(params_with(shipped_tuning()));
		std::printf("Metro params hash %016llx, synthetic %016llx\n", static_cast<unsigned long long>(metro_hash), static_cast<unsigned long long>(synthetic_hash));
		CHECK(metro_hash == synthetic_hash);
	}

	TEST_CASE("sim.motor: Metro: the 102 TDM spawn points and the two spawners") {
		Metro *m = metro();
		if (m == nullptr) {
			return;
		}
		REQUIRE(m->points.size() == 102);
		float lo = 1e9f, hi = -1e9f;
		for (size_t i = 0; i < m->points.size(); ++i) {
			const SpawnPoint &p = m->points[i];
			CHECK(p.index == i);
			CHECK(p.team == 0);
			CHECK(p.priority == 1.0f);
			CHECK((std::isfinite(p.position[0]) && std::isfinite(p.position[1]) && std::isfinite(p.position[2])));
			lo = std::min(lo, p.position[1]);
			hi = std::max(hi, p.position[1]);
		}
		std::printf("Metro spawn points: y %.3f..%.3f\n", double(lo), double(hi));
		CHECK(lo > 59.0f);
		CHECK(hi < 74.0f);
		REQUIRE(m->spawns.spawners.size() == 2);
		std::set<int> teams;
		for (const core::level::CharacterSpawnerDef &s : m->spawns.spawners) {
			teams.insert(s.team);
			CHECK(s.protection_radius == 10.0f);
		}
		CHECK(teams == std::set<int>{ 1, 2 });
	}

	TEST_CASE("sim.motor: Metro E3: settle_spawn on all 102 points (all ok; worst lift, shift and gap printed)") {
		Metro *m = metro();
		if (m == nullptr) {
			return;
		}
		const float pad = m->params.tuning.character_padding;
		const auto start = std::chrono::steady_clock::now();
		size_t ok = 0;
		float worst_lift = 0, worst_shift = 0, worst_gap = 0, min_gap = 1e9f;
		size_t lifted = 0, shifted = 0;
		for (const SpawnPoint &p : m->points) {
			const SettleResult r = settle_spawn(*m->motor, *m->world, p, 1, 1000);
			CHECK_MESSAGE(r.ok, "point " << p.index << ": " << r.failure);
			ok += r.ok ? 1 : 0;
			if (std::fabs(r.lift) > std::fabs(worst_lift)) {
				worst_lift = r.lift;
			}
			worst_shift = std::max(worst_shift, r.shift);
			worst_gap = std::max(worst_gap, r.gap);
			min_gap = std::min(min_gap, r.gap);
			lifted += std::fabs(r.lift) > 0.05f ? 1 : 0;
			shifted += r.shift > 0.0f ? 1 : 0;
			if (r.ok) {
				CHECK(r.gap <= pad + SettleOptions().hover_tolerance);
				CHECK_FALSE(r.overlapping);
				CHECK((r.state.flags & soldier_flags::kGrounded) != 0);
				CHECK(r.state.tick == 1000);
				CHECK(r.state.protect_ticks == m->params.immortal_ticks);
			}
		}
		std::printf("E3 Metro settle: %zu / %zu ok in %.3f s; worst lift %.4f m (%zu over 5 cm), worst shift %.3f m (%zu shifted), gap %.5f..%.5f m\n", ok,
				m->points.size(), seconds_since(start), double(worst_lift), lifted, double(worst_shift), shifted, double(min_gap), double(worst_gap));
		CHECK(ok == 102);
	}

	TEST_CASE("sim.motor: Metro E1a-E1c at 6 spawn points along find_clear_heading courses") {
		Metro *m = metro();
		if (m == nullptr) {
			return;
		}
		const SoldierMotor &motor = *m->motor;
		struct Course {
			const char *name;
			Pose pose;
			uint32_t buttons;
			float expected;
		};
		const Course courses[] = { { "walk", Pose::Stand, 0, 4.0f }, { "sprint", Pose::Stand, buttons::kSprint, 6.5f }, { "crouch", Pose::Crouch, 0, 2.0f },
			{ "prone", Pose::Prone, 0, 0.9f } };
		struct Change {
			Pose from, to;
			uint32_t ticks;
		};
		const Change changes[] = { { Pose::Stand, Pose::Crouch, 18 }, { Pose::Stand, Pose::Prone, 42 }, { Pose::Crouch, Pose::Stand, 18 },
			{ Pose::Crouch, Pose::Prone, 30 }, { Pose::Prone, Pose::Stand, 42 }, { Pose::Prone, Pose::Crouch, 63 } };
		size_t used = 0, skipped = 0;
		for (const SpawnPoint &p : m->points) {
			if (used == 6) {
				break;
			}
			const SettleResult settled = settle_spawn(motor, *m->world, p, 1, 1000);
			if (!settled.ok) {
				++skipped; // E3 reports it
				continue;
			}
			const Vec3 feet = settled.state.position;
			const std::optional<uint16_t> heading = find_clear_heading(motor, *m->world, feet, 20.0f);
			if (!heading) {
				++skipped;
				continue;
			}
			++used;
			const uint16_t yaw = *heading;
			// The settled state turned to the course heading (one idle tick at that yaw, still at rest).
			SoldierState base = settled.state;
			run(motor, base, 1, [&](Tick t) { return idle_command(t, yaw, 0, Pose::Stand); });
			REQUIRE(grounded(base));
			std::printf("Metro course %zu: point %u at (%.2f, %.3f, %.2f), heading %.1f deg\n", used, p.index, double(feet[0]), double(feet[1]),
					double(feet[2]), double(yaw_radians(yaw)) * 180.0 / 3.14159265358979323846);
			// E1a: take the pose at the start, then 120 ticks full forward; the steady window is ticks 60-120.
			for (const Course &c : courses) {
				SoldierState s = base;
				run(motor, s, 80, [&](Tick t) { return idle_command(t, yaw, 0, c.pose); });
				REQUIRE(s.pose == c.pose);
				const std::vector<SoldierState> trace = run(motor, s, 120, [&](Tick t) { return move_command(t, kAxisMax, 0, yaw, c.pose, c.buttons); });
				float lo = 1e9f, hi = 0.0f, rise = 0.0f;
				size_t slow = 0, bumps = 0;
				for (size_t i = 59; i < 120; ++i) {
					const float v = horizontal(trace[i].velocity);
					lo = std::min(lo, v);
					hi = std::max(hi, v);
					rise = std::max(rise, std::fabs(trace[i].position[1] - feet[1]));
					slow += v < 0.99f * c.expected ? 1 : 0;
				}
				for (size_t i = 1; i < 120; ++i) {
					bumps += std::fabs(trace[i].position[1] - trace[i - 1].position[1]) > 2e-4f ? 1 : 0; // a seam's ghost contact or crack (printed)
				}
				const float mean = heading_distance(trace[59].position, trace[119].position, yaw) / (60.0f * kTickSeconds);
				std::printf("  E1a %s: speed %.5f..%.5f m/s, mean %.5f (expected %.2f), |dy| %.4f m, %zu ticks under 99 %%, %zu seam bumps\n", c.name,
						double(lo), double(hi), double(mean), double(c.expected), double(rise), slow, bumps);
				// The window's mean speed is held to +-1 % and no tick may exceed +1 %. Per tick the floor is 95 %, not
				// 99 % as on the synthetic floor: Metro's floors have unwelded seams between meshes (flat, < 2 mm apart in
				// height) or cracks where Jolt's CharacterVirtual takes a ghost contact for a tick (a 0.5-14 mm lift or dip, up to 3.5 %
				// of the speed, regained by the acceleration gain within ~30 ticks); docs/design/m1.md §6.1 "As built in
				// T2", an open issue. The synthetic E1a in sim-motor-unit keeps the per-tick +-1 %.
				CHECK_MESSAGE(lo >= 0.95f * c.expected, "point " << p.index << " " << c.name);
				CHECK_MESSAGE(hi <= 1.01f * c.expected, "point " << p.index << " " << c.name);
				CHECK_MESSAGE(std::fabs(mean - c.expected) <= 0.01f * c.expected, "point " << p.index << " " << c.name);
				CHECK(grounded(trace.back()));
			}
			// E1b: a jump from rest at the start.
			{
				SoldierState s = base;
				const float take_off = s.position[1];
				const Tick jump_tick = s.tick + 1;
				const std::vector<SoldierState> trace =
						run(motor, s, 90, [&](Tick t) { return move_command(t, 0, 0, yaw, Pose::Stand, t == jump_tick ? buttons::kJump : 0); });
				float apex = -1.0f;
				bool landed = false;
				for (const SoldierState &e : trace) {
					apex = std::max(apex, e.position[1] - take_off);
					landed = landed || (e.events & soldier_events::kLanded) != 0;
				}
				std::printf("  E1b apex %.6f m, landed %d, end dy %.2e\n", double(apex), landed ? 1 : 0, double(trace.back().position[1] - take_off));
				CHECK_MESSAGE(std::fabs(apex - 0.6f) <= 0.001f, "point " << p.index);
				CHECK(landed);
				CHECK(grounded(trace.back()));
			}
			// E1c: the six transitions at the start.
			std::string ticks;
			for (const Change &c : changes) {
				SoldierState s = base;
				run(motor, s, 90, [&](Tick t) { return idle_command(t, yaw, 0, c.from); });
				REQUIRE(s.pose == c.from);
				const std::vector<SoldierState> trace = run(motor, s, 120, [&](Tick t) { return idle_command(t, yaw, 0, c.to); });
				int64_t started = -1, finished = -1;
				for (const SoldierState &e : trace) {
					if (started < 0 && (e.events & soldier_events::kPoseStarted) != 0) {
						started = int64_t(e.tick);
					} else if (started >= 0 && finished < 0 && e.transition_ticks == 0 && (e.events & soldier_events::kPoseFinished) != 0) {
						finished = int64_t(e.tick);
					}
				}
				const int64_t n = started >= 0 && finished >= 0 ? finished - started + 1 : -1;
				ticks += " " + std::to_string(n);
				CHECK_MESSAGE(n == int64_t(c.ticks), "point " << p.index << ": " << int(c.from) << " -> " << int(c.to));
				CHECK(started == int64_t(trace.front().tick));
			}
			std::printf("  E1c ticks:%s\n", ticks.c_str());
		}
		std::printf("Metro E1a-E1c: %zu courses, %zu points without a 20 m heading skipped\n", used, skipped);
		CHECK(used == 6);
	}

	TEST_CASE("sim.motor: Metro E1d drops of 9.8 / 10.1 / 46 m at find_drop_spot (fall = h within 1 mm, impact within 1e-3 m/s)") {
		Metro *m = metro();
		if (m == nullptr) {
			return;
		}
		const auto start = std::chrono::steady_clock::now();
		const std::optional<Vec3> drop = find_drop_spot(*m->motor, *m->world);
		REQUIRE(drop.has_value());
		std::printf("Metro drop spot (%.2f, %.4f, %.2f) found in %.3f s\n", double((*drop)[0]), double((*drop)[1]), double((*drop)[2]), seconds_since(start));
		struct Drop {
			float h;
			float min_damage, max_damage;
			bool dies;
		};
		const Drop drops[] = { { 9.8f, 0, 0, false }, { 10.1f, 20, 21, false }, { 46.0f, 100, 100, true } };
		for (const Drop &d : drops) {
			// A Teleport at rest with the feet h above the floor (the feet rest on the floor), no spawn protection.
			SoldierState s = m->motor->spawn_state({ (*drop)[0], (*drop)[1] + d.h, (*drop)[2] }, 0, 1, 5000, false);
			const float top = s.position[1];
			SoldierState landed;
			bool seen = false;
			for (int i = 0; i < 600 && !seen; ++i) {
				m->motor->step(s, idle_command(s.tick + 1, 0, 0, Pose::Stand));
				if ((s.events & soldier_events::kLanded) != 0) {
					landed = s;
					seen = true;
				}
			}
			REQUIRE(seen);
			const float fall = top - landed.position[1];
			const float expected = std::sqrt(2.0f * 9.81f * d.h);
			std::printf("E1d Metro %.1f m: fell %.5f m, impact %.5f m/s (sqrt(2gh) %.5f), damage %.3f, health %.3f\n", double(d.h), double(fall),
					double(landed.impact_speed), double(expected), double(landed.damage), double(landed.health));
			CHECK(std::fabs(fall - d.h) <= 0.001f);
			CHECK(std::fabs(landed.impact_speed - expected) <= 1e-3f);
			CHECK(landed.damage >= d.min_damage);
			CHECK(landed.damage <= d.max_damage);
			CHECK(((landed.events & soldier_events::kDied) != 0) == d.dies);
			CHECK((landed.mode == MoveMode::Dead) == d.dies);
		}
	}

	TEST_CASE("sim.motor: Metro: install unchanged (layout.toc, cas.cat, the .sb / .toc files)") {
		Metro *m = metro();
		if (m == nullptr) {
			return;
		}
		const core::SnapshotDiff diff = core::diff_snapshots(m->before, m->reader->snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
		std::printf("install.unchanged: %s\n", diff.clean() ? "clean" : diff.summary().c_str());
	}
}
