#pragma once

#include "bf3/sim/vehicle_model.h"
#include "bf3/sim/vehicles/tyre_rules.h"

#include <cstddef>

namespace bf3::sim {

struct LandWheelState {
	float spin = 0;
	vehicles::SlipAverage slip_ratio, slip_angle;
	vehicles::TyreGrip grip = vehicles::TyreGrip::Reset;
	float track_steer = 0;
};

struct LandWheelContact {
	bool grounded = false;
	float load = 0;
	float longitudinal_speed = 0, lateral_speed = 0;
	float up_dot_normal = 1;
	float drive_axis_up = 0;
};

struct LandSubstep {
	float dt = 0;
	float engine_torque = 0, gear_ratio = 0;
	float throttle = 0, yaw = 0, brake = 0, hand_brake = 0;
	float planar_speed = 0, angular_speed = 0;
	float right_horizontal_length = 1, forward_up = 0;
	float stuck_seconds = 0;
};

struct LandWheelDrive {
	float drive = 0;
	float resistance = 0;
};

struct LandWheelForce {
	bool applied = false;
	float longitudinal = 0;
	float lateral = 0;
};

LandWheelDrive land_wheel_drive(const VehicleModel &p_model, size_t p_wheel, const LandSubstep &p_step,
		const LandWheelContact &p_contact, LandWheelState &r_state);

bool land_wheel_drives(const LandWheelDrive &p_drive);

LandWheelForce land_wheel_apply(const VehicleModel &p_model, size_t p_wheel, const LandSubstep &p_step,
		const LandWheelContact &p_contact, const LandWheelDrive &p_drive, bool p_any_drive, LandWheelState &r_state);

}
