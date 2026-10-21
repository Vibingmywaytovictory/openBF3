#include <doctest/doctest.h>

#include "bf3/core/ebx.h"
#include "bf3/core/gameplay/vehicle_drive.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/collision_soup.h"
#include "bf3/core/level/vehicle_spawns.h"
#include "bf3/core/thread_pool.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/vehicle_ghost.h"
#include "bf3/sim/vehicle_model.h"
#include "bf3/sim/vehicle_types.h"
#include "bf3/sim/vehicle_world.h"
#include "bf3/sim_harness/vehicle_course.h"
#include "bf3/sim_harness/vehicle_replay.h"
#include "collision_world_internal.h"
#include "test_support.h"
#include "vehicle_world_internal.h"

#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace bf3;
using namespace bf3::sim;
namespace gp = bf3::core::gameplay;

namespace {

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

std::string read_text(const char *p_path) {
	std::ifstream in(p_path, std::ios::binary);
	std::ostringstream s;
	s << in.rdbuf();
	return s.str();
}

#ifdef OPENBF3_S2_ASAN
constexpr bool kAsan = true;
#else
constexpr bool kAsan = false;
#endif

gp::VehicleDriveDef humvee_def() {
	gp::VehicleDriveDef d;
	d.blueprint = "fixture/humveearmored";
	gp::ChassisConfigDef &c = d.chassis;
	c.body_mass = 1500.0f;
	c.gravity_modifier = 1.85f;
	c.center_of_mass = { 0.0f, 0.6f, 0.2f };
	c.inertia_modifier = { 1.49f, 1.51f, 1.5f };
	c.air_density = 1.225f;
	c.air_drag_area = 9.0f;
	c.coefficient_of_air_friction = 0.5f;
	c.anti_roll_front = 1.2f;
	c.anti_roll_rear = 0.5f;
	c.yaw = { 0.0f, 0.23f, 0.85f, 0.1f };
	c.throttle = { 0.0f, 0.0f, 0.0f, 0.2f };
	c.brake = { 0.0f, 0.0f, 0.0f, 0.2f };
	c.use_gearbox = true;
	c.use_stand_still_brake = true;
	c.use_stand_still_sleep = true;
	c.stand_still_low_speed_time_limit = 2.0f;
	c.allow_negative_throttle = true;
	gp::GearboxDef &g = d.gearbox;
	g.present = true;
	g.forward_ratios = { 3.0f, 1.8f, 1.7f, 1.65f, 1.4f };
	g.forward_speeds = { 250.0f, 350.0f, 600.0f, 700.0f, 900.0f };
	g.reverse_ratios = { -3.0f, -1.2f };
	g.reverse_speeds = { 100.0f, 150.0f };
	g.clutch_speed_factor = 10.0f;
	g.transmission_efficiency = 1.0f;
	for (int axle = 0; axle < 2; ++axle) {
		gp::WheelConfigDef w;
		w.radius = 0.46f;
		w.width = 0.3f;
		w.mass = axle == 0 ? 12.0f : 15.0f;
		w.inertia = { 2.5f, 2.5f, 2.5f };
		w.spring_length = 0.4f;
		w.spring_stiffness = 0.5f;
		w.spring_damping = 0.06f;
		w.brake_force = 4000.0f;
		w.diff_gear_ratio = 3.8f;
		w.driving_type = 1;
		w.steering_type = axle == 0 ? 1 : 0;
		if (axle == 0) {
			w.steering_sensitivity = { { 0, 0.25f }, { 15, 0.22f }, { 25, 0.18f }, { 30, 0.15f }, { 66, 0.12f }, { 150, 0.12f } };
		}
		d.wheel_configs.push_back(w);
	}
	const float xs[2] = { 0.98269f, -0.98207f };
	const float zs[2] = { 1.94698f, -1.75039f };
	for (int axle = 0; axle < 2; ++axle) {
		for (int side = 0; side < 2; ++side) {
			gp::VehicleWheelDef w;
			w.physics_type = "wptNormal";
			w.transform.trans = { xs[side], 0.48876f, zs[axle] };
			w.config = uint32_t(axle);
			d.wheels.push_back(w);
		}
	}
	gp::EngineDef e;
	e.config_type = "CombustionEngineConfigData";
	e.rpm_points = { 500, 1000, 2000, 2800, 3500, 4000, 4500 };
	e.torque_points = { 250, 450, 400, 285, 185, 85, 30 };
	e.rpm_min = 500;
	e.rpm_max = 5000;
	d.engines.push_back(e);
	gp::DriveRigidBodyDef body;
	body.body_type = "RBTypeCollision";
	body.mass = 1500.0f;
	body.friction = 0.5f;
	body.restitution = 0.4f;
	d.rigid_bodies.push_back(body);
	d.physics_asset = "fixture";
	return d;
}

std::vector<std::vector<Vec3>> humvee_pieces() {
	std::vector<std::vector<Vec3>> out;
	const std::array<std::array<Vec3, 2>, 4> boxes{ { { Vec3{ -1.211f, 0.811f, -2.778f }, Vec3{ 1.210f, 1.222f, 2.635f } },
			{ Vec3{ -1.216f, 0.417f, -1.036f }, Vec3{ 1.216f, 1.881f, 0.958f } }, { Vec3{ -1.214f, 0.735f, -2.750f }, Vec3{ 1.202f, 1.830f, -0.917f } },
			{ Vec3{ -0.801f, 0.461f, -1.948f }, Vec3{ 0.788f, 0.924f, 2.342f } } } };
	for (const auto &b : boxes) {
		std::vector<Vec3> p;
		for (int i = 0; i < 8; ++i) {
			p.push_back({ (i & 1) ? b[1][0] : b[0][0], (i & 2) ? b[1][1] : b[0][1], (i & 4) ? b[1][2] : b[0][2] });
		}
		out.push_back(p);
	}

	std::vector<Vec3> ring;
	const float cx = -0.021f, cz = 0.7025f, r = 0.46f;
	for (int k = 0; k < 12; ++k) {
		const float a = float(k) * 6.2831853f / 12.0f;
		for (const float y : { 1.945f, 2.530f }) {
			ring.push_back({ cx + r * std::cos(a), y, cz + r * std::sin(a) });
		}
	}
	out.push_back(ring);
	return out;
}

std::shared_ptr<const VehicleModel> fixture_model(const VehicleTuning &p_tuning = {}) {
	static const std::vector<std::vector<Vec3>> pieces = humvee_pieces();
	std::vector<ChassisPieceSource> sources;
	for (size_t i = 0; i < pieces.size(); ++i) {
		sources.push_back({ std::span<const Vec3>(pieces[i]), i < 4 ? 0.03f : 0.015f });
	}
	core::Result<std::shared_ptr<const VehicleModel>> m = build_vehicle_model(humvee_def(), sources, p_tuning);
	REQUIRE_MESSAGE(m.ok(), (m.ok() ? std::string() : m.error().message));
	return *m;
}

std::shared_ptr<const StaticWorldShapes> ground_shapes(bool p_features = true) {
	static std::shared_ptr<const StaticWorldShapes> with, without;
	std::shared_ptr<const StaticWorldShapes> &slot = p_features ? with : without;
	if (slot == nullptr) {
		SyntheticVehicleGround g;
		g.features = p_features;
		core::Result<std::shared_ptr<const StaticWorldShapes>> s = build_synthetic_vehicle_ground(g);
		REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
		slot = *s;
	}
	return slot;
}

std::unique_ptr<VehicleWorld> world_with(std::shared_ptr<const StaticWorldShapes> p_shapes, std::shared_ptr<const VehicleModel> p_model,
		std::span<const VehiclePlacement> p_at) {
	VehicleWorldOptions o;
	o.max_vehicles = uint32_t(std::max<size_t>(p_at.size(), 1));
	o.tuning = p_model->tuning;
	core::Result<std::unique_ptr<VehicleWorld>> w = VehicleWorld::create(std::move(p_shapes), o);
	REQUIRE_MESSAGE(w.ok(), (w.ok() ? std::string() : w.error().message));
	for (size_t i = 0; i < p_at.size(); ++i) {
		const core::Status s = (*w)->add_vehicle(VehicleSlot(i), p_model, p_at[i]);
		REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
	}
	return std::move(*w);
}

std::unique_ptr<VehicleWorld> world_at(std::shared_ptr<const StaticWorldShapes> p_shapes, std::shared_ptr<const VehicleModel> p_model,
		const VehiclePlacement &p_at) {
	return world_with(std::move(p_shapes), std::move(p_model), std::span<const VehiclePlacement>(&p_at, 1));
}

void step1(VehicleWorld &r_world, const VehicleCommand &p_command) {
	const VehicleSlot slot = 0;
	const core::Status s = r_world.step(std::span<const VehicleSlot>(&slot, 1), std::span<const VehicleCommand>(&p_command, 1));
	REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
}

void settle(VehicleWorld &r_world) {
	for (uint32_t t = 0; t < kSettleTicks; ++t) {
		step1(r_world, kSettleCommand);
	}
}

VehicleCommand command(int p_throttle, int p_brake, int p_steer, uint8_t p_buttons = 0) {
	VehicleCommand c;
	c.throttle = int8_t(p_throttle);
	c.brake = uint8_t(p_brake);
	c.steer = int8_t(p_steer);
	c.buttons = p_buttons;
	return c;
}

VehiclePlacement placed(float p_x, float p_y, float p_z) {
	VehiclePlacement p;
	p.position = { p_x, p_y + 0.05f, p_z };
	return p;
}

std::vector<uint8_t> body_bytes(const VehicleWorld &p_world, VehicleSlot p_slot) {
	std::vector<uint8_t> b;
	p_world.impl().save_body_stream(p_slot, b);
	return b;
}

std::vector<uint8_t> constraint_bytes(const VehicleWorld &p_world, VehicleSlot p_slot) {
	std::vector<uint8_t> b;
	p_world.impl().save_constraint_stream(p_slot, b);
	return b;
}

void print_handling(const char *p_what, const VehicleHandling &p_h) {
	std::printf("H %s (unverified placeholders for S5 Session 2): 0-60 km/h %.2f s, speed at 6 s %.1f km/h (IW4L target 68), braking from 60 km/h "
				"%.2f m at %.2f m/s^2 (IW4L ~10), turning circle %.2f m kerb to kerb (%.2f m CoM), top speed %.1f km/h\n",
			p_what, double(p_h.time_0_60_kmh), double(p_h.speed_at_6s_kmh), double(p_h.brake_distance_60_m), double(p_h.brake_deceleration),
			double(p_h.turning_circle_m), double(p_h.turning_circle_com_m), double(p_h.top_speed_kmh));
}

constexpr unsigned kCourseLanes = 4;

void check_pod_bases(const VehicleCourseReport &p_report) {
	for (const VehicleCorrectionReport &x : p_report.corrections) {
		if (x.mode == VehicleRestoreMode::ExactPod || x.mode == VehicleRestoreMode::PodOwnLambdas) {
			CHECK_MESSAGE(x.diverged_bases > 0, vehicle_course_name(p_report.course.kind) << " " << vehicle_restore_mode_name(x.mode) << " R=" << x.replay_ticks);
		} else if (x.mode == VehicleRestoreMode::Full) {
			CHECK(x.diverged_bases == 0);
		}
	}
}

}

