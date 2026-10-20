#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::gameplay {

inline constexpr size_t kMaxDriveWheels = 16;
inline constexpr size_t kMaxDriveCurvePoints = 32;
inline constexpr size_t kMaxDriveGears = 16;
inline constexpr size_t kMaxDriveRigidBodies = 16;
inline constexpr size_t kMaxDriveEngines = 4;

using DriveVec3 = std::array<float, 3>;

struct DriveTransform {
	DriveVec3 right{ 1, 0, 0 };
	DriveVec3 up{ 0, 1, 0 };
	DriveVec3 forward{ 0, 0, 1 };
	DriveVec3 trans{ 0, 0, 0 };
};

struct SteeringSensitivityPoint {
	float velocity = 0;
	float sensitivity = 0;
};

struct WheelConfigDef {
	float radius = 0;
	float width = 0;
	float mass = 0;
	DriveVec3 inertia{};
	DriveVec3 offset{};

	float spring_length = 0;
	float spring_stiffness = 0;
	float spring_damping = 0;
	float spring_attach_offset_y = 0;
	float spring_max_force = 0;
	float spring_progressive_exponent = 0;
	float spring_progressive_start_ratio = 0;
	float spring_disabled_strength_modifier = 0;

	float brake_force = 0;
	float brake_factor = 0;
	float hand_brake_force = 0;
	float hand_brake_factor = 0;
	float diff_gear_ratio = 0;
	int32_t driving_type = 0;
	int32_t steering_type = 0;
	int32_t engine_index = 0;
	bool has_steering_inverted = false;
	std::vector<SteeringSensitivityPoint> steering_sensitivity;

	float lateral_friction_force_max_factor = 0;
	float longitudinal_friction_force_max_factor = 0;
	float longitudinal_pos_k = 0, longitudinal_neg_k = 0;
	float lateral_pos_k = 0, lateral_neg_k = 0;
	float align_mom_scale = 0;

	float rolling_resistance_base_factor = 0;
	float rolling_resistance_velocity_factor = 0;
	bool use_engine_brake = false;
	float engine_brake_min_factor = 0, engine_brake_max_factor = 0, engine_brake_velocity_factor = 0;
	std::vector<std::array<float, 2>> transmission_loss;
};

struct VehicleWheelDef {
	uint32_t component = 0;
	std::string physics_type;
	DriveTransform transform;
	uint32_t config = 0;
};

struct EngineDef {
	std::string config_type;
	DriveTransform transform;
	std::vector<float> rpm_points;
	std::vector<float> torque_points;
	float rpm_min = 0, rpm_max = 0, rpm_cut = 0;
	float power_multiplier = 1;
	float internal_acceleration = 0;
	float internal_deceleration = 0;

	float boost_acceleration_scale = 1, boost_forward_strength = 1, boost_reverse_strength = 1;
	float boost_crawl_strength = 1, boost_dissipation_time = 1, boost_recovery_time = 1;
};

struct GearboxDef {
	std::vector<float> forward_ratios, forward_speeds;
	std::vector<float> reverse_ratios, reverse_speeds;
	float gear_change_time = 0;
	float clutch_speed_factor = 0;
	float gear_down_speed_factor = 0;
	float opposite_dir_gear_change_max_speed = 0;
	float opposite_dir_gear_change_time = 0;
	float backward_throttle_limit = 0;
	float transmission_efficiency = 1;
	int32_t gearbox_type = 0, gearbox_mode = 0;
	bool use_auto_clutch = false, use_neutral_gear = false, limit_backward_throttle = false;
	bool present = false;
};

struct InputAxisDef {
	float deadzone = 0;
	float inertia_in = 0;
	float inertia_out = 0;
	float inertia_min_ratio = 0;
};

struct ChassisConfigDef {
	float body_mass = 0;
	float gravity_modifier = 1;
	DriveVec3 center_of_mass{};
	DriveVec3 center_of_mass_handling_offset{};
	DriveVec3 inertia_modifier{ 1, 1, 1 };
	float air_density = 0, air_drag_area = 0, coefficient_of_air_friction = 0;
	float anti_roll_front = 0, anti_roll_rear = 0;
	float anti_roll_front_torque_limit = 0, anti_roll_rear_torque_limit = 0;
	InputAxisDef throttle, brake, yaw, pitch, roll;
	float yaw_min = -1, yaw_max = 1;
	bool use_gearbox = false, use_stand_still_brake = false, use_stand_still_sleep = false;
	bool use_turn_around_force = false, use_down_force = false, use_wind_resistance = false;
	float stand_still_low_speed_time_limit = 0;
	float down_force_base_factor = 0, down_force_wheel_factor = 0;

	float damping_linear = 0, damping_pitch = 0, damping_roll = 0, damping_yaw = 0;
	DriveVec3 damping_linear_modifier{ 1, 1, 1 };

	bool allow_negative_throttle = false, always_full_throttle = false;
};

struct DriveRigidBodyDef {
	std::string body_type;
	std::string collision_layer;
	std::string motion_type;
	float mass = 0, friction = 0, restitution = 0;
	float linear_damping = -1, angular_damping = -1;
	DriveVec3 inertia_modifier{ 1, 1, 1 };
};

struct VehicleDriveDef {
	std::string blueprint;
	ChassisConfigDef chassis;
	GearboxDef gearbox;
	std::vector<VehicleWheelDef> wheels;
	std::vector<WheelConfigDef> wheel_configs;
	std::vector<EngineDef> engines;
	std::vector<DriveRigidBodyDef> rigid_bodies;
	std::string physics_asset;
	float physics_asset_scale = 1;
	size_t components = 0;
};

Result<VehicleDriveDef> read_vehicle_drive_def(const EbxResolver &p_db, std::string_view p_blueprint, WorkBudget *p_budget = nullptr);

}
