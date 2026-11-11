#include <doctest/doctest.h>

#include "bf3/sim/vehicles/land_rules.h"
#include "bf3/sim/vehicles/vehicle_spawning.h"

#include <array>
#include <cmath>
#include <vector>

using namespace bf3::sim::vehicles;

namespace {

constexpr std::array<float, 3> kRpm{ 1000.0f, 3000.0f, 5000.0f };
constexpr std::array<float, 3> kTorque{ 200.0f, 400.0f, 300.0f };
constexpr std::array<float, 3> kForwardSpeeds{ 20.0f, 40.0f, 60.0f };
constexpr std::array<float, 2> kReverseSpeeds{ 15.0f, 30.0f };

GearboxRule gearbox(int32_t p_mode, bool p_neutral) {
	GearboxRule r;
	r.forward_speeds = kForwardSpeeds;
	r.reverse_speeds = kReverseSpeeds;
	r.forward_gears = 3;
	r.reverse_gears = 2;
	r.gearbox_type = 1;
	r.gearbox_mode = p_mode;
	r.gear_change_time = 0.3f;
	r.gear_down_speed_factor = 0.8f;
	r.opposite_dir_gear_change_max_speed = 10.0f;
	r.opposite_dir_gear_change_time = 0.5f;
	r.clutch_speed_factor = 5.0f;
	r.use_neutral_gear = p_neutral;
	return r;
}

float kmh(float p_kmh) {
	return p_kmh / 3.6f;
}

}

