#include <doctest/doctest.h>

#include "bf3/sim/vehicles/chassis_rules.h"
#include "bf3/sim/vehicles/land_rules.h"
#include "bf3/sim/vehicles/land_wheel.h"
#include "bf3/sim/vehicles/tyre_rules.h"
#include "bf3/sim/vehicles/vehicle_input.h"
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

	TEST_CASE(
			"sim.vehicles.vehicle_input: only the vehicle's handbrake bit holds the handbrake; the infantry jump does "
			"not") {
		namespace b = bf3::sim::buttons;
		CHECK(handbrake_held(seat_buttons::kHandbrake));
		CHECK(handbrake_held(seat_buttons::kHandbrake | b::kJump));
		CHECK_FALSE(handbrake_held(b::kJump));
		CHECK_FALSE(handbrake_held(b::kJump | b::kSprint | b::kUse | b::kFire));
	}

	TEST_CASE(
			"sim.vehicles.chassis_rules: R5-G1 vectors: spring K and damper, steer angle, TransmissionLoss and engine "
			"brake, rolling resistance, motion damping, wind and down force, anti-roll loads, substeps") {
		CHECK(spring_constant(800.0f, 4, 0.62f, 0.35f) == doctest::Approx(14766.9f).epsilon(1e-4));
		CHECK(spring_constant(5000.0f, 18, 0.5f, 0.4f) == doctest::Approx(13638.9f).epsilon(1e-4));
		CHECK(damper_coefficient(800.0f, 4, 0.09f, 1.0f, 1.0f / 30.0f) * 0.5f == doctest::Approx(270.0f));
		const float standing = damper_coefficient(800.0f, 4, 0.09f, 1.0f, 1.0f / 90.0f);
		const float growler_load = 1.75f * spring_constant(800.0f, 4, 0.62f, 0.35f) * 0.1254f;
		CHECK(standing == doctest::Approx(1620.0f));
		CHECK(damper_clamped_coefficient(standing, growler_load, -2.0f) == standing);
		CHECK(damper_clamped_coefficient(standing, growler_load, 1.0f) == standing);
		CHECK(damper_clamped_coefficient(standing, growler_load, 2.0f) * 2.0f ==
				doctest::Approx(0.8f * growler_load).epsilon(1e-5));
		CHECK(damper_clamped_coefficient(standing, 0.0f, 2.0f) == standing);
		CHECK(chassis_substeps(0.0f, true) == 3u);
		CHECK(chassis_substeps(14.9f / 3.6f, true) == 3u);
		CHECK(chassis_substeps(20.0f / 3.6f, true) == 2u);

		const std::array<CurvePoint, 6> growler{ { { 0.0f, 0.28f }, { 15.0f, 0.28f }, { 30.0f, 0.28f },
				{ 58.0f, 0.28f }, { 66.0f, 0.24f }, { 150.0f, 0.24f } } };
		CHECK(steer_sensitivity(growler, 20.0f / 3.6f, false) == doctest::Approx(-0.28f));
		CHECK(steer_angle(1.0f, steer_sensitivity(growler, 20.0f / 3.6f, false)) ==
				doctest::Approx(0.8796f).epsilon(1e-4));
		CHECK(steer_angle(0.5f, -0.28f) * 180.0f / kSteerHalfTurn == doctest::Approx(25.2f));
		CHECK(steer_sensitivity(growler, 62.0f / 3.6f, false) == doctest::Approx(-0.26f));
		CHECK(steer_sensitivity(growler, 160.0f / 3.6f, false) == doctest::Approx(-0.24f));
		CHECK(steer_sensitivity(growler, 20.0f / 3.6f, true) == doctest::Approx(0.28f));
		const std::array<CurvePoint, 1> steep{ { { 0.0f, 0.7f } } };
		CHECK(steer_sensitivity(steep, 0.0f, false) == doctest::Approx(-0.5f));
		CHECK(steer_angle(1.0f, steer_sensitivity({}, 3.0f, false)) == doctest::Approx(kSteerHalfTurn * 0.5f));
		const std::array<CurvePoint, 2> rising{ { { 0.0f, 0.1f }, { 10.0f, 0.2f } } };
		CHECK(steer_curve_at(rising, 30.0f) == doctest::Approx(0.4f));
		CHECK(steer_curve_at(rising, -5.0f) == doctest::Approx(0.1f));

		const std::array<CurvePoint, 7> m1a2{ { { 0.0f, 0.0f }, { 8.0f, 7.0f }, { 32.0f, 12.0f }, { 45.0f, 35.0f },
				{ 50.0f, 60.0f }, { 58.0f, 100.0f }, { 100.0f, 100.0f } } };
		CHECK(transmission_loss_resistance(m1a2, 10.0f, 1.0f, 5000.0f, 18) == doctest::Approx(10.598f).epsilon(1e-3));
		CHECK(transmission_loss_resistance(m1a2, 10.0f, 1.12f, 5000.0f, 18) == doctest::Approx(5.741f).epsilon(1e-3));
		CHECK(transmission_loss_resistance(m1a2, 10.0f, 0.68f, 5000.0f, 18) == doctest::Approx(55.556f).epsilon(1e-3));
		CHECK(transmission_loss_resistance(m1a2, 8.0f / 3.6f, 1.0f, 5000.0f, 18) ==
				doctest::Approx(3.889f).epsilon(1e-3));
		CHECK(transmission_loss_resistance(m1a2, 0.0f, 1.0f, 5000.0f, 18) == 0.0f);
		CHECK(transmission_loss_resistance({}, 10.0f, 1.0f, 800.0f, 4) == 0.0f);
		const std::array<CurvePoint, 2> line{ { { 10.0f, 10.0f }, { 20.0f, 20.0f } } };
		CHECK(loss_curve_at(line, 0.0f) == doctest::Approx(0.0f));
		CHECK(loss_curve_at(line, 30.0f) == doctest::Approx(30.0f));
		CHECK(engine_brake_resistance(800.0f, 4.0f) == doctest::Approx(7.0f));
		CHECK(engine_brake_resistance(800.0f, 20.0f) == doctest::Approx(5.0f));
		CHECK(wheel_drive_torque(250.0f, 6.0f, 3.8f, 1.0f, 1.0f) == doctest::Approx(106.875f));

		CHECK(rolling_resistance(1.0f, 15.0f, 150.0f, 0.3f, 12.0f, 4) == doctest::Approx(3.24f).epsilon(1e-4));
		CHECK(rolling_resistance(10.0f, 15.0f, 150.0f, 0.3f, 12.0f, 4) == doctest::Approx(27.0f));
		CHECK(rolling_resistance(-10.0f, 15.0f, 150.0f, 0.3f, 12.0f, 4) == doctest::Approx(-27.0f));
		CHECK(rolling_resistance(10.0f, 15.0f, 150.0f, 0.15f, 35.0f, 18) == doctest::Approx(8.75f));
		CHECK(spin_after_resistance(1.0f, -60.0f, 1.0f / 60.0f, 0.5f) == 0.0f);
		CHECK(spin_after_resistance(10.0f, -60.0f, 1.0f / 60.0f, 0.5f) == doctest::Approx(8.0f));
		CHECK(spin_after_resistance(-10.0f, -60.0f, 1.0f / 60.0f, 0.5f) == doctest::Approx(-12.0f));

		const MotionDampingRule damping{ 0.12f, 0.2f, 0.2f, 0.1f, { 0.001f, 0.001f, 0.001f } };
		const std::array<float, 3> tick = motion_damping_angular(damping, 1.0f / 30.0f);
		CHECK(tick[0] == doctest::Approx(0.960789f).epsilon(1e-5));
		CHECK(tick[1] == doctest::Approx(0.935507f).epsilon(1e-5));
		CHECK(tick[2] == doctest::Approx(0.935507f).epsilon(1e-5));
		CHECK(motion_damping_linear(damping, 1.0f / 30.0f)[1] == doctest::Approx(0.9999667f).epsilon(1e-7));
		const std::array<float, 3> half = motion_damping_angular(damping, 1.0f / 60.0f);
		CHECK(half[1] * half[1] == doctest::Approx(tick[1]).epsilon(1e-6));
		CHECK(motion_damping_angular(MotionDampingRule{}, 1.0f / 60.0f)[0] == 1.0f);

		const WindRule tank{ 0.5f, 9.0f, 1.225f, 0.2f, 100.0f, 1.0f, 4.0f };
		CHECK(wind_drag(tank, 15.0f) == doctest::Approx(124.03f).epsilon(1e-3));
		CHECK(wind_drag(tank, 40.0f) == doctest::Approx(1829.0f).epsilon(1e-3));
		CHECK(wind_drag(tank, 60.0f) == doctest::Approx(7938.0f).epsilon(1e-3));
		CHECK(down_force(20.0f, 1.225f, 8.0f, false, 40.0f, 4, 1) == doctest::Approx(196.0f));
		CHECK(down_force(20.0f, 1.225f, 8.0f, true, 40.0f, 4, 1) == doctest::Approx(23520.0f));
		CHECK(down_force(20.0f, 1.225f, 8.0f, true, 40.0f, 4, 4) == doctest::Approx(196.0f));

		const std::array<float, 2> front = anti_roll_loads(3000.0f, 1000.0f, 0.5f);
		CHECK(front[0] == doctest::Approx(4000.0f));
		CHECK(front[1] == 0.0f);
		const std::array<float, 2> rear = anti_roll_loads(2200.0f, 1800.0f, 1.5f);
		CHECK(rear[0] == doctest::Approx(2800.0f));
		CHECK(rear[1] == doctest::Approx(1200.0f));
		const std::array<float, 2> airborne = anti_roll_loads(2000.0f, 0.0f, 10.0f);
		CHECK(airborne[0] == doctest::Approx(12000.0f));
		CHECK(airborne[1] == 0.0f);
	}
}

