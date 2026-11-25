#include "land_drive.h"

#include "bf3/sim/vehicles/chassis_rules.h"
#include "bf3/sim/vehicles/land_rules.h"
#include "bf3/sim/vehicles/tracked_rules.h"

#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace bf3::sim {

namespace {

constexpr float kLockedWheelTorque = std::numeric_limits<float>::max() * kTickSeconds * kTickSeconds;
constexpr float kUnpairedDriveShare = 1.0f;
constexpr float kJoltSlipSpeedFloor = 1.0e-3f;

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

float signed_unit(float p_value) {
	return p_value >= 0.0f ? 1.0f : -1.0f;
}

float contact_speed(const JPH::BodyInterface &p_bi, JPH::BodyID p_body, const JPH::Wheel &p_wheel) {
	if (!p_wheel.HasContact()) {
		return 0.0f;
	}
	const JPH::Vec3 longitudinal = p_wheel.GetContactNormal().Cross(p_wheel.GetContactLateral());
	const JPH::Vec3 relative = p_bi.GetPointVelocity(p_body, p_wheel.GetContactPosition());
	return relative.Dot(longitudinal);
}

float wheel_slip(float p_rim_speed, float p_contact_speed) {
	const float ratio = (p_rim_speed - p_contact_speed) / std::max(std::fabs(p_contact_speed), kJoltSlipSpeedFloor);
	return ratio / (1.0f + std::fabs(ratio));
}

void anti_roll_bars(VehicleWorld::Impl::Vehicle &r_vehicle) {
	const VehicleModel &m = *r_vehicle.model;
	r_vehicle.tyre_load_scale.fill(1.0f);
	if (m.wheel_count <= 2 || (m.anti_roll_front == 0.0f && m.anti_roll_rear == 0.0f)) {
		return;
	}
	std::array<uint8_t, kMaxVehicleWheels> order{};
	for (uint8_t i = 0; i < m.wheel_count; ++i) {
		order[i] = i;
	}
	std::stable_sort(order.begin(), order.begin() + m.wheel_count,
			[&](uint8_t p_a, uint8_t p_b) { return m.wheels[p_a].data_centre[2] > m.wheels[p_b].data_centre[2]; });
	const auto bar = [&](uint8_t p_a, uint8_t p_b, float p_stiffness) {
		if (p_stiffness == 0.0f ||
				!(std::fabs(m.wheels[p_a].data_centre[2] - m.wheels[p_b].data_centre[2]) <
						vehicles::kAntiRollSameAxle)) {
			return;
		}
		const JPH::Wheel *a = r_vehicle.constraint->GetWheel(p_a);
		const JPH::Wheel *b = r_vehicle.constraint->GetWheel(p_b);
		const float load_a = a->HasContact() ? std::max(a->GetSuspensionLambda(), 0.0f) : 0.0f;
		const float load_b = b->HasContact() ? std::max(b->GetSuspensionLambda(), 0.0f) : 0.0f;
		const std::array<float, 2> barred = vehicles::anti_roll_loads(load_a, load_b, p_stiffness);
		r_vehicle.tyre_load_scale[p_a] = load_a > 0.0f ? barred[0] / load_a : 1.0f;
		r_vehicle.tyre_load_scale[p_b] = load_b > 0.0f ? barred[1] / load_b : 1.0f;
	};
	bar(order[0], order[1], m.anti_roll_front);
	bar(order[m.wheel_count - 2u], order[m.wheel_count - 1u], m.anti_roll_rear);
}

std::span<const vehicles::CurvePoint> steer_curve(const VehicleModel &p_model) {
	return std::span<const vehicles::CurvePoint>(p_model.steer_sensitivity.data(), p_model.steer_points);
}

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
	const float yaw = -d.steer_filter;

	JPH::BodyInterface &bi = r_world.system->GetBodyInterfaceNoLock();
	const JPH::Vec3 velocity = bi.GetLinearVelocity(v.body);
	const JPH::Quat rotation = bi.GetRotation(v.body);
	const JPH::Vec3 up_axis = rotation * JPH::Vec3::sAxisY();
	const float speed = velocity.Length();
	const float planar_speed = (velocity - up_axis * velocity.Dot(up_axis)).Length();
	const float spin_rate = bi.GetAngularVelocity(v.body).Length();

	anti_roll_bars(v);

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

	const bool tracked = track_steered(m);
	float track_forward = 0.0f;
	float turn_attenuation = 1.0f;
	if (tracked) {
		const vehicles::TrackSteerRule track{ land.track_turn_speed_limit, land.track_forward_speed_limit,
			std::span<const std::array<float, 2>>(land.steer_inertia.data(), land.steer_inertia_points) };
		std::array<float, 2> slip{};
		std::array<uint32_t, 2> grounded{};
		for (size_t i = 0; i < m.wheel_count; ++i) {
			const VehicleWheelModel &wm = m.wheels[i];
			const JPH::Wheel *wheel = v.constraint->GetWheel(JPH::uint(i));
			if (wm.track_steer == 0 || wm.side > 1 || !wheel->HasContact()) {
				continue;
			}
			slip[wm.side] +=
					std::fabs(wheel_slip(wheel->GetAngularVelocity() * wm.radius, contact_speed(bi, v.body, *wheel)));
			++grounded[wm.side];
		}
		const float inertia = vehicles::track_steer_inertia(track, planar_speed);
		const float sensitivity = vehicles::steer_curve_at(steer_curve(m), planar_speed * vehicles::kKmhPerMs);
		for (uint8_t side = 0; side < 2; ++side) {
			const float side_slip = grounded[side] > 0 ? slip[side] / float(grounded[side]) : 0.0f;
			const float target = vehicles::track_turn_term(yaw, ratio, d.throttle_filter, side == 1, m.steer_inverted) *
					vehicles::track_slip_fade(side_slip) * sensitivity;
			d.track_steer[side] = vehicles::track_steer_smooth(d.track_steer[side], target, inertia);
		}
		turn_attenuation = vehicles::track_speed_attenuation(track.turn_speed_limit, spin_rate);
		track_forward = vehicles::track_forward_term(engine_torque, ratio) *
				vehicles::track_speed_attenuation(track.forward_speed_limit, spin_rate);
	}