TEST_SUITE("sim-vehicle-rules-unit") {
	TEST_CASE("sim.vehicles.land_rules: torque by engine rpm (R-G1 vectors: 350, 200, 500, 0)") {
		CHECK(engine_torque(4000.0f, kRpm, kTorque, 2.0f, 0.5f, 0.0f) == doctest::Approx(350.0f));
		CHECK(engine_torque(500.0f, kRpm, kTorque, 2.0f, 0.5f, 0.0f) == doctest::Approx(200.0f));
		CHECK(engine_torque(6000.0f, kRpm, kTorque, 2.0f, 1.0f, 0.0f) == doctest::Approx(500.0f));
		CHECK(engine_torque(4000.0f, kRpm, kTorque, 2.0f, 1.0f, 1.0f) == 0.0f);
		CHECK(torque_curve_at(1000.0f, kRpm, kTorque) == doctest::Approx(200.0f));
		CHECK(torque_curve_at(5000.0f, kRpm, kTorque) == doctest::Approx(300.0f));
	}

	TEST_CASE(
			"sim.vehicles.land_rules: dead zone is the data value less 0.15 (0.55 -> 0.5, 0.05 -> 0, -0.55 -> -0.5)") {
		CHECK(shape_deadzone(0.55f, 0.25f, false) == doctest::Approx(0.5f));
		CHECK(shape_deadzone(0.05f, 0.25f, false) == 0.0f);
		CHECK(shape_deadzone(-0.55f, 0.25f, false) == 0.0f);
		CHECK(shape_deadzone(-0.55f, 0.25f, true) == doctest::Approx(-0.5f));
		CHECK(shape_deadzone(1.0f, 0.25f, false) == doctest::Approx(1.0f));
		CHECK(shape_deadzone(0.7f, 0.1f, false) == doctest::Approx(0.7f));
	}

	TEST_CASE("sim.vehicles.land_rules: inertia (dt 1/60, In 0.5, Out 1, MinRatio 0.2): jump to MinRatio, ramps, snap "
			  "through zero") {
		const InputAxisRule axis{ 0.0f, 0.5f, 1.0f, 0.2f };
		const float dt = 1.0f / 60.0f;
		float v = shape_inertia(0.0f, 1.0f, dt, axis);
		CHECK(v == doctest::Approx(0.216667f).epsilon(1e-4));
		v = shape_inertia(v, 1.0f, dt, axis);
		CHECK(v == doctest::Approx(0.233333f).epsilon(1e-4));
		int ticks = 2;
		while (v < 1.0f && ticks < 100) {
			const float before = v;
			v = shape_inertia(v, 1.0f, dt, axis);
			CHECK((v - before) <= doctest::Approx(1.0f / 60.0f).epsilon(1e-3));
			++ticks;
		}
		CHECK(v == 1.0f);
		CHECK(ticks >= 48);
		CHECK(ticks <= 49);

		v = 1.0f;
		for (int i = 0; i < 23; ++i) {
			const float before = v;
			v = shape_inertia(v, 0.0f, dt, axis);
			CHECK((before - v) == doctest::Approx(1.0f / 30.0f).epsilon(1e-3));
		}
		CHECK(v == doctest::Approx(0.2333f).epsilon(1e-3));
		v = shape_inertia(v, 0.0f, dt, axis);
		const bool boundary = v == 0.0f || v == doctest::Approx(0.2f).epsilon(1e-3);
		CHECK(boundary);
		v = shape_inertia(v, 0.0f, dt, axis);
		CHECK(v == 0.0f);

		v = -1.0f;
		float previous = v;
		bool crossed = false;
		for (int i = 0; i < 200 && v < 1.0f; ++i) {
			previous = v;
			v = shape_inertia(v, 1.0f, dt, axis);
			if (previous < 0.0f && v > 0.0f) {
				crossed = true;
				CHECK(std::fabs(previous) < 0.2334f);
				CHECK(v >= 0.2f);
			} else if (v <= 0.0f) {
				CHECK((v - previous) == doctest::Approx(1.0f / 30.0f).epsilon(1e-3));
			}
		}
		CHECK(crossed);
		CHECK(shape_inertia(0.3f, 0.7f, dt, InputAxisRule{}) == 0.7f);
	}

	TEST_CASE("sim.vehicles.land_rules: gearbox decision (R2-G1 vectors) with the checker's corrections") {
		const GearboxRule r = gearbox(1, true);
		GearboxState s;
		s.gear = 2;
		CHECK(gearbox_request(r, s, 0.5f, 0.0f, kmh(45.0f)) == GearRequest::Up);
		CHECK(gearbox_request(r, s, 0.5f, 0.0f, kmh(15.0f)) == GearRequest::Down);
		CHECK(gearbox_request(r, s, 0.5f, 0.0f, kmh(30.0f)) == GearRequest::None);
		s.gear = 1;
		CHECK(gearbox_request(r, s, 0.0f, 0.0f, 0.9f) == GearRequest::Down);
		CHECK(gearbox_request(r, s, 0.5f, 0.0f, 0.9f) == GearRequest::None);
		CHECK(gearbox_request(r, s, 0.5f, 0.5f, kmh(9.0f)) == GearRequest::Down);
		CHECK(gearbox_request(r, s, 0.0f, 0.5f, kmh(11.0f)) == GearRequest::None);
		s.gear = -1;
		CHECK(gearbox_request(r, s, 0.5f, 0.0f, kmh(16.0f)) == GearRequest::Down);
		CHECK(gearbox_request(r, s, 0.0f, 0.5f, kmh(5.0f)) == GearRequest::Up);
		s.gear = 0;
		CHECK(gearbox_request(r, s, 0.0f, 0.5f, 0.0f) == GearRequest::OppositeDirection);
		CHECK(gearbox_request(r, s, 0.5f, 0.0f, 0.0f) == GearRequest::Up);
		s.reverse_direction = 1;
		CHECK(gearbox_request(r, s, 0.5f, 0.0f, 0.0f) == GearRequest::Down);
		s.phase = GearPhase::Wait;
		CHECK(gearbox_request(r, s, 0.5f, 0.0f, 0.0f) == GearRequest::None);
		GearboxRule manual = r;
		manual.gearbox_type = 0;
		CHECK(gearbox_request(manual, GearboxState{}, 1.0f, 0.0f, 100.0f) == GearRequest::None);

		const GearboxRule no_neutral = gearbox(1, false);
		CHECK(gear_step(no_neutral, 1, -1) == -1);
		CHECK(gear_step(no_neutral, -1, 1) == 1);
		CHECK(gear_step(r, 1, -1) == 0);
		CHECK(gear_step(r, 3, 1) == 3);
		CHECK(gear_step(r, -2, -1) == -2);
		GearboxState top;
		top.gear = 3;
		CHECK_FALSE(gearbox_accept(r, top, GearRequest::Up));
		CHECK(top.phase == GearPhase::Idle);
	}

	TEST_CASE("sim.vehicles.land_rules: shift sequence (mode 1 clutch 1/6 per tick, 0.3 s wait, back down; modes 0 and "
			  "2 leave the clutch)") {
		const GearboxRule r = gearbox(1, true);
		const float dt = 1.0f / 30.0f;
		GearboxState s;
		s.gear = 1;
		REQUIRE(gearbox_accept(r, s, GearRequest::Up));
		CHECK(s.phase == GearPhase::Disengage);
		CHECK_FALSE(gearbox_accept(r, s, GearRequest::Up));
		int disengage = 0;
		while (s.phase == GearPhase::Disengage && disengage < 20) {
			const float before = s.clutch;
			gearbox_advance(r, s, dt);
			++disengage;
			if (s.phase == GearPhase::Disengage) {
				CHECK((s.clutch - before) == doctest::Approx(1.0f / 6.0f).epsilon(1e-4));
			}
		}
		CHECK(disengage >= 6);
		CHECK(disengage <= 7);
		CHECK(s.clutch == 1.0f);
		CHECK(engine_torque(3000.0f, kRpm, kTorque, 1.0f, 1.0f, s.clutch) == 0.0f);
		int wait = 0;
		while (s.phase == GearPhase::Wait && wait < 30) {
			CHECK(s.gear == 1);
			gearbox_advance(r, s, dt);
			++wait;
		}
		CHECK(wait >= 9);
		CHECK(wait <= 10);
		CHECK(s.gear == 2);
		CHECK(s.phase == GearPhase::Reengage);
		int reengage = 0;
		while (s.phase == GearPhase::Reengage && reengage < 20) {
			gearbox_advance(r, s, dt);
			++reengage;
		}
		CHECK(reengage >= 6);
		CHECK(reengage <= 7);
		CHECK(s.clutch == 0.0f);
		CHECK(s.phase == GearPhase::Idle);

		for (const int32_t mode : { 0, 2 }) {
			const GearboxRule m = gearbox(mode, true);
			GearboxState t;
			t.gear = 1;
			REQUIRE(gearbox_accept(m, t, GearRequest::Up));
			CHECK(t.phase == GearPhase::Wait);
			CHECK(gearbox_clutch_pressed(m, t) == (mode == 0));
			for (int i = 0; i < 10; ++i) {
				gearbox_advance(m, t, dt);
			}
			CHECK(t.gear == 2);
			CHECK(t.clutch == 0.0f);
			CHECK(t.phase == GearPhase::Idle);
			CHECK_FALSE(gearbox_clutch_pressed(m, t));
		}

		GearboxState turn;
		turn.gear = 0;
		REQUIRE(gearbox_accept(r, turn, GearRequest::OppositeDirection));
		for (int i = 0; i < 16; ++i) {
			gearbox_advance(r, turn, dt);
		}
		CHECK(turn.reverse_direction == 1);
		CHECK(turn.phase == GearPhase::Idle);
		GearboxRule classic = r;
		classic.use_classic_auto_clutch = true;
		GearboxState shift;
		REQUIRE(gearbox_accept(classic, shift, GearRequest::Up));
		CHECK(gearbox_clutch_pressed(classic, shift));
		CHECK_FALSE(gearbox_clutch_pressed(r, shift));
	}

	TEST_CASE("sim.vehicles.land_rules: engine rpm (wheel rpm 1031.3, step 1133.33, the 1 rpm minimum step, targets "
			  "per mode)") {
		const std::array<float, 2> wheels{ 10.0f, -10.0f };
		CHECK(wheel_rpm(wheels, 3.0f, 4.0f, 0.9f) == doctest::Approx(1031.3f).epsilon(1e-4));
		const EngineRule e{ 800.0f, 5000.0f, 2.0f, 1.0f, 1 };
		CHECK(engine_rpm_step(e, 1000.0f, 3000.0f, 0.0f, 1.0f / 30.0f) == doctest::Approx(1133.33f).epsilon(1e-5));
		CHECK(engine_rpm_step(e, 1000.0f, 1000.5f, 0.0f, 1.0f / 30.0f) == doctest::Approx(1000.5f));
		CHECK(engine_rpm_step(e, 1000.0f, 0.0f, 0.0f, 1.0f / 30.0f) == doctest::Approx(1000.0f - 1000.0f / 30.0f));
		CHECK(engine_rpm_step(e, 4990.0f, 9000.0f, 0.0f, 1.0f / 30.0f) == 5000.0f);
		CHECK(engine_rpm_target(e, true, false, 0.0f, 1.0f, 1500.0f) == doctest::Approx(800.0f + 1500.0f));
		CHECK(engine_rpm_target(e, true, false, 1.0f, 0.5f, 1500.0f) == doctest::Approx(800.0f + 0.5f * 4200.0f));
		CHECK(engine_rpm_target(e, true, true, 0.5f, 1.0f, 1500.0f) == 800.0f);
		CHECK(engine_rpm_target(e, false, false, 0.0f, 0.5f, 0.0f) == doctest::Approx(800.0f + 2100.0f));
		const EngineRule zero{ 800.0f, 5000.0f, 2.0f, 1.0f, 0 };
		CHECK(engine_rpm_target(zero, true, false, 0.0f, 1.0f, 1500.0f) == 1500.0f);
		CHECK(engine_rpm_target(zero, true, true, 0.0f, 0.1f, 1500.0f) == 800.0f);
		CHECK(engine_rpm_target(zero, true, false, 0.0f, 1.0f, 9000.0f) == 5000.0f);
		const EngineRule two{ 800.0f, 5000.0f, 2.0f, 1.0f, 2 };
		CHECK(engine_rpm_target(two, true, true, 0.0f, 1.0f, 1500.0f) == doctest::Approx(2300.0f));
		CHECK(engine_load(1000.0f, 2000.0f, 0.0f, 1.0f) == doctest::Approx(0.5f));
		CHECK(engine_load(3000.0f, 2000.0f, 0.0f, 1.0f) == 0.0f);
		CHECK(engine_load(2000.0f, 2000.0f, std::sin(3.14159265f / 8.0f), 1.0f) == doctest::Approx(0.5f).epsilon(1e-4));
		CHECK(engine_rpm_desired(3000.0f, 0.5f, 0.0f) == doctest::Approx(1500.0f));
	}

	TEST_CASE("sim.vehicles.land_rules: spring progressive 2^(1/3), substep factors 1 / 1.5 / 1.75; brake x0.9; "
			  "stand-still friction; ellipse; slip; boost") {
		CHECK(spring_progressive(0.4f, 0.3f, 2.0f, 1.0f) == doctest::Approx(1.259921f).epsilon(1e-5));
		CHECK(spring_progressive(0.2f, 0.3f, 2.0f, 1.0f) == 1.0f);
		CHECK(spring_substep_factor(1) == 1.0f);
		CHECK(spring_substep_factor(2) == 1.5f);
		CHECK(spring_substep_factor(3) == 1.75f);
		CHECK(spring_substep_factor(9) == 1.75f);
		CHECK(spring_substep_factor(0) == 1.0f);

		CHECK(brake_scale(10.0f, 0.1f, 1.0f) == doctest::Approx(9.0f));
		CHECK(brake_scale(10.0f, 1.5f, 1.0f) == 0.0f);
		CHECK(wheel_drive_torque(350.0f, 3.0f, 4.0f, 0.9f, 1.0f) == doctest::Approx(70.875f));
		CHECK(wheel_drive_torque(350.0f, -3.0f, 4.0f, 0.9f, 0.5f) == doctest::Approx(-35.4375f));
		CHECK(brake_scale(10.0f, 0.1f, 0.0f) == 10.0f);
		CHECK(brake_scale(10.0f, 20.0f, 1.0f) == 0.0f);

		const float s9 = kmh(9.0f) * kmh(9.0f);
		const float s11 = kmh(11.0f) * kmh(11.0f);
		CHECK(chassis_friction(s9, 0.1f, 1.0f, 0.0f) == 1.0f);
		CHECK(chassis_friction(s9, 0.3f, 1.0f, 0.0f) == 0.0f);
		CHECK(chassis_friction(s9, 0.25f, 1.0f, 0.0f) == 0.0f);
		CHECK(chassis_friction(s11, 0.0f, 1.0f, 0.0f) == 1.0f);

		const EllipseForces f = friction_ellipse(3000.0f, 4000.0f, 5000.0f, 8000.0f);
		CHECK(f.lateral == doctest::Approx(3200.0f));
		CHECK(f.longitudinal == doctest::Approx(2598.1f).epsilon(1e-4));

		CHECK(clamp_slip(250.0f) == 200.0f);
		CHECK(clamp_slip(-250.0f) == -200.0f);
		CHECK(clamp_slip(12.5f) == 12.5f);

		const BoostRule boost{ 3.0f, 6.0f };
		float tank = 3.0f;
		int boosted = 0;
		while (tank > 0.0f && boosted < 1000) {
			tank = boost_tank_step(boost, tank, true, 1.0f / 60.0f);
			++boosted;
		}
		CHECK(boosted >= 180);
		CHECK(boosted <= 181);
		CHECK(boost_tank_step(boost, 0.0f, false, 1.0f) == doctest::Approx(0.5f));
		CHECK(boost_tank_step(boost, 2.9f, false, 1.0f) == 3.0f);
	}

	TEST_CASE("sim.vehicles.land_rules: vehicle spawn delay (M-CQ4: initial 15 s then 40 s; modifier 0.5 gives 7.5 s "
			  "and 20 s)") {
		CHECK(vehicle_spawn_delay(0, 15.0f, 40.0f, 1.0f) == 15.0f);
		CHECK(vehicle_spawn_delay(1, 15.0f, 40.0f, 1.0f) == 40.0f);
		CHECK(vehicle_spawn_delay(7, 15.0f, 40.0f, 1.0f) == 40.0f);
		CHECK(vehicle_spawn_delay(0, 15.0f, 40.0f, 0.5f) == 7.5f);
		CHECK(vehicle_spawn_delay(2, 15.0f, 40.0f, 0.5f) == 20.0f);
	}
}