TEST_SUITE("sim-vehicle-unit") {
	TEST_CASE("sim.vehicle: the shipped vehicle_tuning.cfg parses to the defaults; format / parse round trip; hostile text fails with the line") {
#ifdef OPENBF3_VEHICLE_TUNING_CFG
		const std::string text = read_text(OPENBF3_VEHICLE_TUNING_CFG);
		REQUIRE(!text.empty());
		core::Result<VehicleTuning> shipped = parse_vehicle_tuning(text);
		REQUIRE_MESSAGE(shipped.ok(), (shipped.ok() ? std::string() : shipped.error().message));
		CHECK(*shipped == VehicleTuning{});
#endif
		VehicleTuning t;
		t.gravity = 9.80665f;
		t.tester = WheelCollisionTester::Ray;
		t.steer_model = SteerAngleModel::SensitivityRadians;
		t.velocity_steps = 17;
		core::Result<VehicleTuning> back = parse_vehicle_tuning(format_vehicle_tuning(t));
		REQUIRE(back.ok());
		CHECK(*back == t);
		CHECK(vehicle_tuning_hash(t) != vehicle_tuning_hash(VehicleTuning{}));
		const char *bad[] = { "nonsense = 1", "gravity = 1\ngravity = 2", "gravity 9.81", "gravity = nan", "gravity = 1e40", "gravity = -1",
			"tester = laser", "steer_model = auto", "collision_steps = 0", "collision_steps = 1.5", "velocity_steps = 99999999999" };
		for (const char *b : bad) {
			const core::Result<VehicleTuning> r = parse_vehicle_tuning(b);
			CHECK_MESSAGE(!r.ok(), b);
			if (!r.ok()) {
				CHECK(r.error().message.find("line") != std::string::npos);
			}
		}
		CHECK(!parse_vehicle_tuning(std::string(300, 'x')).ok());
		std::string many;
		for (int i = 0; i < 600; ++i) {
			many += "# comment\n";
		}
		CHECK(!parse_vehicle_tuning(many).ok());
		CHECK(parse_vehicle_tuning("# only a comment\n\n").ok());
	}

	TEST_CASE("sim.vehicle: ghost codec: 440 bits for the Humvee, round trip within half a step, hostile codes refused with the state untouched") {
		CHECK(vehicle_ghost_bits(kDefaultVehicleGhost, 4) == 440);
		VehicleGhostQuantisation no_suspension = kDefaultVehicleGhost;
		no_suspension.send_suspension = false;
		CHECK(vehicle_ghost_bits(no_suspension, 4) == 376);
		const std::shared_ptr<const VehicleModel> model = fixture_model();
		const VehicleGhostScale scale = vehicle_ghost_scale(*model);
		CHECK(scale.wheel_count == 4);
		CHECK(scale.min_gear == -2);
		CHECK(scale.max_gear == 5);
		std::mt19937 rng(7);
		std::uniform_real_distribution<float> u(-1.0f, 1.0f);
		for (int i = 0; i < 500; ++i) {
			VehicleState s;
			s.wheel_count = 4;
			s.body.com_position = { 765.0f * u(rng), 140.0f + 50.0f * u(rng), 765.0f * u(rng) };
			std::array<float, 4> q{ u(rng), u(rng), u(rng), u(rng) };
			const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
			for (float &c : q) {
				c /= n;
			}
			s.body.rotation = q;
			s.body.linear_velocity = { 40.0f * u(rng), 10.0f * u(rng), 40.0f * u(rng) };
			s.body.angular_velocity = { 3.0f * u(rng), 3.0f * u(rng), 3.0f * u(rng) };
			for (size_t w = 0; w < 4; ++w) {
				s.wheels[w].suspension_length = 0.2f + 0.19f * u(rng);
				s.wheels[w].angular_velocity = 90.0f * u(rng);
				s.wheels[w].angle = 3.14f + 3.0f * u(rng);
			}
			s.controller.engine_rpm = 2500.0f + 2000.0f * u(rng);
			s.controller.gear = int32_t(std::lround(2.0f + 3.0f * u(rng)));
			s.controller.clutch = 0.5f + 0.5f * u(rng);
			s.driver.steer_filter = u(rng);
			s.driver.throttle_filter = u(rng);
			s.driver.brake_filter = 0.5f + 0.5f * u(rng);
			s.driver.stand_still_ticks = uint16_t(i % 121);
			VehicleState back;
			back.wheel_count = 4;
			const VehicleGhostCodes codes = encode_vehicle_ghost(s, kDefaultVehicleGhost, scale);
			const core::Status ok = decode_vehicle_ghost(codes, kDefaultVehicleGhost, scale, back);
			REQUIRE_MESSAGE(ok.ok(), (ok.ok() ? std::string() : ok.error().message));
			for (size_t a = 0; a < 3; ++a) {
				CHECK(std::fabs(back.body.com_position[a] - s.body.com_position[a]) <= (a == 1 ? 1.0f / 256.0f : 1.0f / 512.0f) + 1e-4f);
				CHECK(std::fabs(back.body.linear_velocity[a] - s.body.linear_velocity[a]) <= 1.0f / 128.0f + 1e-5f);
				CHECK(std::fabs(back.body.angular_velocity[a] - s.body.angular_velocity[a]) <= 1.0f / 512.0f + 1e-5f);
			}
			const float d = std::fabs(q[0] * back.body.rotation[0] + q[1] * back.body.rotation[1] + q[2] * back.body.rotation[2] + q[3] * back.body.rotation[3]);
			CHECK(d > 0.99999f);
			CHECK(back.controller.gear == s.controller.gear);
			CHECK(back.driver.stand_still_ticks == s.driver.stand_still_ticks);
			CHECK(std::fabs(back.wheels[2].suspension_length - s.wheels[2].suspension_length) <= 0.4f / 65535.0f);
		}

		VehicleState good;
		good.wheel_count = 4;
		good.controller.gear = 1;
		const VehicleGhostCodes base = encode_vehicle_ghost(good, kDefaultVehicleGhost, scale);
		std::vector<std::pair<const char *, VehicleGhostCodes>> hostile;
		VehicleGhostCodes h = base;
		h.position[0] = 1u << 21;
		hostile.push_back({ "position width", h });
		h = base;
		h.orientation_largest = 4;
		hostile.push_back({ "largest index", h });
		h = base;
		h.orientation_largest = 3;
		h.orientation[0] = h.orientation[1] = h.orientation[2] = 2047;
		hostile.push_back({ "smallest-three radicand", h });
		h = base;
		h.active = 2;
		hostile.push_back({ "active = 2", h });
		h = base;
		h.reversing = 2;
		hostile.push_back({ "reversing = 2", h });
		h = base;
		h.wheel_count = 3;
		hostile.push_back({ "wheel count", h });
		h = base;
		h.wheel_count = 9;
		hostile.push_back({ "wheel count above 8", h });
		h = base;
		h.gear = 8 + 6;
		hostile.push_back({ "gear above the model's", h });
		h = base;
		h.gear = 8 - 3;
		hostile.push_back({ "gear below the model's", h });
		h = base;
		h.wheel_spin[1] = 1u << 15;
		hostile.push_back({ "wheel spin width", h });
		h = base;
		h.stand_still = 1u << 10;
		hostile.push_back({ "stand-still width", h });
		for (const auto &[what, codes] : hostile) {
			VehicleState target;
			target.wheel_count = 4;
			target.body.com_position = { 1.0f, 2.0f, 3.0f };
			const VehicleState before = target;
			const core::Status s = decode_vehicle_ghost(codes, kDefaultVehicleGhost, scale, target);
			CHECK_MESSAGE(!s.ok(), what);
			CHECK_MESSAGE(target == before, what);
		}
	}

	TEST_CASE("sim.vehicle: smallest-three: round trip, renormalised, a radicand below -1e-3 refused") {
		std::mt19937 rng(3);
		std::normal_distribution<float> n(0.0f, 1.0f);
		for (int i = 0; i < 2000; ++i) {
			std::array<float, 4> q{ n(rng), n(rng), n(rng), n(rng) };
			const float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
			for (float &c : q) {
				c /= l;
			}
			uint8_t largest = 0;
			uint16_t codes[3];
			encode_quat_smallest_three(q, 11, largest, codes);
			core::Result<std::array<float, 4>> back = decode_quat_smallest_three(largest, codes, 11);
			REQUIRE(back.ok());
			const std::array<float, 4> &b = *back;
			CHECK(std::fabs(b[0] * b[0] + b[1] * b[1] + b[2] * b[2] + b[3] * b[3] - 1.0f) < 1e-5f);
			CHECK(std::fabs(q[0] * b[0] + q[1] * b[1] + q[2] * b[2] + q[3] * b[3]) > 0.999998f);
		}
		const uint16_t big[3] = { 2047, 2047, 0 };
		CHECK(!decode_quat_smallest_three(0, big, 11).ok());
		const uint16_t over[3] = { 4096, 0, 0 };
		CHECK(!decode_quat_smallest_three(0, over, 11).ok());
		const uint16_t mid[3] = { 1024, 1024, 1024 };
		CHECK(!decode_quat_smallest_three(5, mid, 11).ok());
		CHECK(!decode_quat_smallest_three(0, mid, 17).ok());
	}

	TEST_CASE("sim.vehicle: vehicle_state_hash covers the restorable fields only; first difference names the field") {
		VehicleState a;
		a.wheel_count = 4;
		a.wheels[1].lambda_lateral = 2.0f;
		const uint64_t h = vehicle_state_hash(a);
		VehicleState b = a;
		b.body.origin = { 9.0f, 9.0f, 9.0f };
		b.wheels[0].steer_angle = 0.3f;
		b.wheels[0].in_contact = 1;
		b.tick = 77;
		b.wheels[5].angle = 1.0f;
		CHECK(vehicle_state_hash(b) == h);
		CHECK(vehicle_state_first_difference(a, b).empty());
		b = a;
		b.wheels[2].lambda_lateral = -0.0f;
		CHECK(vehicle_state_hash(b) != h);
		CHECK(vehicle_state_first_difference(a, b) == "wheels[2].lambda_lateral");
		b = a;
		b.body.sleep_test_timer = 0.25f;
		CHECK(vehicle_state_first_difference(a, b) == "body.sleep_test_timer");
		b = a;
		b.controller.gear = 2;
		CHECK(vehicle_state_first_difference(a, b) == "controller.gear");
		b = a;
		b.driver.reversing = 1;
		CHECK(vehicle_state_first_difference(a, b) == "driver.reversing");
		b = a;
		b.current_step = 5;
		CHECK(vehicle_state_first_difference(a, b) == "current_step");
		b = a;
		b.wheel_count = 3;
		CHECK(vehicle_state_first_difference(a, b) == "wheel_count");
	}

	TEST_CASE("sim.vehicle: command log round trip; hostile logs refused with the line") {
		VehicleCommandLog log;
		for (Tick t = 1; t <= 300; ++t) {
			VehicleCommand c = command(int(t % 255) - 127, int(t % 256), 127 - int(t % 255), uint8_t(t % 8));
			c.tick = t;
			log.commands.push_back(c);
		}
		core::Result<VehicleCommandLog> back = parse_vehicle_command_log(format_vehicle_command_log(log));
		REQUIRE(back.ok());
		REQUIRE(back->commands.size() == log.commands.size());
		CHECK(back->commands == log.commands);
		const char *bad[] = { "", "OBF3VLOG 2\n1 0 0 0 0\n", "OBF3VLOG 1\n1 128 0 0 0\n", "OBF3VLOG 1\n1 0 256 0 0\n", "OBF3VLOG 1\n1 0 0 -128 0\n",
			"OBF3VLOG 1\n1 0 0 0 8\n", "OBF3VLOG 1\n1 0 0 0 0\n3 0 0 0 0\n", "OBF3VLOG 1\n1 0 0 0\n", "OBF3VLOG 1\n1 0 0 0 0 0\n", "OBF3VLOG 1\nx 0 0 0 0\n" };
		for (const char *b : bad) {
			CHECK_MESSAGE(!parse_vehicle_command_log(b).ok(), b);
		}
		CHECK(!parse_vehicle_command_log("OBF3VLOG 1\n" + std::string(200, '1') + "\n").ok());
		CHECK(!parse_vehicle_command_log(format_vehicle_command_log(log), 10).ok());
		CHECK(parse_vehicle_command_log("OBF3VLOG 1\n# a comment\n5 1 2 3 1\n").ok());
	}

	TEST_CASE("sim.vehicle: build_vehicle_model on the Humvee fixture gives the s2.md 5.2 / 5.3 numbers; out-of-bounds input fails") {
		const std::shared_ptr<const VehicleModel> m = fixture_model();
		CHECK(m->mass == 1500.0f);
		CHECK(m->gravity_factor == 1.85f);
		CHECK(m->wheel_count == 4);
		CHECK(m->chassis.size() == 5);
		const VehicleWheelModel &fl = m->wheels[0];
		CHECK(fl.attachment[1] == doctest::Approx(0.48876f + 0.225f).epsilon(1e-6));
		CHECK(fl.suspension_max == 0.4f);
		CHECK(fl.suspension_min == doctest::Approx(0.04f));
		CHECK(fl.spring_stiffness == doctest::Approx(38890.0f).epsilon(0.001));
		CHECK(fl.spring_damping == doctest::Approx(3437.0f).epsilon(0.001));
		CHECK(fl.max_brake_torque == doctest::Approx(1840.0f));
		CHECK(fl.max_hand_brake_torque == 0.0f);
		CHECK(m->wheels[2].max_hand_brake_torque == 4000.0f);
		CHECK(fl.max_steer_angle == doctest::Approx(32.0f * 3.14159265f / 180.0f));
		CHECK(m->wheels[2].max_steer_angle == 0.0f);
		CHECK(fl.side == 0);
		CHECK(m->wheels[1].side == 1);
		CHECK(fl.axle == 0);
		CHECK(m->wheels[3].axle == 1);
		CHECK(m->differentials == 2);
		CHECK(m->differential_ratio[0] == doctest::Approx(3.8f));
		CHECK(m->differential_torque_ratio[1] == 0.5f);
		CHECK(m->anti_roll_bars == 2);
		CHECK(m->anti_roll_stiffness[0] == doctest::Approx(12000.0f));
		CHECK(m->anti_roll_stiffness[1] == doctest::Approx(5000.0f));
		CHECK(m->engine_max_torque == 450.0f);
		CHECK(m->torque_points == 7);
		CHECK(m->forward_gears == 5);
		CHECK(m->reverse_gears == 2);
		CHECK(m->drag_coefficient == doctest::Approx(2.756f).epsilon(0.001));
		CHECK(m->stand_still_ticks == 120);
		CHECK(vehicle_max_steer_angle(*m, 0.0f) == doctest::Approx(32.0f * 3.14159265f / 180.0f));
		CHECK(vehicle_max_steer_angle(*m, 40.0f / 3.6f) < vehicle_max_steer_angle(*m, 10.0f / 3.6f));

		CHECK(fixture_model()->model_digest == m->model_digest);
		VehicleTuning t;
		t.lateral_grip = 1.1f;
		CHECK(fixture_model(t)->model_digest != m->model_digest);

		const std::vector<std::vector<Vec3>> pieces = humvee_pieces();
		std::vector<ChassisPieceSource> sources;
		for (const auto &p : pieces) {
			sources.push_back({ std::span<const Vec3>(p), 0.03f });
		}
		{
			gp::VehicleDriveDef d = humvee_def();
			d.wheels.clear();
			CHECK(!build_vehicle_model(d, sources, {}).ok());
		}
		{
			gp::VehicleDriveDef d = humvee_def();
			while (d.wheels.size() < 9) {
				d.wheels.push_back(d.wheels[0]);
			}
			CHECK(!build_vehicle_model(d, sources, {}).ok());
		}
		{
			gp::VehicleDriveDef d = humvee_def();
			d.chassis.body_mass = std::nanf("");
			d.rigid_bodies.clear();
			CHECK(!build_vehicle_model(d, sources, {}).ok());
		}
		{
			gp::VehicleDriveDef d = humvee_def();
			d.wheel_configs[0].radius = 0.0f;
			CHECK(!build_vehicle_model(d, sources, {}).ok());
		}
		{
			gp::VehicleDriveDef d = humvee_def();
			d.engines.clear();
			CHECK(!build_vehicle_model(d, sources, {}).ok());
		}
		{
			std::vector<ChassisPieceSource> many(kMaxChassisPieces + 1, sources[0]);
			CHECK(!build_vehicle_model(humvee_def(), many, {}).ok());
			std::vector<Vec3> bad = pieces[0];
			bad[3][1] = std::nanf("");
			std::vector<ChassisPieceSource> nan_piece{ { std::span<const Vec3>(bad), 0.03f } };
			CHECK(!build_vehicle_model(humvee_def(), nan_piece, {}).ok());
			CHECK(!build_vehicle_model(humvee_def(), {}, {}).ok());
		}
	}

	TEST_CASE("sim.vehicle: static BodyIDs equal CollisionWorld's and between worlds; the Jolt streams are the s2.md 6.4 sizes") {
		const std::shared_ptr<const StaticWorldShapes> shapes = ground_shapes();
		core::Result<std::unique_ptr<CollisionWorld>> cw = CollisionWorld::create(shapes);
		REQUIRE(cw.ok());
		const std::shared_ptr<const VehicleModel> m = fixture_model();
		const VehiclePlacement at[2] = { placed(0, 0, 0), placed(10, 0, 0) };
		std::unique_ptr<VehicleWorld> a = world_with(shapes, m, at);
		std::unique_ptr<VehicleWorld> b = world_with(shapes, m, at);
		const std::vector<JPH::BodyID> &reference = (*cw)->impl().bodies;
		REQUIRE(a->static_body_count() == reference.size());
		REQUIRE(b->static_body_count() == reference.size());
		size_t differ = 0;
		for (size_t i = 0; i < reference.size(); ++i) {
			differ += a->static_body_id(i) == reference[i].GetIndexAndSequenceNumber() && b->static_body_id(i) == reference[i].GetIndexAndSequenceNumber() ? 0 : 1;
		}
		CHECK(differ == 0);
		CHECK(a->vehicle_body_id(1) == uint32_t(reference.size() + 1));
		CHECK(a->state_hash() == b->state_hash());
		CHECK(body_bytes(*a, 0).size() == kJoltBodyStreamBytes);
		CHECK(constraint_bytes(*a, 0).size() == jolt_vehicle_constraint_stream_bytes(4));
		std::printf("static bodies %zu (= CollisionWorld's), vehicle bodies %u and %u, body stream %zu B, constraint stream %zu B\n", reference.size(),
				a->vehicle_body_id(0), a->vehicle_body_id(1), body_bytes(*a, 0).size(), constraint_bytes(*a, 0).size());
	}

	TEST_CASE("sim.vehicle: restore_vehicle_state of a world's own state (kAll, kGhost) is a bit-exact no-op: awake, mid-air and asleep") {
		const std::shared_ptr<const StaticWorldShapes> shapes = ground_shapes();
		const std::shared_ptr<const VehicleModel> m = fixture_model();
		const std::array<VehicleCourse, kVehicleCourseKindCount> courses = synthetic_vehicle_courses();

		VehicleCourse bumps = courses[size_t(VehicleCourseKind::Bumps)];
		bumps.kind = VehicleCourseKind::Straight;
		std::unique_ptr<VehicleWorld> w = world_at(shapes, m, bumps.start);
		settle(*w);
		VehicleScriptMemory memory;
		std::vector<std::pair<const char *, VehicleWorldSnapshot>> cases;
		bool airborne = false;
		for (Tick t = 1; t <= 2400 && !airborne; ++t) {
			step1(*w, drive_vehicle_course(bumps, *m, w->state(0), t, memory));
			if (t == 200) {
				cases.push_back({ "awake", {} });
				w->save(cases.back().second);
			}
			if (t > 200 && w->contacts(0).wheels_in_contact == 0) {
				airborne = true;
				cases.push_back({ "mid-air", {} });
				w->save(cases.back().second);
			}
		}
		CHECK(airborne);
		{
			std::unique_ptr<VehicleWorld> s = world_at(shapes, m, courses[0].start);
			settle(*s);
			for (int i = 0; i < 60; ++i) {
				step1(*s, kSettleCommand);
			}
			s->impl().system->GetBodyInterface().DeactivateBody(s->impl().vehicles[0].body);
			CHECK(s->state(0).body.active == 0);
			cases.push_back({ "asleep", {} });
			s->save(cases.back().second);
		}
		for (const auto &[name, snapshot] : cases) {
			for (const uint32_t groups : { vehicle_restore::kAll, vehicle_restore::kGhost }) {
				std::unique_ptr<VehicleWorld> x = world_at(shapes, m, bumps.start);
				std::unique_ptr<VehicleWorld> twin = world_at(shapes, m, bumps.start);
				REQUIRE(x->restore(snapshot).ok());
				REQUIRE(twin->restore(snapshot).ok());
				const uint64_t before = x->state_hash();
				const core::Status s = x->restore_vehicle_state(0, x->state(0), groups, ContactCachePolicy::Keep);
				REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
				CHECK_MESSAGE(x->state_hash() == before, name);
				size_t mismatches = 0;
				VehicleScriptMemory mx;
				for (Tick t = 1; t <= 600; ++t) {
					const VehicleCommand c = std::string(name) == "asleep" ? (t < 300 ? VehicleCommand{} : command(127, 0, 0)) : drive_vehicle_course(bumps, *m, x->state(0), t, mx);
					step1(*x, c);
					step1(*twin, c);
					mismatches += x->state_hash() == twin->state_hash() ? 0 : 1;
				}
				CHECK_MESSAGE(mismatches == 0, name << " groups " << groups);
			}
		}
	}

	TEST_CASE("sim.vehicle: kAll from another world's state(slot) makes the vehicle's Jolt bytes equal (only the contact cache may differ)") {
		const std::shared_ptr<const StaticWorldShapes> shapes = ground_shapes();
		const std::shared_ptr<const VehicleModel> m = fixture_model();
		const VehicleCourse straight = synthetic_vehicle_courses()[0];
		std::unique_ptr<VehicleWorld> a = world_at(shapes, m, straight.start);
		settle(*a);
		VehicleWorldSnapshot early;
		a->save(early);
		VehicleScriptMemory memory;
		for (Tick t = 1; t <= 300; ++t) {
			step1(*a, drive_vehicle_course(straight, *m, a->state(0), t, memory));
		}
		std::unique_ptr<VehicleWorld> b = world_at(shapes, m, straight.start);
		REQUIRE(b->restore(early).ok());
		const core::Status s = b->restore_vehicle_state(0, a->state(0), vehicle_restore::kAll, ContactCachePolicy::Keep);
		REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
		CHECK(vehicle_state_hash(b->state(0)) == vehicle_state_hash(a->state(0)));
		CHECK(body_bytes(*b, 0) == body_bytes(*a, 0));
		CHECK(constraint_bytes(*b, 0) == constraint_bytes(*a, 0));
	}

	TEST_CASE("sim.vehicle: the wake rule: a sleeping vehicle wakes on any non-zero command and stays asleep under the all-zero command") {
		const std::shared_ptr<const StaticWorldShapes> shapes = ground_shapes();
		const std::shared_ptr<const VehicleModel> m = fixture_model();
		const VehicleCommand wakers[] = { command(127, 0, 0), command(0, 255, 0), command(0, 0, 60), command(0, 0, 0, vehicle_buttons::kHandbrake) };
		for (size_t i = 0; i < 4; ++i) {
			std::unique_ptr<VehicleWorld> w = world_at(shapes, m, placed(0, 0, 0));
			settle(*w);
			w->impl().system->GetBodyInterface().DeactivateBody(w->impl().vehicles[0].body);
			REQUIRE(w->state(0).body.active == 0);
			for (int t = 0; t < 30; ++t) {
				step1(*w, VehicleCommand{});
			}
			CHECK_MESSAGE(w->state(0).body.active == 0, "the all-zero command keeps it asleep");
			step1(*w, wakers[i]);
			CHECK_MESSAGE(w->state(0).body.active == 1, "waker " << i);
		}

		std::unique_ptr<VehicleWorld> w = world_at(shapes, m, placed(0, 0, 0));
		settle(*w);
		w->impl().system->GetBodyInterface().DeactivateBody(w->impl().vehicles[0].body);
		const Vec3 start = w->state(0).body.com_position;
		for (int t = 0; t < 180; ++t) {
			step1(*w, command(127, 0, 0));
		}
		const Vec3 end = w->state(0).body.com_position;
		CHECK(end[2] - start[2] > 5.0f);
	}

	TEST_CASE("sim.vehicle: steering sign: a positive steer at speed turns the heading towards -X (negative yaw rate about +Y)") {
		const std::shared_ptr<const StaticWorldShapes> shapes = ground_shapes(false);
		const std::shared_ptr<const VehicleModel> m = fixture_model();
		std::unique_ptr<VehicleWorld> w = world_at(shapes, m, placed(0, 0, -300));
		settle(*w);
		for (int t = 0; t < 180; ++t) {
			step1(*w, command(127, 0, 0));
		}
		float yaw_rate = 0.0f;
		for (int t = 0; t < 60; ++t) {
			step1(*w, command(60, 0, 127));
			yaw_rate = w->state(0).body.angular_velocity[1];
		}
		const VehicleState s = w->state(0);
		std::printf("steering sign: steer +127 at %.1f km/h -> yaw rate %.3f rad/s, x %.2f m\n", double(std::sqrt(s.body.linear_velocity[0] * s.body.linear_velocity[0] +
				s.body.linear_velocity[2] * s.body.linear_velocity[2]) * 3.6f), double(yaw_rate), double(s.body.origin[0]));
		CHECK(yaw_rate < -0.05f);
		CHECK(s.body.origin[0] < -0.5f);
	}

	TEST_CASE("sim.vehicle: hostile VehicleStates are refused before any write; restore() refuses a foreign or broken snapshot") {
		const std::shared_ptr<const StaticWorldShapes> shapes = ground_shapes();
		const std::shared_ptr<const VehicleModel> m = fixture_model();
		std::unique_ptr<VehicleWorld> w = world_at(shapes, m, placed(0, 0, 0));
		settle(*w);
		const uint64_t before = w->state_hash();
		const VehicleState good = w->state(0);
		std::vector<std::pair<const char *, std::pair<VehicleState, uint32_t>>> bad;
		VehicleState s = good;
		s.body.com_position[1] = std::nanf("");
		bad.push_back({ "NaN position", { s, vehicle_restore::kBody } });
		s = good;
		s.body.rotation = { 0.0f, 0.0f, 0.0f, 0.9f };
		bad.push_back({ "non-unit rotation", { s, vehicle_restore::kGhost } });
		s = good;
		s.body.linear_velocity[0] = std::numeric_limits<float>::infinity();
		bad.push_back({ "infinite velocity", { s, vehicle_restore::kAll } });
		s = good;
		s.controller.gear = 6;
		bad.push_back({ "gear 6", { s, vehicle_restore::kController } });
		s = good;
		s.wheels[1].contact_body = 123456;
		bad.push_back({ "foreign contact body", { s, vehicle_restore::kWheelContacts } });
		s = good;
		s.wheels[3].lambda_lateral = std::nanf("");
		bad.push_back({ "NaN lambda", { s, vehicle_restore::kLambdas } });
		s = good;
		s.wheel_count = 3;
		bad.push_back({ "wheel count", { s, vehicle_restore::kAll } });
		s = good;
		s.driver.steer_filter = std::nanf("");
		bad.push_back({ "NaN driver", { s, vehicle_restore::kDriver } });
		for (const auto &[what, state_groups] : bad) {
			const core::Status r = w->restore_vehicle_state(0, state_groups.first, state_groups.second, ContactCachePolicy::Keep);
			CHECK_MESSAGE(!r.ok(), what);
			CHECK_MESSAGE(w->state_hash() == before, what);
		}
		CHECK(!w->restore_vehicle_state(1, good, vehicle_restore::kAll, ContactCachePolicy::Keep).ok());

		s = good;
		s.wheels[0].lambda_lateral = std::nanf("");
		CHECK(w->restore_vehicle_state(0, s, vehicle_restore::kGhost, ContactCachePolicy::Keep).ok());
		CHECK(w->state_hash() == before);

		const VehiclePlacement two[2] = { placed(0, 0, 0), placed(10, 0, 0) };
		std::unique_ptr<VehicleWorld> other = world_with(shapes, m, two);
		VehicleWorldSnapshot foreign;
		other->save(foreign);
		CHECK(!w->restore(foreign).ok());
		CHECK(w->state_hash() == before);
		VehicleWorldSnapshot own;
		w->save(own);
		VehicleWorldSnapshot cut = own;
		cut.jolt.resize(cut.jolt.size() - 5);
		CHECK(!w->restore(cut).ok());
		CHECK(w->state_hash() == before);
		VehicleWorldSnapshot padded = own;
		padded.jolt.push_back(0);
		CHECK(!w->restore(padded).ok());
		CHECK(w->state_hash() == before);
		CHECK(w->restore(own).ok());
		CHECK(w->state_hash() == before);
	}

	TEST_CASE("sim.vehicle: D, G1, G1r and G2x on the 7 synthetic courses, G2 reported (Full mode error exactly 0; slope_idle sleep reported)") {
		const std::shared_ptr<const StaticWorldShapes> shapes = ground_shapes();
		const std::shared_ptr<const VehicleModel> m = fixture_model();
		std::vector<VehicleCourseReport> reports;
		VehicleReplayOptions o;
		if (kAsan) {

			o.modes = (1u << uint32_t(VehicleRestoreMode::OwnHistory)) | (1u << uint32_t(VehicleRestoreMode::ExactPod)) |
					(1u << uint32_t(VehicleRestoreMode::Full));
		}
		const auto start = std::chrono::steady_clock::now();

		const auto synthetic = synthetic_vehicle_courses();
		std::vector<core::Result<VehicleCourseReport>> runs = core::parallel_map<core::Result<VehicleCourseReport>>(
				synthetic.size(), [&](size_t i) { return run_vehicle_course(shapes, m, synthetic[i], o); }, kCourseLanes);
		for (size_t i = 0; i < synthetic.size(); ++i) {
			const VehicleCourse &c = synthetic[i];
			core::Result<VehicleCourseReport> &r = runs[i];
			REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
			std::printf("%s", format_vehicle_course_report(*r).c_str());
			for (const VehicleCorrectionReport &x : r->corrections) {
				if (x.mode == VehicleRestoreMode::Full) {
					CHECK_MESSAGE(x.all.max == 0.0f, vehicle_course_name(c.kind));
				}
			}
			check_pod_bases(*r);
			if (c.kind == VehicleCourseKind::SlopeIdle) {
				std::printf("slope_idle (synthetic 12 degrees): asleep %u of %u ticks (report only)\n", r->ticks_asleep, r->ticks);
			}
			reports.push_back(std::move(*r));
		}
		std::printf("synthetic courses: %.1f s (noisy)\n", seconds_since(start));
		std::string why;
		CHECK_MESSAGE(vehicle_gate_courses(reports, &why), why);
		CHECK_MESSAGE(vehicle_gate_drove(reports, &why), why);
		CHECK_MESSAGE(vehicle_gate_g1(reports, &why), why);

		why.clear();
		const bool g2 = vehicle_gate_g2(reports, &why);
		std::printf("G2 (report only, quantised ghost): %s%s%s\n", g2 ? "pass" : "over 2 cm", why.empty() ? "" : ": ", why.c_str());
		why.clear();
		CHECK_MESSAGE(vehicle_gate_g2x(reports, &why), "G2x: " << why);
	}

	TEST_CASE("sim.vehicle: G1m: three Humvees that touch and cross tile seams, bit-identical, G1 and G1r") {
		const std::shared_ptr<const StaticWorldShapes> shapes = ground_shapes();
		const std::shared_ptr<const VehicleModel> m = fixture_model();
		const std::array<VehicleCourse, 3> group = synthetic_vehicle_group();
		core::Result<VehicleGroupReport> r = run_vehicle_group(shapes, m, group);
		REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
		std::printf("G1m %s", format_vehicle_group_report(*r).c_str());
		std::string why;
		CHECK_MESSAGE(vehicle_gate_g1m(*r, &why), why);
	}

	TEST_CASE("sim.vehicle: G1t: three Humvees over a terrain HeightFieldShape, its patch and a Havok ramp tile, bit-identical, G1 and G1r") {

		const float slope = std::tan(4.0f * 3.14159265f / 180.0f);
		const auto ground_y = [&](float x, float z) { return 30.0f + slope * x + 0.15f * std::sin(x / 5.0f) * std::cos(z / 7.0f); };
		std::vector<float> heights(65 * 65);
		std::vector<uint8_t> holes(65 * 65, 0);
		for (size_t z = 0; z < 65; ++z) {
			for (size_t x = 0; x < 65; ++x) {
				heights[x + z * 65] = ground_y(-64.0f + 2.0f * float(x), -64.0f + 2.0f * float(z));
			}
		}
		holes[32 + 40 * 65] = 1;
		StaticTerrainView view;
		view.width = 65;
		view.height = 65;
		view.origin_xz = { -64.0f, -64.0f };
		view.spacing = { 2.0f, 2.0f };
		view.heights = heights;
		view.holes = holes;
		std::vector<StaticTriangle> ramp;
		const float y64 = ground_y(64.0f, 0.0f);
		for (int i = 0; i < 16; ++i) {
			const float x0 = 64.0f + 2.0f * float(i), x1 = x0 + 2.0f;
			const float y0 = y64 + slope * (x0 - 64.0f), y1 = y64 + slope * (x1 - 64.0f);
			StaticTriangle a, b;
			a.v = { Vec3{ x0, y0, -40.0f }, Vec3{ x0, y0, 40.0f }, Vec3{ x1, y1, -40.0f } };
			b.v = { Vec3{ x1, y1, -40.0f }, Vec3{ x0, y0, 40.0f }, Vec3{ x1, y1, 40.0f } };
			ramp.push_back(a);
			ramp.push_back(b);
		}
		core::Result<std::shared_ptr<const StaticWorldShapes>> shapes = build_static_world_terrain(ramp, std::span<const StaticTerrainView>(&view, 1));
		REQUIRE_MESSAGE(shapes.ok(), (shapes.ok() ? std::string() : shapes.error().message));
		REQUIRE((*shapes)->terrain_count() == 1);
		REQUIRE((*shapes)->terrain(0).patch_triangles > 0);
		REQUIRE((*shapes)->tile_count() >= 1);
		const std::shared_ptr<const VehicleModel> m = fixture_model();
		const auto at = [&](float x, float z, bool along_x) {
			VehiclePlacement p;
			p.position = { x, ground_y(x, z) + 0.6f, z };
			if (along_x) {
				p.rotation = { 0.0f, 0.70710678f, 0.0f, 0.70710678f };
			}
			return p;
		};
		std::array<VehicleCourse, 3> group;

		group[0].kind = VehicleCourseKind::SlopeIdle;
		group[0].found = true;
		group[0].a = { 2.5f, ground_y(2.5f, 13.0f), 13.0f };
		group[0].b = group[0].a;
		group[0].start = at(2.5f, 13.0f, true);
		group[0].slope_degrees = 4.0f;

		group[1].kind = VehicleCourseKind::Collision;
		group[1].found = true;
		group[1].a = { 2.5f, ground_y(2.5f, -15.0f), -15.0f };
		group[1].b = { 2.5f, ground_y(2.5f, 11.8f), 11.8f };
		group[1].start = at(2.5f, -15.0f, false);
		group[1].feature = 1.5f;

		group[2].kind = VehicleCourseKind::Slalom;
		group[2].found = true;
		group[2].a = { -50.0f, ground_y(-50.0f, 17.0f), 17.0f };
		group[2].b = { 90.0f, y64 + slope * 26.0f, 17.0f };
		group[2].start = at(-50.0f, 17.0f, true);
		core::Result<VehicleGroupReport> r = run_vehicle_group(*shapes, m, group);
		REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
		std::printf("G1t %s", format_vehicle_group_report(*r).c_str());
		std::string why;
		CHECK_MESSAGE(vehicle_gate_g1m(*r, &why), why);
	}

	TEST_CASE("sim.vehicle: two worlds stepped on two threads at once give the single-thread hashes") {
		const std::shared_ptr<const StaticWorldShapes> shapes = ground_shapes();
		const std::shared_ptr<const VehicleModel> m = fixture_model();
		const VehicleCourse slalom = synthetic_vehicle_courses()[size_t(VehicleCourseKind::Slalom)];
		const auto run = [&](std::vector<uint64_t> &r_hashes) {
			std::unique_ptr<VehicleWorld> w = world_at(shapes, m, slalom.start);
			for (uint32_t t = 0; t < kSettleTicks; ++t) {
				const VehicleSlot slot = 0;
				(void)w->step(std::span<const VehicleSlot>(&slot, 1), std::span<const VehicleCommand>(&kSettleCommand, 1));
			}
			VehicleScriptMemory memory;
			for (Tick t = 1; t <= 1200; ++t) {
				const VehicleCommand c = drive_vehicle_course(slalom, *m, w->state(0), t, memory);
				const VehicleSlot slot = 0;
				(void)w->step(std::span<const VehicleSlot>(&slot, 1), std::span<const VehicleCommand>(&c, 1));
				r_hashes.push_back(w->state_hash());
			}
		};
		std::vector<uint64_t> single, a, b;
		run(single);
		std::thread ta([&] { run(a); });
		std::thread tb([&] { run(b); });
		ta.join();
		tb.join();
		CHECK(a == single);
		CHECK(b == single);
	}

	TEST_CASE("sim.vehicle: handling numbers of the fixture model (report H, unverified placeholders)") {
		core::Result<VehicleHandling> h = measure_vehicle_handling(fixture_model());
		REQUIRE_MESSAGE(h.ok(), (h.ok() ? std::string() : h.error().message));
		print_handling("fixture", *h);
		CHECK(h->time_0_60_kmh > 0.0f);
		CHECK(h->brake_distance_60_m > 0.0f);
		CHECK(h->turning_circle_m > 0.0f);
	}
}