namespace {

constexpr float kM1a2Load = 5000.0f * 9.82f * 1.6f / 14.0f;
constexpr float kM1a2PhysicsWheelLoad = 5000.0f * 9.82f * 1.6f / 6.0f;
constexpr float kGrowlerFrontLoad = 2782.0f;

TyreCurveRule retail_tyre() {
	return { 5.002381f, 0.053606f, 5.39649f, 0.176533f, false };
}

bf3::sim::VehicleModel m1a2_road_wheel(uint8_t p_wheels) {
	bf3::sim::VehicleModel m;
	m.mass = 5000.0f;
	m.wheel_count = p_wheels;
	bf3::sim::VehicleWheelModel &w = m.wheels[0];
	w.radius = 0.4f;
	w.width = 0.15f;
	w.inertia = 2.0f;
	w.mass = 35.0f;
	w.diff_gear_ratio = 4.88f;
	w.fx_torque_multiplier = 1.0f;
	w.brake_factor = 0.125f;
	w.hand_brake_factor = 25.0f;
	w.rolling_base = 15.0f;
	w.rolling_velocity_kmh = 150.0f;
	w.rolling_resistance = 1;
	w.engine_brake = 1;
	w.low_speed_auto_brake = 1;
	w.driven = 1;
	w.track_steer = 1;
	w.side = 1;
	w.longitudinal_force_max = 10.0f;
	w.lateral_force_max = 2.0f;
	w.lattitude_brake_scale = 1.0f;
	w.slope_grip_min = 40.0f;
	w.slope_grip_max = 50.0f;
	w.slope_grip_exponent = 1;
	w.grip = { 2.85f, 13.75f, 50.0f, 0.125f, false };
	w.tyre = retail_tyre();
	bf3::sim::LandDriveModel &l = m.land;
	const std::array<CurvePoint, 7> loss{ { { 0, 0 }, { 8, 7 }, { 32, 12 }, { 45, 35 }, { 50, 60 }, { 58, 100 },
			{ 100, 100 } } };
	for (size_t i = 0; i < loss.size(); ++i) {
		l.transmission_loss[i] = loss[i];
	}
	l.transmission_loss_points = uint8_t(loss.size());
	l.transmission_efficiency = 0.75f;
	l.track_turn_speed_limit = 2.0f;
	l.track_forward_speed_limit = 1.5f;
	const std::array<CurvePoint, 5> inertia{ { { 0, 50 }, { 15, 50 }, { 22, 35 }, { 30, 18 }, { 50, 1 } } };
	for (size_t i = 0; i < inertia.size(); ++i) {
		l.steer_inertia[i] = inertia[i];
	}
	l.steer_inertia_points = uint8_t(inertia.size());
	m.steer_sensitivity[0] = { 0.0f, 0.3f };
	m.steer_sensitivity[1] = { 20.0f, 0.3f };
	m.steer_sensitivity[2] = { 100.0f, 0.03f };
	m.steer_points = 3;
	return m;
}

bf3::sim::VehicleModel growler_front_wheel() {
	bf3::sim::VehicleModel m;
	m.mass = 800.0f;
	m.wheel_count = 4;
	bf3::sim::VehicleWheelModel &w = m.wheels[0];
	w.radius = 0.4f;
	w.width = 0.3f;
	w.inertia = 2.5f;
	w.mass = 12.0f;
	w.diff_gear_ratio = 3.8f;
	w.fx_torque_multiplier = 1.0f;
	w.rolling_base = 15.0f;
	w.rolling_velocity_kmh = 150.0f;
	w.rolling_resistance = 1;
	w.engine_brake = 1;
	w.low_speed_auto_brake = 1;
	w.driven = 1;
	w.steers = 1;
	w.longitudinal_force_max = 3.5f;
	w.lateral_force_max = 5.0f;
	w.lattitude_brake_scale = 1.0f;
	w.slope_grip_min = 40.0f;
	w.slope_grip_max = 90.0f;
	w.slope_grip_exponent = 1;
	w.grip = { 5700.0f, 5700.0f, 360.0f, 100.0f, false };
	w.tyre = retail_tyre();
	m.land.transmission_efficiency = 1.0f;
	return m;
}

struct ReducedRun {
	float drive = 0, resistance = 0, force = 0, spin = 0, slip_ratio = 0, hull = 0;
};

ReducedRun reduced_substep(const bf3::sim::VehicleModel &p_model, float p_load, uint32_t p_wheels,
		float p_engine_torque, float p_gear_ratio, float p_yaw, float p_hull, bf3::sim::LandWheelState &r_state) {
	bf3::sim::LandSubstep step;
	step.dt = 1.0f / 90.0f;
	step.engine_torque = p_engine_torque;
	step.gear_ratio = p_gear_ratio;
	step.throttle = p_engine_torque != 0.0f ? 1.0f : 0.0f;
	step.yaw = p_yaw;
	step.planar_speed = std::fabs(p_hull);
	bf3::sim::LandWheelContact contact;
	contact.grounded = true;
	contact.load = p_load;
	contact.longitudinal_speed = p_hull;
	const bf3::sim::LandWheelDrive drive = bf3::sim::land_wheel_drive(p_model, 0, step, contact, r_state);
	const bf3::sim::LandWheelForce force =
			bf3::sim::land_wheel_apply(p_model, 0, step, contact, drive, bf3::sim::land_wheel_drives(drive), r_state);
	ReducedRun out;
	out.drive = drive.drive;
	out.resistance = drive.resistance;
	out.force = force.longitudinal;
	out.spin = r_state.spin;
	out.slip_ratio = slip_average_value(r_state.slip_ratio);
	out.hull = p_hull + float(p_wheels) * force.longitudinal * step.dt / p_model.mass;
	return out;
}

}

