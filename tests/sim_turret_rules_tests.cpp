#include <doctest/doctest.h>

#include "bf3/core/gameplay/vehicle_def.h"
#include "bf3/sim/vehicles/tracked_rules.h"
#include "bf3/sim/vehicles/turret_rules.h"
#include "bf3/sim/vehicles/vehicle_armament.h"
#include "bf3/sim/vehicles/vehicle_weapon_rules.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace bf3::sim::vehicles;

namespace {

constexpr float kDt = 1.0f / 30.0f;
constexpr float kDeg = 3.14159265358979f / 180.0f;

RotationBodyRule unit_body() {
	RotationBodyRule r;
	r.mass = 1;
	r.inertia = 1;
	r.momentum_multiplier = 1;
	r.force_modifier = 2;
	r.use_linear_input = true;
	r.use_damping = true;
	r.damping_acc = 1.0f;
	r.damping_deacc = 0.9f;
	r.use_threshold = true;
	r.momentum_threshold = 1.0f;
	r.constrained = true;
	r.min_degrees = -30;
	r.max_degrees = 30;
	return r;
}

RotationBodyRule m1a2_turret_yaw(bool p_post) {
	RotationBodyRule r;
	r.mass = 100;
	r.inertia = 100;
	r.momentum_multiplier = 120;
	r.force_modifier = 14000;
	r.momentum_threshold = 120;
	r.use_threshold = true;
	r.use_damping = true;
	r.damping_acc = 0.5f;
	r.damping_deacc = 0.5f;
	r.use_linear_input = true;
	r.post_satisfy = p_post;
	return r;
}

float degrees(float p_radians) {
	return p_radians / kDeg;
}

void tick(const RotationBodyRule &p_rule, RotationBodyState &r_state, float p_level) {
	const RotationLimits limits = rotation_limits(p_rule, 0.0f, false);
	rotation_body_step(p_rule, r_state, rotation_input_torque(p_rule, p_level, r_state.angle), limits, kDt);
}

bf3::core::gameplay::VehicleTurretDef tank_body(int p_parent, int p_axis, const char *p_input, float p_y, float p_z) {
	bf3::core::gameplay::VehicleTurretDef t;
	t.parent = p_parent;
	t.frame_turret = p_parent;
	t.seat = 0;
	t.rotation_axis = p_axis;
	t.rotation_input = p_input;
	t.mass = 100;
	t.inertia = 100;
	t.angular_momentum_multiplier = 120;
	t.force_modifier = 14000;
	t.angular_momentum_threshold = 120;
	t.damping_acc = 0.5f;
	t.damping_deacc = 0.5f;
	t.use_linear_input = true;
	t.use_threshold = true;
	t.use_damping = true;
	t.post_satisfy = true;
	t.frame.trans = { 0.0f, p_y, p_z };
	return t;
}

bf3::core::gameplay::VehicleDef tank_def() {
	bf3::core::gameplay::VehicleDef d;
	d.blueprint = "test/tank";
	d.seats.resize(1);
	d.seats[0].stance_count = 2;
	d.turrets.push_back(tank_body(-1, 1, "EIARoll", 1.5f, 0.0f));
	bf3::core::gameplay::VehicleTurretDef gun = tank_body(0, 0, "EIAPitch", 0.5f, 1.9f);
	gun.constrained = true;
	gun.min_angle = -20;
	gun.max_angle = 10;
	gun.world_space_lock_efficiency = 1.0f;
	d.turrets.push_back(gun);
	bf3::core::gameplay::VehicleWeaponDef cannon;
	cannon.firing = "test/cannon";
	cannon.seat = 0;
	cannon.turret = 1;
	cannon.frame_turret = 1;
	cannon.projectile_spawn_offset = { 0.0f, 0.0f, 5.0f };
	cannon.stances = { 0 };
	d.weapons.push_back(cannon);
	return d;
}

bf3::core::Vec3 forward_of(const bf3::core::Transform &p_t) {
	return p_t.forward;
}

constexpr std::array<InputCurvePoint, 7> kTankRoll{ { { 0.0f, 0.0f }, { 0.15f, 0.0f }, { 0.25f, 0.015f },
		{ 0.75f, 0.03f }, { 0.875f, 0.066f }, { 0.95f, 0.1f }, { 1.0f, 0.1f } } };

}