namespace {

constexpr const char *kHumvee = "vehicles/humveearmored/humveearmored";

struct Caspian {
	std::unique_ptr<core::InstallReader> reader;
	std::shared_ptr<core::Install> install;
	core::InstallSnapshot before;
	core::level::LevelCollision collision;
	std::shared_ptr<const StaticWorldShapes> shapes;
	std::unique_ptr<CollisionWorld> world;
	std::vector<core::level::VehicleSpawnerDef> spawners;
	std::shared_ptr<const VehicleModel> model;
	VehicleTuning tuning;
	double seconds_soup = 0, seconds_shapes = 0;
};

VehicleTuning shipped_vehicle_tuning() {
#ifdef OPENBF3_VEHICLE_TUNING_CFG
	core::Result<VehicleTuning> t = parse_vehicle_tuning(read_text(OPENBF3_VEHICLE_TUNING_CFG));
	REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
	return *t;
#else
	return {};
#endif
}

VehiclePlacement placement_of(const core::level::LevelTransform &p_t, float p_lift) {
	VehiclePlacement p;
	p.position = { p_t.trans[0], p_t.trans[1] + p_lift, p_t.trans[2] };
	const double h = std::atan2(double(p_t.forward[0]), double(p_t.forward[2]));
	p.rotation = { 0.0f, float(std::sin(h / 2)), 0.0f, float(std::cos(h / 2)) };
	return p;
}

Caspian *caspian() {
	static std::unique_ptr<Caspian> s_caspian;
	static bool s_tried = false;
	if (s_tried) {
		if (s_caspian == nullptr) {
			bf3::test::skip_install_case("no BF3 install (see the first sim-vehicle case)");
		}
		return s_caspian.get();
	}
	s_tried = true;
	auto m = std::make_unique<Caspian>();
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
	core::Result<core::level::LevelCollision> c = core::level::load_level_collision(m->install, "MP_007", "RushLarge0");
	REQUIRE_MESSAGE(c.ok(), (c.ok() ? std::string() : c.error().message));
	m->collision = std::move(*c);
	m->seconds_soup = seconds_since(start);
	start = std::chrono::steady_clock::now();
	core::Result<std::shared_ptr<const StaticWorldShapes>> shapes = build_static_world(m->collision.soup);
	REQUIRE_MESSAGE(shapes.ok(), (shapes.ok() ? std::string() : shapes.error().message));
	m->shapes = *shapes;
	m->seconds_shapes = seconds_since(start);
	core::Result<std::unique_ptr<CollisionWorld>> cw = CollisionWorld::create(m->shapes);
	REQUIRE_MESSAGE(cw.ok(), (cw.ok() ? std::string() : cw.error().message));
	m->world = std::move(*cw);
	core::Result<std::vector<core::level::VehicleSpawnerDef>> spawners = core::level::read_vehicle_spawners(*m->collision.world);
	REQUIRE_MESSAGE(spawners.ok(), (spawners.ok() ? std::string() : spawners.error().message));
	for (const core::level::VehicleSpawnerDef &s : *spawners) {
		if (s.blueprint == kHumvee) {
			m->spawners.push_back(s);
		}
	}
	m->tuning = shipped_vehicle_tuning();
	core::Result<std::shared_ptr<const VehicleModel>> model = load_vehicle_model(m->collision.level->db(), m->collision.level->mount(), kHumvee, m->tuning);
	REQUIRE_MESSAGE(model.ok(), (model.ok() ? std::string() : model.error().message));
	m->model = *model;
	const Vec3 lo = m->shapes->min(), hi = m->shapes->max();
	std::printf("MP_007 RushLarge0: soup %.2f s, shapes %.2f s (noisy), %zu tiles, bounds (%.1f, %.1f, %.1f)..(%.1f, %.1f, %.1f), %zu vehicle spawners "
				"(%zu Humvee)\n",
			m->seconds_soup, m->seconds_shapes, m->shapes->tile_count(), double(lo[0]), double(lo[1]), double(lo[2]), double(hi[0]), double(hi[1]),
			double(hi[2]), spawners->size(), m->spawners.size());
	s_caspian = std::move(m);
	return s_caspian.get();
}

class OverlayResolver final : public core::EbxResolver {
public:
	OverlayResolver(const core::EbxResolver &p_base, std::string_view p_name, const core::EbxPartition &p_partition) :
			base_(p_base), name_(p_name), partition_(p_partition) {}
	const core::EbxPartition *partition(const core::Guid &p_guid) const override {
		return p_guid == partition_.guid() ? &partition_ : base_.partition(p_guid);
	}
	const core::EbxPartition *partition_by_name(std::string_view p_name) const override {
		return p_name == name_ ? &partition_ : base_.partition_by_name(p_name);
	}
	std::optional<std::string_view> name_of(const core::Guid &p_guid) const override {
		return p_guid == partition_.guid() ? std::optional<std::string_view>(name_) : base_.name_of(p_guid);
	}

private:
	const core::EbxResolver &base_;
	std::string name_;
	const core::EbxPartition &partition_;
};

}

