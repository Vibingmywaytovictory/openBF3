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
#include <random>
#include <sstream>
#include <string>
#include <vector>

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
	gameplay::PoseDef crouch = stand;
	crouch.pose = SoldierPose::Crouch;
	crouch.height = 1.15f;
	crouch.eye_height = 1.0f;
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



}