TEST_SUITE("sim-vehicle-rules-unit") {
	TEST_CASE("sim.vehicles.land_rules: FrictionMethod 0 (R6-G1-tyre vectors): load table, combined slip, blend, "
			  "direction on the M1A2 road wheel and the Growler front wheel") {
		const float m1a2 = tyre_load(kM1a2Load, 5000.0f, 0.0f, 1.0f, 10.0f, 1.0f);
		CHECK(m1a2 == doctest::Approx(5611.4f).epsilon(1e-4));
		CHECK(tyre_load(kM1a2Load, 5000.0f, 0.0f, 1.0f, 0.5f, 1.0f) == doctest::Approx(1402.9f).epsilon(1e-4));
		CHECK(tyre_load(kM1a2Load, 5000.0f, 0.0f, 1.0f, 2.0f, 1.0f) == doctest::Approx(0.625f * 5611.4f).epsilon(1e-4));
		CHECK(tyre_load(kM1a2Load, 5000.0f, 0.0f, 1.0f, 0.5f, 0.98f) == doctest::Approx(5611.4f).epsilon(1e-4));
		const std::array<float, 4> p = tyre_peaks(1200.0f * m1a2 / 5000.0f);
		CHECK(p[0] == doctest::Approx(0.36656f).epsilon(1e-4));
		CHECK(p[1] == doctest::Approx(0.27887f).epsilon(1e-4));
		CHECK(p[2] == doctest::Approx(1.02333f).epsilon(1e-4));
		CHECK(p[3] == doctest::Approx(1.06180f).epsilon(1e-4));
		const TyreCurveRule tyre = retail_tyre();
		const float grip = 0.9f * 2.5f, slide = 0.9f * 1.875f;
		const auto forces = [&](float p_kappa, float p_alpha, float p_mu, float p_load, float p_mass) {
			return tyre_forces(tyre, p_kappa, p_alpha, p_load, p_mass, p_mu, p_mu, 10.0f, 0.0f);
		};
		TyreForces f = forces(0.1f, 0.0f, grip, m1a2, 5000.0f);
		CHECK(f.longitudinal == doctest::Approx(8934.5f).epsilon(2e-4));
		CHECK(f.lateral == 0.0f);
		f = forces(0.0f, 0.1f, grip, m1a2, 5000.0f);
		CHECK(f.longitudinal == 0.0f);
		CHECK(f.lateral == doctest::Approx(9358.1f).epsilon(2e-4));
		f = forces(0.2f, 0.1f, grip, m1a2, 5000.0f);
		CHECK(f.longitudinal == doctest::Approx(10230.8f).epsilon(2e-4));
		CHECK(f.lateral == doctest::Approx(5194.4f).epsilon(2e-4));
		CHECK(forces(1.0f, 0.0f, grip, m1a2, 5000.0f).longitudinal == doctest::Approx(12478.6f).epsilon(2e-4));
		f = forces(-0.1f, -0.05f, slide, m1a2, 5000.0f);
		CHECK(f.longitudinal == doctest::Approx(-6229.7f).epsilon(2e-4));
		CHECK(f.lateral == doctest::Approx(-3333.4f).epsilon(2e-4));

		const std::array<float, 4> g = tyre_peaks(1200.0f * kGrowlerFrontLoad / 800.0f);
		CHECK(g[0] == doctest::Approx(0.43680f).epsilon(1e-4));
		CHECK(g[1] == doctest::Approx(0.32977f).epsilon(1e-4));
		CHECK(g[2] == doctest::Approx(1.02836f).epsilon(1e-4));
		CHECK(g[3] == doctest::Approx(1.07638f).epsilon(1e-4));
		CHECK(forces(0.1f, 0.0f, grip, kGrowlerFrontLoad, 800.0f).longitudinal ==
				doctest::Approx(4199.1f).epsilon(2e-4));
		CHECK(forces(0.0f, 0.1f, grip, kGrowlerFrontLoad, 800.0f).lateral == doctest::Approx(4398.0f).epsilon(2e-4));
		f = forces(0.2f, 0.1f, grip, kGrowlerFrontLoad, 800.0f);
		CHECK(f.longitudinal == doctest::Approx(4943.9f).epsilon(2e-4));
		CHECK(f.lateral == doctest::Approx(2535.0f).epsilon(2e-4));
		f = forces(-0.1f, -0.05f, slide, kGrowlerFrontLoad, 800.0f);
		CHECK(f.longitudinal == doctest::Approx(-2933.3f).epsilon(2e-4));
		CHECK(f.lateral == doctest::Approx(-1591.0f).epsilon(2e-4));
		CHECK(tyre_force_clamp(4199.1f, 3.5f, 800.0f) == doctest::Approx(2800.0f));
		CHECK(tyre_force_clamp(-4199.1f, 3.5f, 800.0f) == doctest::Approx(-2800.0f));
		CHECK(tyre_peaks(9600.0f)[0] == doctest::Approx(0.569186f + 100.0f * 0.0002f * 0.09402996f).epsilon(1e-5));
	}

	TEST_CASE("sim.vehicles.land_rules: slip inputs, the running average, the grip and slide states, friction scale "
			  "curves and the lateral brake blend (R6-G1-tyre rules 1-3)") {
		CHECK(slip_ratio_sample(1.0f, 0.4f, 0.0f) == doctest::Approx(0.04f));
		CHECK(slip_ratio_sample(15.0f, 0.4f, 5.0f) == doctest::Approx(0.2f));
		CHECK(slip_ratio_sample(1000.0f, 0.4f, 0.0f) == doctest::Approx(40.0f));
		CHECK(slip_ratio_sample(9000.0f, 0.4f, 0.0f) == 200.0f);
		CHECK(slip_angle_sample(1.0f, 1.0f) == doctest::Approx(0.0997f).epsilon(1e-3));
		CHECK(slip_divisor(2.0f) == 10.0f);
		CHECK(slip_divisor(-2.5f) == 2.5f);
		SlipAverage a;
		const float feed[] = { 1, 1, 1, 1, 1, 0 };
		const float want[] = { 1, 1, 1, 1, 1, 0.8f };
		for (size_t i = 0; i < 6; ++i) {
			a = slip_average_push(a, feed[i], kSlipRatioSamples);
			CHECK(slip_average_value(a) == doctest::Approx(want[i]));
		}
		CHECK(a.count == 5);
		CHECK(slip_average_value(SlipAverage{}) == 0.0f);
		CHECK(slip_average_push(a, std::nanf(""), kSlipRatioSamples).sum == a.sum);

		const GripRule m1a2{ 2.85f, 13.75f, 50.0f, 0.125f, false };
		CHECK(tyre_grip(m1a2, TyreGrip::Reset, kM1a2Load, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f).state == TyreGrip::Slide);
		CHECK(tyre_grip(m1a2, TyreGrip::Reset, kM1a2Load, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f).friction ==
				doctest::Approx(1.6875f));
		CHECK(tyre_grip(m1a2, TyreGrip::Slide, kM1a2Load, 0.0f, 0.0f, 0.118f, 1.0f, 1.0f, 1.0f).state ==
				TyreGrip::Grip);
		CHECK(tyre_grip(m1a2, TyreGrip::Slide, kM1a2Load, 0.0f, 0.0f, 0.119f, 1.0f, 1.0f, 1.0f).state ==
				TyreGrip::Slide);
		const GripStep held = tyre_grip(m1a2, TyreGrip::Grip, kM1a2Load, 0.0f, 0.0f, 0.157f, 1.0f, 1.0f, 1.0f);
		CHECK(held.state == TyreGrip::Grip);
		CHECK(held.friction == doctest::Approx(2.25f));
		CHECK(tyre_grip(m1a2, TyreGrip::Grip, kM1a2Load, 0.0f, 0.0f, 0.159f, 1.0f, 1.0f, 1.0f).state ==
				TyreGrip::Slide);
		CHECK(tyre_grip(m1a2, TyreGrip::Grip, kM1a2Load, 0.0f, 0.0f, 0.159f, 0.04f, 1.0f, 1.0f).state ==
				TyreGrip::Grip);
		CHECK(tyre_grip(m1a2, TyreGrip::Grip, kM1a2Load, 0.0f, 1.96f, 0.0f, 1.0f, 1.0f, 1.0f).state == TyreGrip::Grip);
		CHECK(tyre_grip(m1a2, TyreGrip::Grip, kM1a2Load, 0.0f, 1.97f, 0.0f, 1.0f, 1.0f, 1.0f).state == TyreGrip::Slide);
		const GripRule rear{ 2.85f, 13.75f, 0.18f, 0.125f, false };
		CHECK(tyre_grip(rear, TyreGrip::Grip, 3699.0f, 0.0f, 0.0070f, 0.0f, 1.0f, 1.0f, 1.0f).state == TyreGrip::Grip);
		CHECK(tyre_grip(rear, TyreGrip::Grip, 3699.0f, 0.0f, 0.0072f, 0.0f, 1.0f, 1.0f, 1.0f).state == TyreGrip::Slide);
		const GripRule always{ 2.85f, 13.75f, 0.18f, 0.125f, true };
		CHECK(tyre_grip(always, TyreGrip::Grip, 3699.0f, 1.0f, 5.0f, 5.0f, 1.0f, 1.0f, 1.0f).state == TyreGrip::Grip);

		CHECK(friction_scale_at({}, 50.0f) == 1.0f);
		const std::array<CurvePoint, 2> scale{ { { 10.0f, 1.0f }, { 50.0f, 0.5f } } };
		CHECK(friction_scale_at(scale, 0.0f) == 1.0f);
		CHECK(friction_scale_at(scale, 30.0f) == doctest::Approx(0.75f));
		CHECK(friction_scale_at(scale, 90.0f) == 0.5f);
		CHECK(tyre_lateral_brake_scale(1.0f, 3.5f, 0.4f) == doctest::Approx(0.4f));
		CHECK(tyre_lateral_brake_scale(0.5f, 3.5f, 0.4f) == doctest::Approx(0.7f));
		CHECK(tyre_lateral_brake_scale(1.0f, 2.9f, 0.4f) == 1.0f);
		CHECK(spin_term_scale(1.0f) == doctest::Approx(1.4f));
		CHECK(spin_term_scale(3.0f) == doctest::Approx(0.7f));
		CHECK(spin_term_scale(5.0f) == 0.0f);
		CHECK(stuck_seconds_step(1.0f, 63.0f, 0.6f, 0.5f) == doctest::Approx(1.5f));
		CHECK(stuck_seconds_step(1.0f, 65.0f, 0.6f, 0.5f) == 0.0f);
		CHECK(stuck_seconds_step(1.0f, 10.0f, 0.4f, 0.5f) == 0.0f);
	}

	TEST_CASE("sim.vehicles.land_rules: the apply step (R6-G1-apply): low-speed scaling, slope grip, the reaction "
			  "torque over 16 x DiffGearRatio, the torque gate and the substep clock") {
		CHECK(tyre_low_speed_scale(0.5f, 0.0f, 0.05f) == doctest::Approx(0.5f));
		CHECK(tyre_low_speed_scale(0.5f, 1.0f, 0.05f) == 1.0f);
		CHECK(tyre_low_speed_scale(0.5f, 0.0f, 0.2f) == 1.0f);
		CHECK(tyre_low_speed_scale(1.5f, 0.0f, 0.0f) == 1.0f);
		const float s45 = std::sin(45.0f * 3.14159265f / 180.0f);
		CHECK(slope_grip_scale(s45, 40.0f, 50.0f, 1) == doctest::Approx(0.5f).epsilon(1e-4));
		CHECK(slope_grip_scale(std::sin(0.8727f), 40.0f, 50.0f, 1) == doctest::Approx(0.0f).epsilon(1e-3));
		CHECK(slope_grip_scale(s45, 40.0f, 90.0f, 1) == doctest::Approx(0.9f).epsilon(1e-4));
		CHECK(slope_grip_scale(s45, 40.0f, 50.0f, 2) == doctest::Approx(0.25f).epsilon(1e-4));
		CHECK(slope_grip_scale(s45, 40.0f, 50.0f, 0) == doctest::Approx(0.5f).epsilon(1e-4));
		CHECK(slope_grip_scale(-s45, 40.0f, 50.0f, 1) == 1.0f);
		CHECK(wheel_reaction_torque(1000.0f, 0.0f, 5000.0f, 18, 0.4f, 1.0f, 4.88f) ==
				doctest::Approx(1000.0f * 0.00512f).epsilon(1e-3));
		CHECK(wheel_reaction_torque(1000.0f, 0.0f, 800.0f, 4, 0.4f, 1.0f, 3.8f) ==
				doctest::Approx(1000.0f * 0.00658f).epsilon(1e-3));
		CHECK(wheel_reaction_torque(1000.0f, 0.0f, 800.0f, 4, 0.4f, 1.85f, 3.8f) ==
				doctest::Approx(1000.0f * 0.01217f).epsilon(1e-3));
		CHECK(wheel_reaction_torque(0.0f, -0.5f, 800.0f, 4, 0.4f, 1.0f, 3.8f) ==
				doctest::Approx(-0.5f * 800.0f * 9.82f / 4.0f * 0.4f / (16.0f * 3.8f)).epsilon(1e-4));
		CHECK(wheel_spin_gate(false, 0.0f) == false);
		CHECK(wheel_spin_gate(true, 0.0f));
		CHECK(wheel_spin_gate(false, 0.02f));
		CHECK(wheel_spin_step(1.0f, 90.0f, 1.0f / 90.0f, 2.0f) == doctest::Approx(1.5f));

		uint8_t clock = 0;
		uint32_t slow = 0, fast = 0;
		for (int t = 0; t < 6; ++t) {
			slow += land_substeps_due(clock, 1.0f);
		}
		clock = 0;
		for (int t = 0; t < 6; ++t) {
			fast += land_substeps_due(clock, 10.0f);
		}
		CHECK(slow == 9);
		CHECK(fast == 6);
		CHECK(land_substep_seconds(1.0f) == doctest::Approx(1.0f / 90.0f));
		CHECK(land_substep_seconds(10.0f) == doctest::Approx(1.0f / 60.0f));
	}

	TEST_CASE("sim.vehicles.land_rules: the shipped wheel step reproduces R6-G1-vectors A (M1A2 throttle 1, six "
			  "substeps), B (pivot, first substeps) and C (Growler throttle, first substeps)") {
		const bf3::sim::VehicleModel tank = m1a2_road_wheel(18);
		bf3::sim::LandWheelState s;
		float hull = 0.0f;
		const float spin_a[] = { 0.431063f, 0.848946f, 1.25570f, 1.65284f, 2.04152f, 2.42173f };
		const float force_a[] = { 0.0f, 374.718f, 676.237f, 923.392f, 1129.17f, 1335.28f };
		const float hull_a[] = { 0.0f, 0.0116579f, 0.0326964f, 0.0614241f, 0.0965539f, 0.138096f };
		for (size_t i = 0; i < 6; ++i) {
			const ReducedRun r = reduced_substep(tank, kM1a2Load, 14, 627.0f, 1.5f, 0.0f, hull, s);
			hull = r.hull;
			CAPTURE(i);
			CHECK(r.drive == doctest::Approx(77.5913f).epsilon(1e-5));
			CHECK(r.force == doctest::Approx(force_a[i]).epsilon(1e-4));
			CHECK(r.spin == doctest::Approx(spin_a[i]).epsilon(1e-4));
			CHECK(hull == doctest::Approx(hull_a[i]).epsilon(1e-4));
			if (i == 1) {
				CHECK(r.resistance == doctest::Approx(-0.452616f).epsilon(1e-4));
				CHECK(r.slip_ratio == doctest::Approx(0.0086213f).epsilon(1e-4));
				CHECK(s.grip == TyreGrip::Grip);
			}
			if (i == 2) {
				CHECK(r.resistance == doctest::Approx(-0.911794f).epsilon(1e-4));
				CHECK(r.slip_ratio == doctest::Approx(0.0166782f).epsilon(1e-4));
			}
		}

		bf3::sim::LandWheelState pivot;
		const ReducedRun p0 = reduced_substep(tank, kM1a2Load, 14, 0.0f, 1.5f, 1.0f, 0.0f, pivot);
		CHECK(pivot.track_steer == doctest::Approx(4.8f));
		CHECK(p0.drive == doctest::Approx(4.8f));
		CHECK(p0.spin == doctest::Approx(0.026667f).epsilon(1e-4));
		CHECK(pivot.grip == TyreGrip::Slide);
		const ReducedRun p1 = reduced_substep(tank, kM1a2Load, 14, 0.0f, 1.5f, 1.0f, 0.0f, pivot);
		CHECK(pivot.track_steer == doctest::Approx(9.504f));
		CHECK(p1.resistance == doctest::Approx(-0.0280f).epsilon(1e-2));
		CHECK(p1.slip_ratio == doctest::Approx(0.000533f).epsilon(1e-3));
		CHECK(pivot.grip == TyreGrip::Grip);
		CHECK(p1.force == doctest::Approx(24.93f).epsilon(1e-3));

		const bf3::sim::VehicleModel jeep = growler_front_wheel();
		bf3::sim::LandWheelState c;
		const ReducedRun c0 = reduced_substep(jeep, kGrowlerFrontLoad, 4, 250.0f, 6.0f, 0.0f, 0.0f, c);
		CHECK(c0.drive == doctest::Approx(106.875f).epsilon(1e-5));
		CHECK(c0.spin == doctest::Approx(0.475f).epsilon(1e-5));
		CHECK(c0.force == 0.0f);
		const ReducedRun c1 = reduced_substep(jeep, kGrowlerFrontLoad, 4, 250.0f, 6.0f, 0.0f, 0.0f, c);
		CHECK(c1.resistance == doctest::Approx(-1.539f).epsilon(1e-3));
		CHECK(c1.slip_ratio == doctest::Approx(0.0095f).epsilon(1e-4));
		CHECK(c1.force == doctest::Approx(198.66f).epsilon(1e-3));
		CHECK(c1.spin == doctest::Approx(0.937351f).epsilon(1e-4));
	}

	TEST_CASE("sim.vehicles.land_rules: the shipped wheel step reproduces R7-G1-vectors A (M1A2 throttle 1 on its 6 "
			  "physics wheels, six substeps, static load 13093.3 N per wheel)") {
		const bf3::sim::VehicleModel tank = m1a2_road_wheel(6);
		bf3::sim::LandWheelState s;
		float hull = 0.0f;
		const float spin_a[] = { 0.431063f, 0.830047f, 1.20250f, 1.55244f, 1.88289f, 2.19420f };
		const float force_a[] = { 0.0f, 862.06f, 1537.22f, 2078.1f, 2519.5f, 2954.3f };
		const float hull_a[] = { 0.0f, 0.0114941f, 0.0319903f, 0.0596983f, 0.0932916f, 0.132682f };
		CHECK(kM1a2PhysicsWheelLoad == doctest::Approx(13093.3f).epsilon(1e-5));
		for (size_t i = 0; i < 6; ++i) {
			const ReducedRun r = reduced_substep(tank, kM1a2PhysicsWheelLoad, 6, 627.0f, 1.5f, 0.0f, hull, s);
			hull = r.hull;
			CAPTURE(i);
			CHECK(r.drive == doctest::Approx(77.5913f).epsilon(1e-5));
			CHECK(r.force == doctest::Approx(force_a[i]).epsilon(1e-4));
			CHECK(r.spin == doctest::Approx(spin_a[i]).epsilon(1e-4));
			CHECK(hull == doctest::Approx(hull_a[i]).epsilon(1e-4));
			if (i == 1) {
				CHECK(r.resistance == doctest::Approx(-1.35785f).epsilon(1e-4));
				CHECK(r.slip_ratio == doctest::Approx(0.0086213f).epsilon(1e-4));
				CHECK(s.grip == TyreGrip::Grip);
			}
			if (i == 2) {
				CHECK(r.resistance == doctest::Approx(-2.67499f).epsilon(1e-4));
				CHECK(r.slip_ratio == doctest::Approx(0.0164317f).epsilon(1e-4));
			}
		}
	}
}
