#include "bf3/sim/vehicles/land_wheel.h"

#include "bf3/sim/vehicles/chassis_rules.h"
#include "bf3/sim/vehicles/land_rules.h"
#include "bf3/sim/vehicles/tracked_rules.h"

#include <algorithm>
#include <cmath>

namespace bf3::sim {

namespace {

// INFERRED: the differential's wheel pair list is empty in retail, so every drive share is 1 (notes R5-G1-1 checker
// found no code that fills it). Settle: a note that finds the pair builder.
constexpr float kUnpairedDriveShare = 1.0f;

float against(float p_speed) {
	return p_speed >= 0.0f ? 1.0f : -1.0f;
}

std::span<const vehicles::CurvePoint> curve(
		const std::array<vehicles::CurvePoint, kMaxModelCurvePoints> &p_points, uint8_t p_count) {
	return std::span<const vehicles::CurvePoint>(p_points.data(), p_count);
}

// SOURCE: notes R5-G1-5 and R6-G1-apply 6 (each track wheel takes the forward term and its side's turn term).
// INFERRED: each wheel smooths its own turn term, faded by its own averaged slip ratio. Settle: a note on that state.
float track_drive(const VehicleModel &p_model, const VehicleWheelModel &p_wheel, const LandSubstep &p_step,
		LandWheelState &r_state) {
	const LandDriveModel &land = p_model.land;
	const vehicles::TrackSteerRule rule{ land.track_turn_speed_limit, land.track_forward_speed_limit,
		curve(land.steer_inertia, land.steer_inertia_points) };
	const float sensitivity = vehicles::steer_curve_at(
			curve(p_model.steer_sensitivity, p_model.steer_points), p_step.planar_speed * vehicles::kKmhPerMs);
	const float target = vehicles::track_turn_term(p_step.yaw, p_step.gear_ratio, p_step.throttle, p_wheel.side == 1,
								 p_model.steer_inverted) *
			vehicles::track_slip_fade(vehicles::slip_average_value(r_state.slip_ratio)) * sensitivity;
	r_state.track_steer = vehicles::track_steer_smooth(
			r_state.track_steer, target, vehicles::track_steer_inertia(rule, p_step.planar_speed));
	const float forward = vehicles::track_forward_term(p_step.engine_torque, p_step.gear_ratio) *
			vehicles::track_speed_attenuation(rule.forward_speed_limit, p_step.angular_speed);
	return forward +
			r_state.track_steer * vehicles::track_speed_attenuation(rule.turn_speed_limit, p_step.angular_speed);
}

}

LandWheelDrive land_wheel_drive(const VehicleModel &p_model, size_t p_wheel, const LandSubstep &p_step,
		const LandWheelContact &p_contact, LandWheelState &r_state) {
	const VehicleWheelModel &w = p_model.wheels[p_wheel];
	const LandDriveModel &land = p_model.land;
	if (p_contact.grounded) {
		r_state.slip_ratio = vehicles::slip_average_push(r_state.slip_ratio,
				vehicles::slip_ratio_sample(r_state.spin, w.radius, p_contact.longitudinal_speed),
				vehicles::kSlipRatioSamples);
		r_state.slip_angle = vehicles::slip_average_push(r_state.slip_angle,
				vehicles::slip_angle_sample(p_contact.lateral_speed, p_contact.longitudinal_speed),
				vehicles::kSlipAngleSamples);
	}
	LandWheelDrive out;
	if (w.track_steer != 0 && w.side < 2) {
		out.drive = track_drive(p_model, w, p_step, r_state);
	} else if (w.driven != 0) {
		out.drive = vehicles::wheel_drive_torque(p_step.engine_torque, p_step.gear_ratio, w.diff_gear_ratio,
				land.transmission_efficiency, kUnpairedDriveShare);
	}
	if (w.rolling_resistance != 0) {
		out.resistance -= vehicles::rolling_resistance(
				r_state.spin, w.rolling_base, w.rolling_velocity_kmh, w.width, w.mass, p_model.wheel_count);
	}
	// STAND-IN: no boost or crawl input exists, so the TransmissionLoss strength is 1 (R5-G1-1 3). Settle: build both keys.
	if (w.driven != 0 && p_contact.grounded) {
		out.resistance -= against(p_contact.longitudinal_speed) *
				vehicles::transmission_loss_resistance(curve(land.transmission_loss, land.transmission_loss_points),
						p_step.planar_speed, 1.0f, p_model.mass, p_model.wheel_count);
		if (w.engine_brake != 0 && out.drive == 0.0f && p_contact.longitudinal_speed != 0.0f) {
			out.resistance -= against(p_contact.longitudinal_speed) *
					vehicles::engine_brake_resistance(p_model.mass, p_step.planar_speed);
		}
	}
	r_state.spin = vehicles::brake_scale(r_state.spin, w.brake_factor, p_step.brake);
	r_state.spin = vehicles::brake_scale(r_state.spin, w.hand_brake_factor, p_step.hand_brake);
	if (w.low_speed_auto_brake != 0 && w.driven != 0 && p_contact.grounded &&
			p_step.planar_speed < vehicles::kLowSpeedAutoBrakeSpeed && out.drive == 0.0f) {
		r_state.spin = 0.0f;
	}
	return out;
}

bool land_wheel_drives(const LandWheelDrive &p_drive) {
	return std::fabs(p_drive.drive) > vehicles::kSpinGateTorque;
}

// SOURCE: notes R6-G1-apply 2 to 5 (airborne reset, tyre force, low-speed scaling, clamps, slope grip, reaction gate).
LandWheelForce land_wheel_apply(const VehicleModel &p_model, size_t p_wheel, const LandSubstep &p_step,
		const LandWheelContact &p_contact, const LandWheelDrive &p_drive, bool p_any_drive, LandWheelState &r_state) {
	const VehicleWheelModel &w = p_model.wheels[p_wheel];
	const float torque = p_drive.drive + p_drive.resistance;
	LandWheelForce out;
	// STAND-IN: R6-G1-apply 2a's 0.1 drive factor for an airborne wheel under a vehicle level (under water, inferred) is
	// not applied. Settle: trace that wheel flag.
	if (!p_contact.grounded) {
		r_state.grip = vehicles::TyreGrip::Reset;
		r_state.spin = vehicles::wheel_spin_step(r_state.spin, torque, p_step.dt, w.inertia);
		return out;
	}
	const float slip_ratio = vehicles::slip_average_value(r_state.slip_ratio);
	const float slip_angle = vehicles::slip_average_value(r_state.slip_angle);
	// STAND-IN: the ground material's StaticFrictionModifier and DynamicFrictionModifier are 1 (R6-G1-tyre 2).
	// Settle: read the contact material's MaterialPropertyPhysicsData.
	const vehicles::GripStep grip = vehicles::tyre_grip(w.grip, r_state.grip, p_contact.load, slip_angle,
			p_contact.lateral_speed, slip_ratio, r_state.spin, 1.0f, 1.0f);
	r_state.grip = grip.state;
	const float kmh = p_step.planar_speed * vehicles::kKmhPerMs;
	const float longitudinal_scale =
			vehicles::friction_scale_at(curve(w.longitude_scale, w.longitude_scale_points), kmh) * grip.friction;
	const float lateral_scale = vehicles::friction_scale_at(curve(w.lattitude_scale, w.lattitude_scale_points), kmh) *
			grip.friction *
			vehicles::tyre_lateral_brake_scale(p_step.brake, p_contact.longitudinal_speed, w.lattitude_brake_scale);
	const float tyre_load = vehicles::tyre_load(p_contact.load, p_model.mass, w.constant_spring_force_friction_scale,
			p_contact.up_dot_normal, p_step.planar_speed, p_step.right_horizontal_length);
	const vehicles::TyreForces f = vehicles::tyre_forces(w.tyre, slip_ratio, slip_angle, tyre_load, p_model.mass,
			longitudinal_scale, lateral_scale, p_step.planar_speed, p_step.stuck_seconds);
	const float low = vehicles::tyre_low_speed_scale(p_step.planar_speed, p_drive.drive, p_step.angular_speed);
	const float longitudinal = vehicles::tyre_force_clamp(f.longitudinal * low, w.longitudinal_force_max, p_model.mass);
	out.lateral = vehicles::tyre_force_clamp(f.lateral * low, w.lateral_force_max, p_model.mass);
	out.longitudinal = longitudinal;
	if (std::fabs(r_state.spin) > vehicles::kAxisEpsilon) {
		out.longitudinal *= vehicles::slope_grip_scale((r_state.spin > 0.0f ? 1.0f : -1.0f) * p_contact.drive_axis_up,
				w.slope_grip_min, w.slope_grip_max, w.slope_grip_exponent);
	}
	out.applied = true;
	if (vehicles::wheel_spin_gate(p_any_drive, p_step.planar_speed)) {
		const float reaction = vehicles::wheel_reaction_torque(longitudinal, p_step.forward_up, p_model.mass,
				p_model.wheel_count, w.radius, w.fx_torque_multiplier, w.diff_gear_ratio);
		r_state.spin = vehicles::wheel_spin_step(r_state.spin, torque - reaction, p_step.dt, w.inertia);
	}
	return out;
}

}