TEST_SUITE("sim-vehicle") {
	TEST_CASE("sim.vehicle: the Humvee drive def as s2.md 4.2 lists it; the 6 Humvee spawners of 4.3; the model's 5 chassis pieces of 4.5") {
		Caspian *m = caspian();
		if (m == nullptr) {
			return;
		}
		gp::WorkBudget budget;
		core::Result<gp::VehicleDriveDef> def = gp::read_vehicle_drive_def(m->collision.level->db(), kHumvee, &budget);
		REQUIRE_MESSAGE(def.ok(), (def.ok() ? std::string() : def.error().message));
		const gp::VehicleDriveDef &d = *def;
		std::printf("drive def: %zu components, %zu wheels, %zu wheel configs, %zu engines, %zu rigid bodies, asset %s x %.2f\n", d.components, d.wheels.size(),
				d.wheel_configs.size(), d.engines.size(), d.rigid_bodies.size(), d.physics_asset.c_str(), double(d.physics_asset_scale));
		CHECK(d.chassis.body_mass == 1500.0f);
		CHECK(d.chassis.gravity_modifier == doctest::Approx(1.85f));
		CHECK(d.chassis.center_of_mass[1] == doctest::Approx(0.6f));
		CHECK(d.chassis.center_of_mass[2] == doctest::Approx(0.2f));
		CHECK(d.chassis.inertia_modifier[0] == doctest::Approx(1.49f));
		CHECK(d.chassis.air_drag_area == doctest::Approx(9.0f));
		CHECK(d.chassis.anti_roll_front == doctest::Approx(1.2f));
		CHECK(d.chassis.anti_roll_rear == doctest::Approx(0.5f));
		CHECK(d.chassis.yaw.inertia_in == doctest::Approx(0.23f));
		CHECK(d.chassis.yaw.inertia_out == doctest::Approx(0.85f));
		CHECK(d.chassis.use_stand_still_brake);
		CHECK(d.chassis.stand_still_low_speed_time_limit == doctest::Approx(2.0f));
		CHECK(d.chassis.allow_negative_throttle);
		size_t normal = 0;
		for (const gp::VehicleWheelDef &w : d.wheels) {
			normal += w.physics_type == "wptNormal" ? 1 : 0;
		}
		CHECK(normal == 4);
		REQUIRE(d.wheel_configs.size() >= 2);
		CHECK(d.wheel_configs[0].radius == doctest::Approx(0.46f));
		CHECK(d.wheel_configs[0].spring_length == doctest::Approx(0.4f));
		CHECK(d.wheel_configs[0].brake_force == doctest::Approx(4000.0f));
		CHECK(d.wheel_configs[0].diff_gear_ratio == doctest::Approx(3.8f));
		REQUIRE(!d.engines.empty());
		CHECK(d.engines[0].rpm_points.size() == 7);
		CHECK(d.engines[0].rpm_max == doctest::Approx(5000.0f));
		CHECK(d.gearbox.present);
		CHECK(d.gearbox.forward_ratios.size() == 5);
		CHECK(d.gearbox.reverse_ratios.size() == 2);
		REQUIRE(d.rigid_bodies.size() >= 1);
		CHECK(d.rigid_bodies[0].body_type == "RBTypeCollision");
		CHECK(d.rigid_bodies[0].friction == doctest::Approx(0.5f));
		CHECK(d.physics_asset == "Vehicles/HumveeArmored/HumveeArmored_Physics_0_Win32");

		CHECK(m->spawners.size() == 6);
		const float expected[6][3] = { { 1.02f, 141.00f, 765.79f }, { -35.04f, 141.49f, 727.20f }, { 60.75f, 132.00f, 268.41f }, { 90.13f, 132.00f, 249.63f },
			{ -158.91f, 140.11f, -91.97f }, { -137.93f, 140.42f, -92.79f } };
		size_t matched = 0;
		for (const auto &e : expected) {
			for (const core::level::VehicleSpawnerDef &s : m->spawners) {
				if (std::fabs(s.transform.trans[0] - e[0]) < 0.02f && std::fabs(s.transform.trans[1] - e[1]) < 0.02f && std::fabs(s.transform.trans[2] - e[2]) < 0.02f) {
					++matched;
					break;
				}
			}
		}
		CHECK(matched == 6);
		for (const core::level::VehicleSpawnerDef &s : m->spawners) {
			std::printf("spawner (%.2f, %.2f, %.2f) team %d enabled %d auto %d delay %.1f s radius %.1f m\n", double(s.transform.trans[0]),
					double(s.transform.trans[1]), double(s.transform.trans[2]), s.team, int(s.enabled), int(s.auto_spawn), double(s.spawn_delay),
					double(s.spawn_area_radius));
		}

		const VehicleModel &v = *m->model;
		CHECK(v.chassis.size() == 5);
		CHECK(v.wheel_count == 4);
		CHECK(v.mass == 1500.0f);
		float lowest = 1e9f;
		for (const ChassisPiece &p : v.chassis) {
			for (const Vec3 &q : p.points) {
				lowest = std::min(lowest, q[1]);
			}
		}
		CHECK(lowest == doctest::Approx(0.417f).epsilon(0.01));
		std::printf("model: digest %016llx, %zu pieces, tuning hash %016llx\n", (unsigned long long)v.model_digest, v.chassis.size(),
				(unsigned long long)vehicle_tuning_hash(m->tuning));
	}

	TEST_CASE("sim.vehicle: MP_007: the vehicle world's static BodyIDs equal CollisionWorld's (report S: the spawn settle per Humvee spawner)") {
		Caspian *m = caspian();
		if (m == nullptr) {
			return;
		}
		VehicleWorldOptions o;
		o.max_vehicles = 1;
		o.tuning = m->tuning;
		core::Result<std::unique_ptr<VehicleWorld>> w = VehicleWorld::create(m->shapes, o);
		REQUIRE(w.ok());
		const std::vector<JPH::BodyID> &reference = m->world->impl().bodies;
		REQUIRE((*w)->static_body_count() == reference.size());
		size_t differ = 0;
		for (size_t i = 0; i < reference.size(); ++i) {
			differ += (*w)->static_body_id(i) == reference[i].GetIndexAndSequenceNumber() ? 0 : 1;
		}
		CHECK(differ == 0);
		std::vector<VehiclePlacement> at;
		for (const core::level::VehicleSpawnerDef &s : m->spawners) {
			at.push_back(placement_of(s.transform, 0.1f));
		}
		core::Result<std::vector<VehicleSettleReport>> r = measure_vehicle_settle(m->shapes, m->model, at);
		REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
		size_t settled = 0;
		for (const VehicleSettleReport &s : *r) {
			std::printf("S spawner (%.2f, %.2f, %.2f): drop %.3f m, drift y %.5f m, drift %.5f m, wheels in contact %u: %s\n", double(s.spawn[0]),
					double(s.spawn[1]), double(s.spawn[2]), double(s.drop), double(s.drift_y), double(s.drift), unsigned(s.wheels_in_contact),
					s.settled ? "settled" : "NOT settled");
			settled += s.settled ? 1 : 0;
		}
		std::printf("S: %zu of %zu spawners settle within %.2f m (report only)\n", settled, r->size(), double(kSettleDriftLimit));
	}

	TEST_CASE("sim.vehicle: MP_007 courses (gate C, queries per kind), then D, G1, G1r and G2x on every found course, G2 reported") {
		Caspian *m = caspian();
		if (m == nullptr) {
			return;
		}
		std::vector<Vec3> seeds;
		for (const core::level::VehicleSpawnerDef &s : m->spawners) {
			seeds.push_back(s.transform.trans);
		}
		auto start = std::chrono::steady_clock::now();
		core::Result<std::array<VehicleCourse, kVehicleCourseKindCount>> courses = find_vehicle_courses(*m->world, seeds, *m->model);
		REQUIRE_MESSAGE(courses.ok(), (courses.ok() ? std::string() : courses.error().message));
		std::printf("C: course search %.1f s (noisy)\n", seconds_since(start));
		for (const VehicleCourse &c : *courses) {
			std::printf("C %-10s %s: %zu queries%s; start (%.1f, %.1f, %.1f) a (%.1f, %.1f, %.1f) b (%.1f, %.1f, %.1f) slope %.1f deg feature %.3f seed %u\n",
					vehicle_course_name(c.kind), c.found ? "found" : "NOT FOUND", c.queries, c.budget_exhausted ? " (budget exhausted)" : "",
					double(c.start.position[0]), double(c.start.position[1]), double(c.start.position[2]), double(c.a[0]), double(c.a[1]), double(c.a[2]),
					double(c.b[0]), double(c.b[1]), double(c.b[2]), double(c.slope_degrees), double(c.feature), c.seed);
		}
		VehicleReplayOptions o;
		if (kAsan) {

			o.modes = (1u << uint32_t(VehicleRestoreMode::OwnHistory)) | (1u << uint32_t(VehicleRestoreMode::ExactPod));
		}
		start = std::chrono::steady_clock::now();
		uint64_t steps = 0;
		std::vector<VehicleCourseReport> reports;

		std::vector<std::optional<core::Result<VehicleCourseReport>>> runs = core::parallel_map<std::optional<core::Result<VehicleCourseReport>>>(
				courses->size(),
				[&](size_t i) -> std::optional<core::Result<VehicleCourseReport>> {
					if (!(*courses)[i].found) {
						return std::nullopt;
					}
					return run_vehicle_course(m->shapes, m->model, (*courses)[i], o);
				},
				kCourseLanes);
		for (size_t i = 0; i < courses->size(); ++i) {
			const VehicleCourse &c = (*courses)[i];
			VehicleCourseReport r;
			r.course = c;
			if (c.found) {
				core::Result<VehicleCourseReport> &run = *runs[i];
				REQUIRE_MESSAGE(run.ok(), (run.ok() ? std::string() : run.error().message));
				r = std::move(*run);
				std::printf("%s", format_vehicle_course_report(r).c_str());
				check_pod_bases(r);
				steps += r.steps;
			}
			reports.push_back(std::move(r));
		}
		std::printf("MP_007 courses: %llu steps in %.1f s (noisy)\n", (unsigned long long)steps, seconds_since(start));
		std::string why;
		CHECK_MESSAGE(vehicle_gate_courses(reports, &why), "C: " << why);
		why.clear();
		CHECK_MESSAGE(vehicle_gate_drove(reports, &why), "D: " << why);
		why.clear();
		CHECK_MESSAGE(vehicle_gate_g1(reports, &why), "G1: " << why);

		why.clear();
		const bool g2 = vehicle_gate_g2(reports, &why);
		std::printf("G2 (report only, quantised ghost): %s%s%s\n", g2 ? "pass" : "over 2 cm", why.empty() ? "" : ": ", why.c_str());
		why.clear();
		CHECK_MESSAGE(vehicle_gate_g2x(reports, &why), "G2x: " << why);
	}

	TEST_CASE("sim.vehicle: handling numbers of the real Humvee model on the flat ground (report H, unverified placeholders)") {
		Caspian *m = caspian();
		if (m == nullptr) {
			return;
		}
		core::Result<VehicleHandling> h = measure_vehicle_handling(m->model);
		REQUIRE_MESSAGE(h.ok(), (h.ok() ? std::string() : h.error().message));
		print_handling("MP_007 Humvee", *h);
	}

	TEST_CASE("sim.vehicle: 200 seeded mutations of the Humvee partition through read_vehicle_drive_def and build_vehicle_model never crash") {
		Caspian *m = caspian();
		if (m == nullptr) {
			return;
		}
		const core::EbxDb &db = m->collision.level->db();
		const core::EbxPartition *p = db.partition_by_name(kHumvee);
		REQUIRE(p != nullptr);
		const std::vector<uint8_t> original(p->bytes().begin(), p->bytes().end());
		std::vector<std::pair<uint32_t, uint32_t>> instances;
		for (const core::EbxPartition::InstanceEntry &e : p->instance_entries()) {
			instances.emplace_back(e.offset, p->complex_descs()[e.complex].size);
		}
		REQUIRE(!instances.empty());
		const std::vector<std::vector<Vec3>> pieces = humvee_pieces();
		std::vector<ChassisPieceSource> sources;
		for (const auto &q : pieces) {
			sources.push_back({ std::span<const Vec3>(q), 0.03f });
		}
		std::mt19937 rng(0x5332u);
		size_t parsed = 0, read = 0, built = 0, refused = 0;
		const auto attempt = [&](std::vector<uint8_t> p_bytes) {
			core::Result<core::EbxPartition> mutated = core::EbxPartition::parse(std::move(p_bytes));
			if (!mutated.ok()) {
				return;
			}
			++parsed;
			const OverlayResolver overlay(db, kHumvee, *mutated);
			gp::WorkBudget budget;
			core::Result<gp::VehicleDriveDef> def = gp::read_vehicle_drive_def(overlay, kHumvee, &budget);
			if (!def.ok()) {
				++refused;
				return;
			}
			++read;
			core::Result<std::shared_ptr<const VehicleModel>> model = build_vehicle_model(*def, sources, m->tuning);
			(model.ok() ? built : refused) += 1;
		};
		for (int iter = 0; iter < 200; ++iter) {
			std::vector<uint8_t> bytes = original;
			const int flips = 1 + int(rng() % 8);
			for (int k = 0; k < flips; ++k) {
				const auto &[at, size] = instances[rng() % instances.size()];
				if (size == 0 || uint64_t(at) + size > bytes.size()) {
					continue;
				}
				bytes[at + rng() % size] ^= uint8_t(1u << (rng() % 8));
			}
			attempt(std::move(bytes));
		}
		std::printf("Humvee partition fuzz: %zu of 200 mutations parsed, %zu read, %zu built a model, %zu refused with an error\n", parsed, read, built, refused);
		CHECK(parsed > 0);

		const uint32_t words[] = { 0x7FC00000u, 0x7F800000u, 0xFF800000u, 0x7149F2CAu  , 0xF149F2CAu, 0xBF800000u  , 0u, 0xFFFFFFFFu,
			0x00010000u };
		const size_t parsed_flips = parsed, refused_flips = refused, built_flips = built;
		for (int iter = 0; iter < 400; ++iter) {
			std::vector<uint8_t> bytes = original;
			const auto &[at, size] = instances[rng() % instances.size()];
			if (size < 4 || uint64_t(at) + size > bytes.size()) {
				continue;
			}
			const uint32_t offset = at + 4u * uint32_t(rng() % (size / 4));
			const uint32_t w = words[rng() % std::size(words)];
			std::memcpy(bytes.data() + offset, &w, 4);
			attempt(std::move(bytes));
		}
		std::printf("Humvee partition word fuzz: %zu of 400 mutations parsed, %zu refused with an error, %zu built a model\n", parsed - parsed_flips,
				refused - refused_flips, built - built_flips);
		CHECK(parsed > 0);
	}

	TEST_CASE("sim.vehicle: MP_007: install unchanged (layout.toc, cas.cat, the .sb / .toc files)") {
		Caspian *m = caspian();
		if (m == nullptr) {
			return;
		}
		const core::SnapshotDiff diff = core::diff_snapshots(m->before, m->reader->snapshot());
		std::printf("install.unchanged: %s\n", diff.clean() ? "clean" : diff.summary().c_str());
		CHECK(diff.clean());
	}
}