TEST_SUITE("sim-turret-rules-unit") {
	TEST_CASE("sim.vehicles.turret_rules: R2-G2 TV1 torque, damping, threshold and midpoint step") {
		const RotationBodyRule r = unit_body();
		RotationBodyState s;
		CHECK(rotation_input_torque(r, 1.0f, 0.0f) == doctest::Approx(-2.0f));
		std::vector<RotationBodyState> after;
		for (int t = 0; t < 18; ++t) {
			tick(r, s, t < 12 ? 1.0f : 0.0f);
			after.push_back(s);
		}
		CHECK(after[0].momentum == doctest::Approx(-0.06667f).epsilon(1e-3));
		CHECK(degrees(after[0].angle) == doctest::Approx(-0.0637f).epsilon(1e-3));
		CHECK(after[1].momentum == doctest::Approx(-0.13333f).epsilon(1e-3));
		CHECK(degrees(after[1].angle) == doctest::Approx(-0.2546f).epsilon(1e-3));
		CHECK(after[11].momentum == doctest::Approx(-0.8f).epsilon(1e-3));
		CHECK(degrees(after[11].angle) == doctest::Approx(-9.1673f).epsilon(1e-3));
		CHECK(after[12].momentum == doctest::Approx(-0.72f).epsilon(1e-3));
		CHECK(degrees(after[12].angle) == doctest::Approx(-10.5424f).epsilon(1e-3));
		CHECK(after[17].momentum == doctest::Approx(-0.42515f).epsilon(1e-3));
		CHECK(degrees(after[17].angle) == doctest::Approx(-15.6105f).epsilon(1e-3));
	}

	TEST_CASE("sim.vehicles.turret_rules: R2-G2 TV2 signed square input is a quarter of the torque") {
		RotationBodyRule r = unit_body();
		r.use_linear_input = false;
		CHECK(rotation_input_torque(r, 0.5f, 0.0f) == doctest::Approx(-0.5f));
		CHECK(rotation_input_torque(r, -0.5f, 0.0f) == doctest::Approx(0.5f));
		RotationBodyState s;
		for (int t = 0; t < 4; ++t) {
			tick(r, s, 0.5f);
		}
		CHECK(s.momentum == doctest::Approx(-0.06667f).epsilon(1e-3));
		CHECK(degrees(s.angle) == doctest::Approx(-0.2546f).epsilon(1e-3));
	}

	TEST_CASE("sim.vehicles.turret_rules: R2-G2 TV3 reset spring below ResetForceInputThreshold") {
		RotationBodyRule r = unit_body();
		r.constrained = false;
		r.reset_force_modifier = -3.0f;
		r.reset_input_threshold = 0.1f;
		RotationBodyState s{ -20.0f * kDeg, 0.0f };
		CHECK(rotation_input_torque(r, 0.0f, s.angle) == doctest::Approx(1.0472f).epsilon(1e-3));
		CHECK(rotation_input_torque(r, 0.5f, s.angle) == doctest::Approx(-1.0f));
		std::vector<float> angles;
		for (int t = 0; t < 6; ++t) {
			tick(r, s, 0.0f);
			angles.push_back(degrees(s.angle));
		}
		CHECK(angles[0] == doctest::Approx(-19.9667f).epsilon(1e-4));
		CHECK(angles[5] == doctest::Approx(-18.8086f).epsilon(1e-4));
	}

	TEST_CASE("sim.vehicles.turret_rules: R2-G2 TV4 a stop snaps to the limit and kills the momentum") {
		const RotationBodyRule r = unit_body();
		RotationBodyState s{ -29.9f * kDeg, -0.9f };
		tick(r, s, 1.0f);
		CHECK(degrees(s.angle) == doctest::Approx(-30.0f));
		CHECK(s.momentum == 0.0f);
		RotationBodyState up{ 29.95f * kDeg, 0.9f };
		tick(r, up, -1.0f);
		CHECK(degrees(up.angle) == doctest::Approx(30.0f));
		CHECK(up.momentum == 0.0f);
		RotationBodyRule open = r;
		open.constrained = false;
		RotationBodyState free{ -29.9f * kDeg, -0.9f };
		tick(open, free, 1.0f);
		CHECK(degrees(free.angle) < -30.0f);
		CHECK(free.momentum < 0.0f);
	}

	TEST_CASE("sim.vehicles.turret_rules: R2-G2 TV5 extended limits blend over the falloff") {
		RotationBodyRule r;
		r.min_degrees = -10;
		r.max_degrees = 20;
		r.extended = { true, 180.0f, 90.0f, 20.0f, -5.0f, 5.0f };
		CHECK(degrees(rotation_limits(r, 150.0f, true).max) == doctest::Approx(5.0f));
		CHECK(degrees(rotation_limits(r, 120.0f, true).max) == doctest::Approx(20.0f));
		CHECK(degrees(rotation_limits(r, 130.0f, true).max) == doctest::Approx(16.25f));
		CHECK(degrees(rotation_limits(r, 130.0f, false).max) == doctest::Approx(20.0f));
		RotationBodyRule wide;
		wide.min_degrees = -400;
		wide.max_degrees = -5;
		const RotationLimits clamped = rotation_limits(wide, 0.0f, false);
		CHECK(clamped.min == doctest::Approx(-3.14159265f));
		CHECK(clamped.max == 0.0f);
	}

	TEST_CASE("sim.vehicles.turret_rules: R2-G2 TV6 the position lock casts on the fifth input-free tick") {
		int32_t c = 0;
		CHECK_FALSE(position_lock_tick(c, 0.4f));
		CHECK(c == kPositionLockWaitTicks);
		std::vector<int32_t> counters;
		bool cast = false;
		for (int t = 0; t < 5 && !cast; ++t) {
			cast = position_lock_tick(c, 0.0f);
			counters.push_back(cast ? 0 : c);
		}
		CHECK(cast);
		CHECK(counters == std::vector<int32_t>{ 4, 3, 2, 1, 0 });
		CHECK(c == -1);
		CHECK_FALSE(position_lock_tick(c, 0.0f));
		CHECK_FALSE(position_lock_tick(c, 0.2f));
		CHECK(c == kPositionLockWaitTicks);
	}

	TEST_CASE("sim.vehicles.turret_rules: world and position locks rotate directly, clamped and wrapped (R2-G2 A5)") {
		RotationBodyRule r = unit_body();
		const RotationLimits limits = rotation_limits(r, 0.0f, false);
		CHECK(rotation_direct(r, 0.1f, 0.00005f, limits) == 0.1f);
		CHECK(rotation_direct(r, 0.1f, 1.0f, limits) == doctest::Approx(30.0f * kDeg));
		CHECK(rotation_direct(r, 40.0f * kDeg, -0.1f, limits) == doctest::Approx(30.0f * kDeg - 0.1f));
		r.constrained = false;
		CHECK(rotation_direct(r, 3.0f, 0.5f, limits) == doctest::Approx(3.5f - 2.0f * 3.14159265f));
		CHECK(world_lock_rotation(0.85f, 0.2f) == doctest::Approx(-0.17f));
		CHECK(world_lock_rotation(0.0f, 0.2f) == 0.0f);
		CHECK(wrap_angle(3.14159265f) == doctest::Approx(3.14159265f));
		CHECK(wrap_angle(-3.2f) == doctest::Approx(-3.2f + 2.0f * 3.14159265f));
	}

	TEST_CASE("sim.vehicles.turret_rules: R2-G2 A2 alignment input and stop tolerance") {
		CHECK(alignment_input(30.0f * kDeg, 0.35f) == doctest::Approx(0.18326f).epsilon(1e-3));
		CHECK(alignment_input(10.0f, 0.35f) == 1.0f);
		CHECK(alignment_input(-10.0f, 0.35f) == -1.0f);
		CHECK(alignment_done(0.9f * kDeg, 1.0f));
		CHECK_FALSE(alignment_done(1.1f * kDeg, 1.0f));
	}

	TEST_CASE("sim.vehicles.turret_rules: R3-G2 TV1-TV3 the M1A2 turret yaw (pre mode as the notes computed it)") {
		const RotationBodyRule r = m1a2_turret_yaw(false);
		CHECK(rotation_input_torque(r, 0.1f, 0.0f) == doctest::Approx(-1680.0f));
		RotationBodyState s;
		std::vector<float> momentum;
		for (int t = 0; t < 8; ++t) {
			tick(r, s, 0.1f);
			momentum.push_back(s.momentum);
		}
		CHECK(momentum[0] == doctest::Approx(-56.0f));
		CHECK(momentum[1] == doctest::Approx(-84.0f));
		CHECK(momentum[2] == doctest::Approx(-98.0f));
		CHECK(momentum[3] == doctest::Approx(-105.0f));
		CHECK(degrees(s.angle) == doctest::Approx(-10.70f).epsilon(1e-3));
		const float before = s.angle;
		tick(r, s, 0.0f);
		CHECK(s.momentum == doctest::Approx(-55.78f).epsilon(1e-3));
		tick(r, s, 0.0f);
		tick(r, s, 0.0f);
		CHECK(degrees(s.angle - before) == doctest::Approx(-1.864f).epsilon(1e-3));
		CHECK(rotation_steady_rate(r, 0.1f, kDt) == doctest::Approx(0.84f).epsilon(1e-4));
		CHECK(degrees(rotation_steady_rate(r, 0.015f, kDt)) == doctest::Approx(7.2f).epsilon(3e-3));
		CHECK(rotation_steady_rate(r, 1.0f, kDt) == doctest::Approx(4.0f).epsilon(1e-4));
	}

	TEST_CASE("sim.vehicles.turret_rules: the data's post mode clamps then damps after the step") {
		const RotationBodyRule r = m1a2_turret_yaw(true);
		RotationBodyState s;
		std::vector<float> momentum;
		for (int t = 0; t < 4; ++t) {
			tick(r, s, 0.1f);
			momentum.push_back(s.momentum);
		}
		CHECK(momentum[0] == doctest::Approx(-28.0f));
		CHECK(momentum[1] == doctest::Approx(-42.0f));
		CHECK(momentum[2] == doctest::Approx(-49.0f));
		CHECK(momentum[3] == doctest::Approx(-52.5f));
		CHECK(rotation_steady_rate(r, 0.1f, kDt) == doctest::Approx(0.84f).epsilon(1e-4));
		CHECK(rotation_steady_rate(r, 1.0f, kDt) == doctest::Approx(3.4f).epsilon(1e-4));
	}

	TEST_CASE("sim.vehicles.turret_rules: R3-G2-turret-input TV1-TV6 radial InputCurves") {
		CHECK(input_curve(kTankRoll, 0.10f) == 0.0f);
		CHECK(input_curve(kTankRoll, 0.50f) == doctest::Approx(0.0225f));
		CHECK(input_curve(kTankRoll, -0.90f) == doctest::Approx(-0.07733f).epsilon(1e-3));
		CHECK(input_curve(kTankRoll, 1.0f) == doctest::Approx(0.1f));
		CHECK(input_curve(std::span<const InputCurvePoint>(kTankRoll.data(), 1), 0.5f) == 0.5f);
		const std::array<InputCurvePoint, 2> offset{ { { 0.2f, 0.1f }, { 1.0f, 0.9f } } };
		CHECK(input_curve(offset, 0.1f) == doctest::Approx(0.0f));
		std::array<float, 2> both{ 0.6f, 0.8f };
		shape_curve_inputs(kTankRoll, both, false);
		CHECK(both[0] == doctest::Approx(0.06f));
		CHECK(both[1] == doctest::Approx(0.08f));
		std::array<float, 2> square{ 0.6f, 0.8f };
		shape_curve_inputs(kTankRoll, square, true);
		const float f = input_curve(kTankRoll, 0.8f) / 0.8f;
		CHECK(square[0] == doctest::Approx(0.6f * f));
		CHECK(square[1] == doctest::Approx(0.8f * f));
		std::array<float, 1> idle{ 0.0f };
		shape_curve_inputs(kTankRoll, idle, false);
		CHECK(idle[0] == 0.0f);
	}

	TEST_CASE("sim.vehicles.tracked_rules: R-G1 tracked steering drive, attenuation and smoothing") {
		CHECK(track_steer_wheel(2, 1));
		CHECK_FALSE(track_steer_wheel(2, 0));
		CHECK_FALSE(track_steer_wheel(1, 1));
		CHECK(track_steer_target(1.0f, 1.0f, 0.0f, 1.0f) == doctest::Approx(800.0f));
		CHECK(track_steer_target(0.5f, -1.0f, 0.1f, 1.0f) == doctest::Approx(-400.0f));
		CHECK(track_steer_target(1.0f, 1.0f, 0.2f, 1.0f) == doctest::Approx(800.0f * 0.6f));
		CHECK(track_steer_target(1.0f, 1.0f, 0.3f, 1.0f) == 0.0f);
		CHECK(track_speed_attenuation(1.5f, 0.0f) == 1.0f);
		CHECK(track_speed_attenuation(1.5f, 1.0f) == doctest::Approx(0.25f));
		CHECK(track_speed_attenuation(2.0f, -0.5f) == doctest::Approx(0.5f));
		CHECK(track_speed_attenuation(2.0f, 3.0f) == 0.0f);
		const std::array<std::array<float, 2>, 2> curve{ { { 0.0f, 4.0f }, { 36.0f, 0.5f } } };
		const TrackSteerRule rule{ 2.0f, 1.5f, curve };
		CHECK(track_steer_inertia(rule, 0.0f) == doctest::Approx(4.0f));
		CHECK(track_steer_inertia(rule, 5.0f) == doctest::Approx(2.25f));
		CHECK(track_steer_inertia(rule, 20.0f) == 1.0f);
		CHECK(track_steer_inertia(TrackSteerRule{}, 3.0f) == 1.0f);
		CHECK(track_steer_smooth(0.0f, 800.0f, 4.0f) == doctest::Approx(200.0f));
		CHECK(track_steer_smooth(200.0f, 800.0f, 4.0f) == doctest::Approx(350.0f));
		CHECK(track_steer_smooth(350.0f, 100.0f, 4.0f) == doctest::Approx(100.0f));
		CHECK(track_steer_smooth(350.0f, -800.0f, 4.0f) == doctest::Approx(-200.0f));
		CHECK(track_steer_smooth(0.0f, 0.005f, 4.0f) == doctest::Approx(0.005f));
	}

	TEST_CASE("sim.vehicles.armament: the turret and gun follow the seat's aim at the body's rate, stop at the gun "
			  "limits, and the gun's world lock holds its elevation while the hull pitches") {
		const bf3::core::Result<VehicleArmament> built = build_vehicle_armament(tank_def());
		REQUIRE_MESSAGE(built.ok(), (built.ok() ? std::string() : built.error().message));
		const VehicleArmament &a = *built;
		REQUIRE(a.turrets.size() == 2);
		CHECK(a.turrets[0].aim == TurretAimAxis::Yaw);
		CHECK(a.turrets[1].aim == TurretAimAxis::Pitch);
		REQUIRE(a.weapons.size() == 1);
		CHECK(weapon_in_stance(a.weapons[0], 0));
		CHECK_FALSE(weapon_in_stance(a.weapons[0], 1));
		CHECK(a.seat_stances == std::vector<uint8_t>{ 2 });

		const bf3::core::Transform hull = hull_transform({ 0.0f, 0.0f, 0.0f, 1.0f }, { 10.0f, 0.0f, 20.0f });
		ArmamentState s;
		const bf3::core::Vec3 rest_muzzle = weapon_muzzle(a, s, hull, 0);
		CHECK(rest_muzzle[0] == doctest::Approx(10.0f));
		CHECK(rest_muzzle[1] == doctest::Approx(2.0f));
		CHECK(rest_muzzle[2] == doctest::Approx(26.9f));

		const float yaw = 90.0f * kDeg, pitch = 10.0f * kDeg;
		const SeatAim aim{ true,
			{ std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch) } };
		const float rate = rotation_steady_rate(a.turrets[0].rule, 1.0f, kTurretStepSeconds);
		int ticks = 0;
		float fastest = 0.0f;
		for (; ticks < 60; ++ticks) {
			const float before = s.bodies[0].angle;
			step_armament(a, s, hull, std::span<const SeatAim>(&aim, 1));
			fastest = std::max(fastest, std::fabs(s.bodies[0].angle - before) / kTurretStepSeconds);
			if (std::fabs(s.bodies[0].angle - yaw) < 0.2f * kDeg &&
					std::fabs(s.bodies[1].angle + pitch) < 0.2f * kDeg) {
				break;
			}
		}
		std::printf("armament: aligned 90 deg / 10 deg in %d steps (%.2f s), fastest %.1f deg/s, cap %.1f deg/s\n",
				ticks + 1, double(float(ticks + 1) * kTurretStepSeconds), double(fastest / kDeg), double(rate / kDeg));
		CHECK(ticks < 60);
		CHECK(fastest <= rate * 1.001f);
		CHECK(float(ticks + 1) * kTurretStepSeconds >= yaw / rate);
		const bf3::core::Vec3 dir = forward_of(weapon_world(a, s, hull, 0));
		CHECK(dir[0] == doctest::Approx(aim.direction[0]).epsilon(0.01));
		CHECK(dir[1] == doctest::Approx(aim.direction[1]).epsilon(0.01));
		CHECK(dir[2] == doctest::Approx(aim.direction[2]).epsilon(0.01));

		const SeatAim high{ true, { 0.0f, std::sin(40.0f * kDeg), std::cos(40.0f * kDeg) } };
		for (int t = 0; t < 90; ++t) {
			step_armament(a, s, hull, std::span<const SeatAim>(&high, 1));
		}
		CHECK(degrees(s.bodies[1].angle) == doctest::Approx(-20.0f));
		CHECK(std::fabs(s.bodies[0].angle) < 0.2f * kDeg);

		const SeatAim level{ true, { 0.0f, 0.0f, 1.0f } };
		ArmamentState locked;
		for (int t = 0; t < 30; ++t) {
			step_armament(a, locked, hull, std::span<const SeatAim>(&level, 1));
		}
		const SeatAim idle{};
		const float nose_up = -5.0f * kDeg;
		const bf3::core::Transform pitched = hull_transform(
				{ std::sin(nose_up / 2.0f), 0.0f, 0.0f, std::cos(nose_up / 2.0f) }, { 10.0f, 0.0f, 20.0f });
		step_armament(a, locked, pitched, std::span<const SeatAim>(&idle, 1));
		const bf3::core::Vec3 held = forward_of(weapon_world(a, locked, pitched, 0));
		std::printf("armament: hull pitched 5 deg nose up, gun angle %.2f deg, muzzle direction y %.4f\n",
				double(degrees(locked.bodies[1].angle)), double(held[1]));
		CHECK(degrees(locked.bodies[1].angle) == doctest::Approx(5.0f).epsilon(0.02));
		CHECK(std::fabs(held[1]) < 0.01f);

		bf3::core::gameplay::VehicleDef broken = tank_def();
		broken.turrets[0].parent = 1;
		CHECK_FALSE(build_vehicle_armament(broken).ok());
		broken = tank_def();
		broken.weapons[0].seat = 3;
		CHECK_FALSE(build_vehicle_armament(broken).ok());
	}

	TEST_CASE("sim.vehicles.weapon_rules: R2-G2 TV7-TV9 reload and heat multipliers, muzzles and the recoil impulse") {
		const float reload = vehicle_reload_seconds(5.0f, 0.8f);
		CHECK(reload == doctest::Approx(4.0f));
		CHECK(reload * 0.6f == doctest::Approx(2.4f));
		CHECK(vehicle_heat_drop_per_second(0.3f, 0.5f) == doctest::Approx(0.15f));
		CHECK(vehicle_heat_drop_per_second(0.3f, 0.5f) * kDt == doctest::Approx(0.005f));

		RecoilImpulseState s;
		std::vector<int> impulses;
		std::vector<int> muzzles;
		for (int t = 0; t < 8; ++t) {
			recoil_impulse_begin(s);
			if (t == 0 || t == 3 || t == 6) {
				muzzles.push_back(s.muzzle);
				recoil_impulse_shot(s, 2);
			}
			impulses.push_back(recoil_impulse_due(s, 50000.0f) ? 1 : 0);
		}
		CHECK(muzzles == std::vector<int>{ 0, 1, 0 });
		CHECK(impulses == std::vector<int>{ 0, 1, 0, 0, 1, 0, 0, 1 });

		RecoilImpulseState back;
		recoil_impulse_begin(back);
		recoil_impulse_shot(back, 1);
		recoil_impulse_begin(back);
		recoil_impulse_shot(back, 1);
		CHECK_FALSE(recoil_impulse_due(back, 50000.0f));
		recoil_impulse_begin(back);
		CHECK(recoil_impulse_due(back, 50000.0f));
		CHECK_FALSE(recoil_impulse_due(back, 0.0f));
		CHECK(back.muzzle == 0);
	}
}
