#pragma once

#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/vehicle_types.h"

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

enum class WheelCollisionTester : uint8_t {
	Cylinder = 0,
	Sphere = 1,
	Ray = 2,
};

enum class SteerAngleModel : uint8_t {

	ScaledLock = 0,

	SensitivityRadians = 1,
};

struct VehicleTuning {

	float gravity = 9.81f;

	float ground_friction = 1.0f;

	float static_sag_fraction = 7.0f / 16.0f;
	float damping_ratio = 0.45f;
	float suspension_min_fraction = 0.1f;

	float wheel_angular_damping = 0.2f;
	float brake_torque_per_force = 1.0f;
	float hand_brake_torque = 4000.0f;
	float longitudinal_grip = 1.0f;
	float lateral_grip = 1.0f;
	WheelCollisionTester tester = WheelCollisionTester::Cylinder;

	SteerAngleModel steer_model = SteerAngleModel::ScaledLock;
	float steer_max_degrees = 32.0f;

	float engine_inertia = 0.5f;
	float engine_angular_damping = 0.2f;
	float shift_up_rpm = 3500.0f;
	float shift_down_rpm = 1500.0f;
	float switch_time = 0.5f;
	float clutch_release_time = 0.3f;
	float switch_latency = 0.5f;
	float clutch_strength = 10.0f;
	float differential_limited_slip = 1.4f;

	float anti_roll_scale = 10000.0f;
	float drag_scale = 1.0f;
	float max_pitch_roll_degrees = 180.0f;

	float stand_still_speed = 0.5f;
	float reverse_switch_speed = 1.0f;

	uint32_t collision_steps = 1;
	uint32_t velocity_steps = 10;
	uint32_t position_steps = 2;

	bool operator==(const VehicleTuning &) const = default;
};

core::Result<VehicleTuning> parse_vehicle_tuning(std::string_view p_text);

std::string format_vehicle_tuning(const VehicleTuning &p_tuning);
uint64_t vehicle_tuning_hash(const VehicleTuning &p_tuning);

struct ChassisPiece {
	std::vector<Vec3> points;
	float convex_radius = 0;
};

struct VehicleWheelModel {
	Vec3 attachment{};
	Vec3 data_centre{};
	float radius = 0, width = 0;
	float suspension_min = 0, suspension_max = 0;
	float spring_stiffness = 0;
	float spring_damping = 0;
	float inertia = 0;
	float angular_damping = 0;
	float max_steer_angle = 0;
	float max_brake_torque = 0, max_hand_brake_torque = 0;
	uint8_t steers = 0, driven = 0, axle = 0;
	uint8_t side = 0;
};

struct VehicleModel {
	std::string blueprint;

	float mass = 0;
	float gravity_factor = 1;
	Vec3 center_of_mass{};
	Vec3 inertia_modifier{ 1, 1, 1 };
	float friction = 0.5f, restitution = 0.4f;
	std::vector<ChassisPiece> chassis;

	std::array<VehicleWheelModel, kMaxVehicleWheels> wheels{};
	uint8_t wheel_count = 0;

	float engine_max_torque = 0;
	float engine_min_rpm = 0, engine_max_rpm = 0;
	std::array<std::array<float, 2>, kMaxModelCurvePoints> torque_curve{};
	uint8_t torque_points = 0;
	float engine_inertia = 0, engine_angular_damping = 0;

	std::array<float, kMaxModelGears> forward_ratios{};
	std::array<float, kMaxModelGears> reverse_ratios{};
	uint8_t forward_gears = 0, reverse_gears = 0;
	float shift_up_rpm = 0, shift_down_rpm = 0, switch_time = 0, clutch_release_time = 0, switch_latency = 0, clutch_strength = 0;

	std::array<std::array<int8_t, 2>, 4> differential_wheels{};
	std::array<float, 4> differential_ratio{};
	std::array<float, 4> differential_torque_ratio{};
	uint8_t differentials = 0;
	float differential_limited_slip = 0;

	std::array<std::array<int8_t, 2>, 2> anti_roll_wheels{};
	std::array<float, 2> anti_roll_stiffness{};
	uint8_t anti_roll_bars = 0;

	std::array<std::array<float, 2>, kMaxModelCurvePoints> steer_sensitivity{};
	uint8_t steer_points = 0;
	SteerAngleModel steer_model = SteerAngleModel::ScaledLock;
	float yaw_inertia_in = 0, yaw_inertia_out = 0, yaw_inertia_min_ratio = 0;
	float throttle_inertia_in = 0, throttle_inertia_out = 0;
	float brake_inertia_in = 0, brake_inertia_out = 0;
	bool allow_negative_throttle = true;

	float drag_coefficient = 0;
	bool stand_still_brake = false;
	uint16_t stand_still_ticks = 0;
	float stand_still_speed = 0, reverse_switch_speed = 0;

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

float vehicle_max_steer_angle(const VehicleModel &p_model, float p_speed);

}
