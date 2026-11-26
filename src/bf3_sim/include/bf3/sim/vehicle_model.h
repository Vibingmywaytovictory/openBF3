#pragma once

#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/vehicle_types.h"
#include "bf3/sim/vehicles/chassis_rules.h"
#include "bf3/sim/vehicles/land_rules.h"
#include "bf3/sim/vehicles/tyre_rules.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {
class LevelMount;
class EbxResolver;
}
namespace bf3::core::gameplay {
struct VehicleDriveDef;
class WorkBudget;
}

namespace bf3::sim {

inline constexpr size_t kMaxChassisPieces = 64;
inline constexpr size_t kMaxChassisPiecePoints = 256;
inline constexpr size_t kMaxModelCurvePoints = 16;
inline constexpr size_t kMaxModelGears = 8;
inline constexpr uint32_t kBalanceSpeedSteps = 800;
inline constexpr float kBalanceSpeedStepKmh = 0.5f;

enum class WheelCollisionTester : uint8_t {
	Cylinder = 0,
	Sphere = 1,
	Ray = 2,
};

struct VehicleTuning {

	float gravity = 9.81f;

	float ground_friction = 1.0f;

	float suspension_min_fraction = 0.1f;

	float longitudinal_grip = 1.0f;
	float lateral_grip = 1.0f;
	WheelCollisionTester tester = WheelCollisionTester::Cylinder;

	float max_pitch_roll_degrees = 180.0f;

	float stand_still_speed = 0.5f;

	uint32_t collision_steps = 1;
	uint32_t velocity_steps = 10;
	uint32_t position_steps = 2;

	bool operator==(const VehicleTuning &) const = default;
};

inline constexpr size_t kMaxVehicleTuningLines = 512;
inline constexpr size_t kMaxVehicleTuningLineBytes = 256;

core::Result<VehicleTuning> parse_vehicle_tuning(std::string_view p_text);

std::string format_vehicle_tuning(const VehicleTuning &p_tuning);
uint64_t vehicle_tuning_hash(const VehicleTuning &p_tuning);

struct ChassisPiece {
	std::vector<Vec3> points;
	float convex_radius = 0;
};

struct LandDriveModel {
	vehicles::InputAxisRule throttle, brake, yaw;
	std::array<float, kMaxModelGears> forward_ratios{}, reverse_ratios{};
	std::array<float, kMaxModelGears> forward_speeds{}, reverse_speeds{};
	uint8_t forward_gears = 0, reverse_gears = 0;
	uint8_t forward_speed_count = 0, reverse_speed_count = 0;
	int32_t gearbox_type = 0, gearbox_mode = 0;
	float gear_change_time = 0, gear_down_speed_factor = 0, clutch_speed_factor = 0;
	float opposite_dir_gear_change_max_speed = 0, opposite_dir_gear_change_time = 0;
	float transmission_efficiency = 1;
	bool use_neutral_gear = false, use_classic_auto_clutch = false, limit_backward_throttle = false;
	float backward_throttle_limit = 0;
	vehicles::EngineRule engine;
	std::array<float, kMaxModelCurvePoints> rpm_points{}, torque_points{};
	uint8_t curve_points = 0;
	float power_multiplier = 1;
	float track_forward_speed_limit = 0, track_turn_speed_limit = 0;
	std::array<std::array<float, 2>, kMaxModelCurvePoints> transmission_loss{};
	uint8_t transmission_loss_points = 0;
	std::array<std::array<float, 2>, kMaxModelCurvePoints> steer_inertia{};
	uint8_t steer_inertia_points = 0;
};

vehicles::GearboxRule land_gearbox_rule(const LandDriveModel &p_land);
float land_gear_ratio(const LandDriveModel &p_land, int32_t p_gear);

struct VehicleWheelModel {
	Vec3 attachment{};
	Vec3 data_centre{};
	float radius = 0, width = 0;
	float suspension_min = 0, suspension_max = 0;
	float spring_constant = 0;
	float spring_damping = 0;
	float progressive_start = 0, progressive_exponent = 0;
	float rest_compression = 0;
	float inertia = 0;
	float mass = 0;
	float brake_factor = 0, hand_brake_factor = 0;
	float diff_gear_ratio = 0;
	float rolling_base = 0, rolling_velocity_kmh = 0;
	uint8_t steers = 0, driven = 0, axle = 0;
	uint8_t side = 0;
	uint8_t track_steer = 0;
	uint8_t rolling_resistance = 0, engine_brake = 0, low_speed_auto_brake = 0;
	float fx_torque_multiplier = 1;
	float longitudinal_force_max = 0, lateral_force_max = 0;
	float lattitude_brake_scale = 1;
	float constant_spring_force_friction_scale = 0;
	float slope_grip_min = 0, slope_grip_max = 0;
	int32_t slope_grip_exponent = 1;
	vehicles::GripRule grip;
	vehicles::TyreCurveRule tyre;
	std::array<std::array<float, 2>, kMaxModelCurvePoints> longitude_scale{}, lattitude_scale{};
	uint8_t longitude_scale_points = 0, lattitude_scale_points = 0;
};

struct VehicleModel {
	std::string blueprint;

	float mass = 0;
	float gravity_factor = 1;
	Vec3 center_of_mass{};
	Vec3 inertia_modifier{ 1, 1, 1 };
	Vec3 center_of_mass_handling_offset{};
	float friction = 0.5f, restitution = 0.4f;
	float linear_damping = 0, angular_damping = 0;
	std::vector<ChassisPiece> chassis;

	std::array<VehicleWheelModel, kMaxVehicleWheels> wheels{};
	uint8_t wheel_count = 0;
	uint8_t wheel_list = 0;

	std::array<std::array<float, 2>, kMaxModelCurvePoints> steer_sensitivity{};
	uint8_t steer_points = 0;
	bool steer_inverted = false;

	LandDriveModel land;

	vehicles::MotionDampingRule motion_damping;
	bool wind_resistance = false;
	vehicles::WindRule wind;
	bool down_force = false, down_force_wheel_factor = false;
	float anti_roll_front = 0, anti_roll_rear = 0;
	bool stand_still_brake = false;
	uint16_t stand_still_ticks = 0;
	float stand_still_speed = 0;
	float down_force_base = 0, down_force_per_wheel = 0;

	WheelCollisionTester tester = WheelCollisionTester::Cylinder;
	float max_pitch_roll_angle = 0;
	VehicleTuning tuning;

	uint64_t model_digest = 0;
};

struct ChassisPieceSource {
	std::span<const Vec3> points;
	float radius = 0;
};

core::Result<std::shared_ptr<const VehicleModel>> build_vehicle_model(const core::gameplay::VehicleDriveDef &p_def,
		std::span<const ChassisPieceSource> p_chassis, const VehicleTuning &p_tuning);

core::Result<std::shared_ptr<const VehicleModel>> load_vehicle_model(const core::EbxResolver &p_db, const core::LevelMount &p_mount,
		std::string_view p_blueprint, const VehicleTuning &p_tuning, core::gameplay::WorkBudget *p_budget = nullptr);

float vehicle_steer_sensitivity(const VehicleModel &p_model, float p_speed);

float land_balance_speed_kmh(const VehicleModel &p_model);
}
