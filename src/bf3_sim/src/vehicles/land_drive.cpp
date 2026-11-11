#include "land_drive.h"

#include "bf3/sim/vehicles/land_rules.h"

#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>

#include <algorithm>
#include <cmath>

namespace bf3::sim {

namespace {

bool channels_swapped(const vehicles::GearboxState &p_gearbox) {
	return p_gearbox.gear < 0 || (p_gearbox.gear == 0 && p_gearbox.reverse_direction != 0);
}

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
	const JPH::Vec3 forward_axis = bi.GetRotation(v.body) * JPH::Vec3::sAxisZ();
	const float forward_speed = velocity.Dot(forward_axis);
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
	const float ratio = land_gear_ratio(land, gear);
	const float wheel_brake = std::max(d.brake_filter, brake_axis);
	JPH::WheeledVehicleController *controller =
			static_cast<JPH::WheeledVehicleController *>(v.constraint->GetController());
	for (size_t i = 0; i < m.wheel_count; ++i) {
		const VehicleWheelModel &wm = m.wheels[i];
		JPH::Wheel *wheel = v.constraint->GetWheel(JPH::uint(i));
		float omega = wheel->GetAngularVelocity();
		if (wm.driven != 0) {
			omega += vehicles::wheel_drive_torque(
							 engine_torque, ratio, wm.diff_gear_ratio, land.transmission_efficiency, land.drive_share) *
					dt / wm.inertia;
		}
		omega = vehicles::brake_scale(omega, wm.brake_factor, wheel_brake);
		omega = vehicles::brake_scale(omega, wm.hand_brake_factor, hand_brake ? 1.0f : 0.0f);
		wheel->SetAngularVelocity(omega);
	}

	float jolt_brake = wheel_brake;
	if (m.stand_still_brake && m.stand_still_ticks > 0) {
		if (forward_pedal == 0.0f && back_pedal == 0.0f && speed < m.stand_still_speed) {
			if (d.stand_still_ticks < m.stand_still_ticks) {
				++d.stand_still_ticks;
			}
		} else {
			d.stand_still_ticks = 0;
		}
		if (d.stand_still_ticks >= m.stand_still_ticks) {
			jolt_brake = 1.0f;
		}
	}
	const bool idle = forward_pedal == 0.0f && back_pedal == 0.0f;
	if (idle && (speed < m.stand_still_speed || (p_command.flags & vehicle_flags::kDriven) == 0)) {
		jolt_brake = 1.0f;
	}

	const float lock = vehicle_max_steer_angle(m, 0.0f);
	const float falloff = lock > 0.0f ? vehicle_max_steer_angle(m, forward_speed) / lock : 0.0f;
	const float right = d.steer_filter * falloff;

	if (p_command.throttle != 0 || p_command.brake != 0 || p_command.steer != 0 || hand_brake) {
		bi.ActivateBody(v.body);
	}
	controller->GetTransmission().Set(0, 0.0f);
	controller->SetDriverInput(0.0f, right, jolt_brake, hand_brake ? 1.0f : 0.0f);
	if (m.drag_coefficient > 0.0f && speed > 0.0f) {
		bi.AddForce(v.body, velocity * (-m.drag_coefficient * speed), JPH::EActivation::DontActivate);
	}
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