	const float steer_max = vehicles::steer_angle(1.0f, vehicle_steer_sensitivity(m, speed));
	const float wheel_brake = std::max(d.brake_filter, brake_axis);
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
	const std::span<const vehicles::CurvePoint> loss(land.transmission_loss.data(), land.transmission_loss_points);
	bool any_locked = false;
	uint32_t grounded_wheels = 0;
	for (size_t i = 0; i < m.wheel_count; ++i) {
		const VehicleWheelModel &wm = m.wheels[i];
		JPH::Wheel *wheel = v.constraint->GetWheel(JPH::uint(i));
		JPH::WheelSettingsWV &settings = *v.wheel_settings[i];
		const bool grounded = wheel->HasContact();
		grounded_wheels += grounded ? 1u : 0u;
		const float extension = grounded
				? -(bi.GetPointVelocity(v.body, wheel->GetContactPosition()) -
						  bi.GetPointVelocity(wheel->GetContactBodyID(), wheel->GetContactPosition()))
						   .Dot(rotation * settings.mSuspensionDirection)
				: 0.0f;
		settings.mSuspensionSpring = land_spring(
				m, wm, speed, grounded ? wm.suspension_max - wheel->GetSuspensionLength() : 0.0f, extension);
		settings.mMaxSteerAngle = wm.steers != 0 ? steer_max : 0.0f;

		float drive = 0.0f;
		if (wm.track_steer != 0 && wm.side < 2) {
			drive = track_forward + d.track_steer[wm.side] * turn_attenuation;
		} else if (wm.driven != 0) {
			drive = vehicles::wheel_drive_torque(
					engine_torque, ratio, wm.diff_gear_ratio, land.transmission_efficiency, kUnpairedDriveShare);
		}
		float omega = wheel->GetAngularVelocity();
		const float along = contact_speed(bi, v.body, *wheel);
		float resistance = 0.0f;
		if (wm.rolling_resistance != 0) {
			resistance -= vehicles::rolling_resistance(
					omega, wm.rolling_base, wm.rolling_velocity_kmh, wm.width, wm.mass, m.wheel_list);
		}
		if (wm.driven != 0 && grounded) {
			resistance -= signed_unit(along) *
					vehicles::transmission_loss_resistance(loss, planar_speed, 1.0f, m.mass, m.wheel_list);
			if (wm.engine_brake != 0 && drive == 0.0f && along != 0.0f) {
				resistance -= signed_unit(along) * vehicles::engine_brake_resistance(m.mass, planar_speed);
			}
		}
		const float applied = land.wheel_torque_scale * dt;
		omega += drive * applied / wm.inertia;
		omega = vehicles::spin_after_resistance(omega, resistance, applied, wm.inertia);
		const bool brake_locks = vehicles::brake_scale(1.0f, wm.brake_factor, wheel_brake) == 0.0f ||
				vehicles::brake_scale(1.0f, wm.hand_brake_factor, hand_brake ? 1.0f : 0.0f) == 0.0f;
		omega = vehicles::brake_scale(omega, wm.brake_factor, wheel_brake);
		omega = vehicles::brake_scale(omega, wm.hand_brake_factor, hand_brake ? 1.0f : 0.0f);
		const bool auto_brake = wm.low_speed_auto_brake != 0 && wm.driven != 0 && grounded &&
				planar_speed < vehicles::kLowSpeedAutoBrakeSpeed && drive == 0.0f;
		const bool locked = auto_brake || held || brake_locks || stand_still;
		if (locked) {
			omega = 0.0f;
		}
		any_locked |= locked;
		settings.mMaxBrakeTorque = locked ? kLockedWheelTorque : 0.0f;
		wheel->SetAngularVelocity(omega);
	}

	if (p_command.throttle != 0 || p_command.brake != 0 || p_command.steer != 0 || hand_brake) {
		bi.ActivateBody(v.body);
	}
	JPH::WheeledVehicleController *controller =
			static_cast<JPH::WheeledVehicleController *>(v.constraint->GetController());
	controller->GetTransmission().Set(0, 0.0f);
	controller->SetDriverInput(0.0f, d.steer_filter, any_locked ? 1.0f : 0.0f, hand_brake ? 1.0f : 0.0f);
	if (m.wind_resistance && speed > 0.0f) {
		bi.AddForce(v.body, velocity * (-vehicles::wind_drag(m.wind, speed) / speed), JPH::EActivation::DontActivate);
	}
	if (m.down_force) {
		const float down = vehicles::down_force(speed, m.wind.air_density, m.down_force_base, m.down_force_wheel_factor,
				m.down_force_per_wheel, m.wheel_count, grounded_wheels);
		bi.AddForce(v.body, JPH::Vec3(0.0f, -down, 0.0f), JPH::EActivation::DontActivate);
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
