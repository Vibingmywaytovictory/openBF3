#include "land_drive.h"

#include "../jolt_convert.h"

#include "bf3/sim/vehicles/chassis_rules.h"
#include "bf3/sim/vehicles/land_rules.h"
#include "bf3/sim/vehicles/land_wheel.h"
#include "bf3/sim/vehicles/tyre_rules.h"

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace bf3::sim {

namespace {

constexpr float kLockedWheelTorque = std::numeric_limits<float>::max() * kTickSeconds * kTickSeconds;

bool channels_swapped(const vehicles::GearboxState &p_gearbox) {
	return p_gearbox.gear < 0 || (p_gearbox.gear == 0 && p_gearbox.reverse_direction != 0);
}

bool track_steered(const VehicleModel &p_model) {
	for (size_t i = 0; i < p_model.wheel_count; ++i) {
		if (p_model.wheels[i].track_steer != 0) {
			return true;
		}
	}
	return false;
}

JPH::Vec3 on_plane(const JPH::Vec3 &p_axis, const JPH::Vec3 &p_normal) {
	const JPH::Vec3 projected = p_axis - p_normal * p_axis.Dot(p_normal);
	const float length = projected.Length();
	return length > vehicles::kAxisEpsilon ? projected / length : JPH::Vec3::sZero();
}

LandWheelState wheel_state(const VehicleTyreState &p_tyre, float p_spin) {
	LandWheelState s;
	s.spin = p_spin;
	s.slip_ratio = { p_tyre.slip_ratio_sum, p_tyre.slip_ratio_samples };
	s.slip_angle = { p_tyre.slip_angle_sum, p_tyre.slip_angle_samples };
	s.grip = vehicles::TyreGrip(p_tyre.grip);
	s.track_steer = p_tyre.track_steer;
	return s;
}

void store_wheel_state(const LandWheelState &p_state, VehicleTyreState &r_tyre) {
	r_tyre.slip_ratio_sum = p_state.slip_ratio.sum;
	r_tyre.slip_ratio_samples = p_state.slip_ratio.count;
	r_tyre.slip_angle_sum = p_state.slip_angle.sum;
	r_tyre.slip_angle_samples = p_state.slip_angle.count;
	r_tyre.grip = uint8_t(p_state.grip);
	r_tyre.track_steer = p_state.track_steer;
}

void anti_roll_bars(const VehicleModel &p_model, std::array<float, kMaxVehicleWheels> &r_loads) {
	if (p_model.wheel_count <= 2 || (p_model.anti_roll_front == 0.0f && p_model.anti_roll_rear == 0.0f)) {
		return;
	}
	std::array<uint8_t, kMaxVehicleWheels> order{};
	for (uint8_t i = 0; i < p_model.wheel_count; ++i) {
		order[i] = i;
	}
	std::stable_sort(order.begin(), order.begin() + p_model.wheel_count, [&](uint8_t p_a, uint8_t p_b) {
		return p_model.wheels[p_a].data_centre[2] > p_model.wheels[p_b].data_centre[2];
	});
	const auto bar = [&](uint8_t p_a, uint8_t p_b, float p_stiffness) {
		if (p_stiffness == 0.0f ||
				!(std::fabs(p_model.wheels[p_a].data_centre[2] - p_model.wheels[p_b].data_centre[2]) <
						vehicles::kAntiRollSameAxle)) {
			return;
		}
		const std::array<float, 2> barred = vehicles::anti_roll_loads(r_loads[p_a], r_loads[p_b], p_stiffness);
		r_loads[p_a] = barred[0];
		r_loads[p_b] = barred[1];
	};
	bar(order[0], order[1], p_model.anti_roll_front);
	bar(order[p_model.wheel_count - 2u], order[p_model.wheel_count - 1u], p_model.anti_roll_rear);
}

struct WheelGeometry {
	bool grounded = false;
	JPH::RVec3 contact;
	JPH::RVec3 applied;
	JPH::Vec3 normal, longitudinal, lateral, suspension;
	JPH::Vec3 ground_velocity;
	float compression = 0;
};
}

JPH::SpringSettings land_spring(const VehicleModel &p_model, const VehicleWheelModel &p_wheel, float p_speed,
		float p_compression, float p_extension_speed) {
	const uint32_t substeps = vehicles::chassis_substeps(p_speed, true);
	const float ratio = p_wheel.suspension_max > 0.0f ? p_compression / p_wheel.suspension_max : 0.0f;
	const float progressive = vehicles::spring_progressive(
			ratio, p_wheel.progressive_start, p_wheel.progressive_exponent, vehicles::kProgressiveSetting);
	const float stiffness = vehicles::spring_substep_factor(substeps) * p_wheel.spring_constant * progressive;
	const float damping = vehicles::damper_coefficient(p_model.mass, p_model.wheel_count, p_wheel.spring_damping,
			progressive, 1.0f / (vehicles::kChassisTickHz * float(substeps)));
	return JPH::SpringSettings(JPH::ESpringMode::StiffnessAndDamping, stiffness,
			vehicles::damper_clamped_coefficient(damping, stiffness * p_compression, p_extension_speed));
}

VehicleDriverState land_driver_at_rest(const VehicleModel &p_model) {
	VehicleDriverState d;
	d.engine_rpm = p_model.land.engine.rpm_min;
	return d;
}

void land_drive(VehicleWorld::Impl &r_world, VehicleSlot p_slot, const VehicleCommand &p_command) {
	VehicleWorld::Impl::Vehicle &v = r_world.vehicles[p_slot];
	const VehicleModel &m = *v.model;
	const LandDriveModel &land = m.land;
	VehicleDriverState &d = v.driver;
	const float dt = kTickSeconds;

	const float throttle_axis = float(std::max<int>(p_command.throttle, -127)) / 127.0f;
	const float brake_axis = float(p_command.brake) / 255.0f;
	const float steer_axis = float(std::max<int>(p_command.steer, -127)) / 127.0f;
	const bool hand_brake = (p_command.buttons & vehicle_buttons::kHandbrake) != 0;
	const float forward_pedal = std::max(throttle_axis, 0.0f);
	const float back_pedal = std::max(-throttle_axis, 0.0f);

	const bool swapped = channels_swapped(d.gearbox);
	const float throttle_in =
			vehicles::shape_deadzone(swapped ? back_pedal : forward_pedal, land.throttle.deadzone, false);
	const float brake_in = vehicles::shape_deadzone(swapped ? forward_pedal : back_pedal, land.brake.deadzone, false);
	const float steer_in = vehicles::shape_deadzone(steer_axis, land.yaw.deadzone, true);
	d.throttle_filter = vehicles::shape_inertia(d.throttle_filter, throttle_in, dt, land.throttle);
	d.brake_filter = vehicles::shape_inertia(d.brake_filter, brake_in, dt, land.brake);
	d.steer_filter = std::clamp(vehicles::shape_inertia(d.steer_filter, steer_in, dt, land.yaw), -1.0f, 1.0f);
	d.reversing = swapped ? 1 : 0;

	JPH::BodyInterface &bi = r_world.system->GetBodyInterfaceNoLock();
	const JPH::Vec3 velocity = bi.GetLinearVelocity(v.body);
	const JPH::Quat rotation = bi.GetRotation(v.body);
	const float speed = velocity.Length();

	const vehicles::GearboxRule rule = land_gearbox_rule(land);
	vehicles::gearbox_accept(
			rule, d.gearbox, vehicles::gearbox_request(rule, d.gearbox, d.throttle_filter, d.brake_filter, speed));
	vehicles::gearbox_advance(rule, d.gearbox, dt);

	const int32_t gear = d.gearbox.gear;
	const bool backward_limited = land.limit_backward_throttle && gear < 0 && speed >= land.backward_throttle_limit;
	const float engine_torque = backward_limited
			? 0.0f
			: vehicles::engine_torque(d.engine_rpm, std::span<const float>(land.rpm_points.data(), land.curve_points),
					  std::span<const float>(land.torque_points.data(), land.curve_points), land.power_multiplier,
					  d.throttle_filter, d.gearbox.clutch);
	d.stuck_seconds = vehicles::stuck_seconds_step(d.stuck_seconds, speed * speed, d.throttle_filter, dt);

	const bool tracked = track_steered(m);
	const float steer_max = vehicles::steer_angle(1.0f, vehicle_steer_sensitivity(m, speed));
	const bool idle = forward_pedal == 0.0f && back_pedal == 0.0f && (!tracked || steer_axis == 0.0f);
	const bool held = idle && (p_command.flags & vehicle_flags::kDriven) == 0;
	bool stand_still = false;
	if (m.stand_still_brake && m.stand_still_ticks > 0) {
		if (idle && speed < m.stand_still_speed) {
			d.stand_still_ticks = uint16_t(std::min<uint32_t>(d.stand_still_ticks + 1u, m.stand_still_ticks));
		} else {
			d.stand_still_ticks = 0;
		}
		stand_still = d.stand_still_ticks >= m.stand_still_ticks;
	}
	const bool hold = held || stand_still;

	v.tick = { engine_torque, land_gear_ratio(land, gear), d.throttle_filter, -d.steer_filter,
		std::max(d.brake_filter, brake_axis), hand_brake ? 1.0f : 0.0f, hold };
	v.jolt_grip = hold ? 1.0f : 0.0f;

	for (size_t i = 0; i < m.wheel_count; ++i) {
		const VehicleWheelModel &wm = m.wheels[i];
		JPH::Wheel *wheel = v.constraint->GetWheel(JPH::uint(i));
		JPH::WheelSettingsWV &settings = *v.wheel_settings[i];
		const bool grounded = wheel->HasContact();
		const float extension = grounded
				? -(bi.GetPointVelocity(v.body, wheel->GetContactPosition()) -
						  bi.GetPointVelocity(wheel->GetContactBodyID(), wheel->GetContactPosition()))
						   .Dot(rotation * settings.mSuspensionDirection)
				: 0.0f;
		settings.mSuspensionSpring = land_spring(
				m, wm, speed, grounded ? wm.suspension_max - wheel->GetSuspensionLength() : 0.0f, extension);
		settings.mMaxSteerAngle = wm.steers != 0 ? steer_max : 0.0f;
		settings.mMaxBrakeTorque = hold ? kLockedWheelTorque : 0.0f;
		if (hold) {
			wheel->SetAngularVelocity(0.0f);
		}
	}

	if (p_command.throttle != 0 || p_command.brake != 0 || p_command.steer != 0 || hand_brake) {
		bi.ActivateBody(v.body);
	}
	JPH::WheeledVehicleController *controller =
			static_cast<JPH::WheeledVehicleController *>(v.constraint->GetController());
	controller->GetTransmission().Set(0, 0.0f);
	controller->SetDriverInput(0.0f, d.steer_filter, hold ? 1.0f : 0.0f, hand_brake ? 1.0f : 0.0f);
	if (m.wind_resistance && speed > 0.0f) {
		bi.AddForce(v.body, velocity * (-vehicles::wind_drag(m.wind, speed) / speed), JPH::EActivation::DontActivate);
	}
	if (m.down_force) {
		uint32_t grounded_wheels = 0;
		for (size_t i = 0; i < m.wheel_count; ++i) {
			grounded_wheels += v.constraint->GetWheel(JPH::uint(i))->HasContact() ? 1u : 0u;
		}
		const float down = vehicles::down_force(speed, m.wind.air_density, m.down_force_base, m.down_force_wheel_factor,
				m.down_force_per_wheel, m.wheel_count, grounded_wheels);
		bi.AddForce(v.body, JPH::Vec3(0.0f, -down, 0.0f), JPH::EActivation::DontActivate);
	}
}

void land_wheel_pass(VehicleWorld::Impl &r_world, VehicleSlot p_slot) {
	VehicleWorld::Impl::Vehicle &v = r_world.vehicles[p_slot];
	if (!v.used || v.tick.hold) {
		return;
	}
	JPH::Body &body = *v.constraint->GetVehicleBody();
	if (!body.IsActive()) {
		return;
	}
	const VehicleModel &m = *v.model;
	VehicleDriverState &d = v.driver;
	const JPH::Quat rotation = body.GetRotation();
	const JPH::Vec3 up_axis = rotation * JPH::Vec3::sAxisY();
	const JPH::Vec3 right_axis = rotation * JPH::Vec3::sAxisX();
	const JPH::Vec3 forward_axis = rotation * JPH::Vec3::sAxisZ();
	const float start_speed = body.GetLinearVelocity().Length();
	const uint32_t substeps = vehicles::land_substeps_due(d.substep_clock, start_speed);
	const float dt = vehicles::land_substep_seconds(start_speed);
	const JPH::Vec3 handling = rotation * -to_jolt(m.center_of_mass_handling_offset);

	std::array<WheelGeometry, kMaxVehicleWheels> geometry{};
	std::array<LandWheelState, kMaxVehicleWheels> states{};
	for (size_t i = 0; i < m.wheel_count; ++i) {
		const JPH::Wheel *wheel = v.constraint->GetWheel(JPH::uint(i));
		states[i] = wheel_state(d.tyres[i], wheel->GetAngularVelocity());
		WheelGeometry &g = geometry[i];
		g.grounded = wheel->HasContact();
		if (!g.grounded) {
			continue;
		}
		JPH::Vec3 forward, up, right;
		v.constraint->GetWheelLocalBasis(wheel, forward, up, right);
		g.contact = wheel->GetContactPosition();
		g.applied = g.contact + handling;
		g.normal = wheel->GetContactNormal();
		g.longitudinal = on_plane(rotation * forward, g.normal);
		g.lateral = on_plane(rotation * right, g.normal);
		g.suspension = rotation * v.wheel_settings[i]->mSuspensionDirection;
		g.ground_velocity = wheel->GetContactPointVelocity();
		g.compression = m.wheels[i].suspension_max - wheel->GetSuspensionLength();
	}

	for (uint32_t s = 0; s < substeps; ++s) {
		const JPH::Vec3 velocity = body.GetLinearVelocity();
		LandSubstep step;
		step.dt = dt;
		step.engine_torque = v.tick.engine_torque;
		step.gear_ratio = v.tick.gear_ratio;
		step.throttle = v.tick.throttle;
		step.yaw = v.tick.yaw;
		step.brake = v.tick.brake;
		step.hand_brake = v.tick.hand_brake;
		step.planar_speed = (velocity - up_axis * velocity.Dot(up_axis)).Length();
		step.angular_speed = body.GetAngularVelocity().Length();
		step.right_horizontal_length = std::hypot(right_axis.GetX(), right_axis.GetZ());
		step.forward_up = forward_axis.GetY();
		step.stuck_seconds = d.stuck_seconds;

		std::array<LandWheelContact, kMaxVehicleWheels> contacts{};
		std::array<float, kMaxVehicleWheels> loads{};
		for (size_t i = 0; i < m.wheel_count; ++i) {
			const WheelGeometry &g = geometry[i];
			LandWheelContact &c = contacts[i];
			c.grounded = g.grounded;
			if (!g.grounded) {
				continue;
			}
			const JPH::Vec3 relative = body.GetPointVelocity(g.contact) - g.ground_velocity;
			c.longitudinal_speed = relative.Dot(g.longitudinal);
			c.lateral_speed = relative.Dot(g.lateral);
			c.up_dot_normal = up_axis.Dot(g.normal);
			c.drive_axis_up = g.longitudinal.GetY();
			const float extension = -relative.Dot(g.suspension);
			const JPH::SpringSettings spring = land_spring(m, m.wheels[i], start_speed, g.compression, extension);
			loads[i] = std::max(spring.mStiffness * g.compression - spring.mDamping * extension, 0.0f);
		}
		anti_roll_bars(m, loads);

		std::array<LandWheelDrive, kMaxVehicleWheels> drives{};
		bool any_drive = false;
		for (size_t i = 0; i < m.wheel_count; ++i) {
			contacts[i].load = loads[i];
			drives[i] = land_wheel_drive(m, i, step, contacts[i], states[i]);
			any_drive |= land_wheel_drives(drives[i]);
		}
		std::array<JPH::Vec3, kMaxVehicleWheels> impulses{};
		std::array<bool, kMaxVehicleWheels> applied{};
		for (size_t i = 0; i < m.wheel_count; ++i) {
			const LandWheelForce f = land_wheel_apply(m, i, step, contacts[i], drives[i], any_drive, states[i]);
			applied[i] = f.applied;
			impulses[i] = (geometry[i].longitudinal * f.longitudinal - geometry[i].lateral * f.lateral) * dt;
		}
		for (size_t i = 0; i < m.wheel_count; ++i) {
			if (applied[i]) {
				body.AddImpulse(impulses[i], geometry[i].applied);
			}
		}
	}

	for (size_t i = 0; i < m.wheel_count; ++i) {
		v.constraint->GetWheel(JPH::uint(i))->SetAngularVelocity(states[i].spin);
		store_wheel_state(states[i], d.tyres[i]);
	}
}

void land_motion_damping(VehicleWorld::Impl &r_world, VehicleSlot p_slot) {
	VehicleWorld::Impl::Vehicle &v = r_world.vehicles[p_slot];
	const VehicleModel &m = *v.model;
	JPH::BodyInterface &bi = r_world.system->GetBodyInterfaceNoLock();
	if (!bi.IsActive(v.body)) {
		return;
	}
	const std::array<float, 3> angular = vehicles::motion_damping_angular(m.motion_damping, kTickSeconds);
	const std::array<float, 3> linear = vehicles::motion_damping_linear(m.motion_damping, kTickSeconds);
	const JPH::Quat rotation = bi.GetRotation(v.body);
	const std::array<JPH::Vec3, 3> axes{ rotation * JPH::Vec3::sAxisX(), rotation * JPH::Vec3::sAxisY(),
		rotation * JPH::Vec3::sAxisZ() };
	JPH::Vec3 w = bi.GetAngularVelocity(v.body);
	JPH::Vec3 u = bi.GetLinearVelocity(v.body);
	for (size_t a = 0; a < 3; ++a) {
		w -= (1.0f - angular[a]) * w.Dot(axes[a]) * axes[a];
		u -= (1.0f - linear[a]) * u.Dot(axes[a]) * axes[a];
	}
	bi.SetLinearAndAngularVelocity(v.body, u, w);
}

void land_engine_update(VehicleWorld::Impl &r_world, VehicleSlot p_slot) {
	VehicleWorld::Impl::Vehicle &v = r_world.vehicles[p_slot];
	const VehicleModel &m = *v.model;
	const LandDriveModel &land = m.land;
	VehicleDriverState &d = v.driver;
	const float ratio = land_gear_ratio(land, d.gearbox.gear);
	float wheel_rpm_sum = 0.0f;
	uint32_t driven = 0;
	bool grounded = false;
	for (size_t i = 0; i < m.wheel_count; ++i) {
		const VehicleWheelModel &wm = m.wheels[i];
		if (wm.driven == 0) {
			continue;
		}
		const JPH::Wheel *wheel = v.constraint->GetWheel(JPH::uint(i));
		const float omega = wheel->GetAngularVelocity();
		wheel_rpm_sum += vehicles::wheel_rpm(
				std::span<const float>(&omega, 1), ratio, wm.diff_gear_ratio, land.transmission_efficiency);
		grounded |= wheel->HasContact();
		++driven;
	}
	const float wheel_rpm = driven > 0 ? wheel_rpm_sum / float(driven) : 0.0f;
	const vehicles::GearboxRule rule = land_gearbox_rule(land);
	const float target = vehicles::engine_rpm_target(land.engine, d.gearbox.gear != 0,
			vehicles::gearbox_clutch_pressed(rule, d.gearbox), d.gearbox.clutch, d.throttle_filter, wheel_rpm);
	float load = 0.0f;
	if (grounded) {
		const JPH::BodyInterface &bi = r_world.system->GetBodyInterfaceNoLock();
		const JPH::Vec3 forward_axis = bi.GetRotation(v.body) * JPH::Vec3::sAxisZ();
		load = vehicles::engine_load(
				wheel_rpm, d.engine_rpm, forward_axis.GetY(), bi.GetLinearVelocity(v.body).Dot(forward_axis));
	}
	d.engine_rpm = vehicles::engine_rpm_step(
			land.engine, d.engine_rpm, vehicles::engine_rpm_desired(target, load, 0.0f), 0.0f, kTickSeconds);
}

}
