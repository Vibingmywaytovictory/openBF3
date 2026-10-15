#pragma once
// VehicleModel: one wheeled vehicle type as the vehicle world builds it (docs/design/s2.md §5). Godot-free and
// Jolt-free: plain numbers, built from bf3_core's VehicleDriveDef (gameplay/vehicle_drive.h), the chassis convex
// pieces of the vehicle's Havok asset (res/havok.h havok_convex_pieces of root 0, the RBTypeCollision body) and
// VehicleTuning.
//
// Numbers: what the data gives is taken as stored (mass, gravity factor, CoM, inertia modifier, wheel positions,
// radius, width, spring length, brake force, engine curve and rpm limits, gear and differential ratios, steering
// sensitivity curve, yaw input filter, anti-roll stiffness, air terms, chassis friction and restitution). What the
// data does not say, or says in a unit we cannot read yet, is VehicleTuning: a config with documented placeholders
// (IW4L's tuned jeep where it has one, ported with attribution; Jolt's defaults otherwise), read from
// res://config/vehicle_tuning.cfg in the game and settable in tests, so S5 Session 2's measured values (H:V1-V6)
// drop in without code. Every key and its source is listed in s2.md §5.3 and in the file.
//
// Determinism: the model is a pure function of (def, pieces, tuning); model_digest hashes every field's bits, and
// two VehicleWorlds that add vehicles from models with equal digests build bit-identical Jolt bodies and
// constraints (s2.md §5.5).

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
} // namespace bf3::core
namespace bf3::core::gameplay {
struct VehicleDriveDef;
class WorkBudget;
} // namespace bf3::core::gameplay

namespace bf3::sim {

// Bounds of a model (s2.md §9): more is a build error, never a truncation.
inline constexpr size_t kMaxChassisPieces = 64;
inline constexpr size_t kMaxChassisPiecePoints = 256;
inline constexpr size_t kMaxModelCurvePoints = 16;
inline constexpr size_t kMaxModelGears = 8;

// ---- tuning: what the data does not say ---------------------------------------------------------------------

enum class WheelCollisionTester : uint8_t {
	Cylinder = 0, // JPH::VehicleCollisionTesterCastCylinder (the wheel's radius and width) [inferred]
	Sphere = 1, // VehicleCollisionTesterCastSphere (radius)
	Ray = 2, // VehicleCollisionTesterRay
};

enum class SteerAngleModel : uint8_t {
	// max steer angle = steer_max_degrees x sensitivity(v) / sensitivity(0) (IW4L's 32 degrees as the low-speed
	// lock, the data's curve as the speed falloff; v in km/h) [inferred]
	ScaledLock = 0,
	// max steer angle = sensitivity(v) radians (0.25 rad = 14.3 degrees at rest) [inferred alternative]
	SensitivityRadians = 1,
};

struct VehicleTuning {
	// world
	float gravity = 9.81f; // m/s^2 before the chassis GravityModifier (1.85 Humvee) [inferred]
	// Friction of the static ground as the tyres see it, applied in the wheel's combine callback (tyre x this on a
	// static body; VehicleConstraint::SetCombineFriction), never by changing the static bodies, which stay
	// byte-identical to CollisionWorld's. The material grid's per-surface friction (S5) replaces it through the
	// same callback (the sub-shape ID names the triangle) [inferred]
	float ground_friction = 1.0f;
	// suspension: IW4L jeep (vehicle_iw4 def.rs, Apache-2.0, the user's own): static sag 7/16 of the travel,
	// damper 0.9 sqrt(k M / 4) (damping ratio 0.45). The data's Spring.Stiffness 0.5 / Damping 0.06 are not read
	// until S5 maps their unit.
	float static_sag_fraction = 7.0f / 16.0f;
	float damping_ratio = 0.45f;
	float suspension_min_fraction = 0.1f; // min length as a fraction of Spring.Length (bump stop) [inferred]
	// wheels
	float wheel_angular_damping = 0.2f; // Jolt default
	float brake_torque_per_force = 1.0f; // max brake torque = BrakeForce x radius x this (Humvee 1,840 Nm) [inferred]
	float hand_brake_torque = 4000.0f; // Nm on non-steering wheels (data HandBrakeForce 0; IW4L handbrake rear) [inferred]
	float longitudinal_grip = 1.0f; // scales Jolt's default friction curves [inferred]
	float lateral_grip = 1.0f;
	WheelCollisionTester tester = WheelCollisionTester::Cylinder;
	// steering
	SteerAngleModel steer_model = SteerAngleModel::ScaledLock;
	float steer_max_degrees = 32.0f; // IW4L jeep max_steer_deg
	// engine and gearbox (Jolt VehicleEngineSettings / VehicleTransmissionSettings)
	float engine_inertia = 0.5f; // kg m^2, Jolt default (data InternalAccelerationFactor 8 unmapped)
	float engine_angular_damping = 0.2f;
	float shift_up_rpm = 3500.0f; // torque falls past 3,500 rpm in the data curve [inferred]
	float shift_down_rpm = 1500.0f;
	float switch_time = 0.5f; // s; Jolt default (data GearChangeTime 0)
	float clutch_release_time = 0.3f;
	float switch_latency = 0.5f;
	float clutch_strength = 10.0f; // data ClutchSpeedFactor 10 = Jolt default 10 [inferred same meaning]
	float differential_limited_slip = 1.4f; // Jolt default
	// chassis
	float anti_roll_scale = 10000.0f; // N/m per unit of AntiRollBar.Stiffness (1.2 / 0.5 -> 12,000 / 5,000) [inferred]
	float drag_scale = 1.0f; // F = -0.5 AirDensity CoefficientOfAirFriction AirDragArea |v| v x this [inferred]
	float max_pitch_roll_degrees = 180.0f; // Jolt mMaxPitchRollAngle (no limit) [inferred]
	// driver
	float stand_still_speed = 0.5f; // m/s below which the stand-still timer runs (UseStandStillBrake) [inferred]
	float reverse_switch_speed = 1.0f; // m/s: below it negative throttle selects reverse, above it brakes [inferred]
	// physics step
	uint32_t collision_steps = 1; // PhysicsSystem::Update collision steps per 60 Hz tick
	uint32_t velocity_steps = 10; // PhysicsSettings::mNumVelocitySteps (Jolt default)
	uint32_t position_steps = 2; // PhysicsSettings::mNumPositionSteps (Jolt default)

	bool operator==(const VehicleTuning &) const = default;
};

// "key = value" lines, '#' comments, blank lines (motor_tuning.cfg's format). Unknown keys, a missing '=', an
// unparsable or non-finite value, a duplicate key, a line over 256 bytes or more than 512 lines fail with the line
// number. Keys omitted keep the defaults above.
core::Result<VehicleTuning> parse_vehicle_tuning(std::string_view p_text);
// Every key in declaration order, parseable by parse_vehicle_tuning (round trip exact).
std::string format_vehicle_tuning(const VehicleTuning &p_tuning);
uint64_t vehicle_tuning_hash(const VehicleTuning &p_tuning); // FNV-1a over format_vehicle_tuning

// ---- the model ------------------------------------------------------------------------------------------------

struct ChassisPiece { // one convex hull, vehicle frame
	std::vector<Vec3> points; // <= kMaxChassisPiecePoints
	float convex_radius = 0; // the Havok piece's radius (Humvee 0.03 / 0.015 m)
};

struct VehicleWheelModel {
	Vec3 attachment{}; // suspension attachment, vehicle frame: wheel centre + (Spring.Length - sag) up
	Vec3 data_centre{}; // the WheelComponentData position (the wheel centre at static load)
	float radius = 0, width = 0;
	float suspension_min = 0, suspension_max = 0; // m
	float spring_stiffness = 0; // N/m (ESpringMode::StiffnessAndDamping)
	float spring_damping = 0; // N s/m
	float inertia = 0; // kg m^2 (data Inertia.x taken as kg m^2 [inferred])
	float angular_damping = 0;
	float max_steer_angle = 0; // rad, the low-speed lock
	float max_brake_torque = 0, max_hand_brake_torque = 0; // Nm
	uint8_t steers = 0, driven = 0, axle = 0; // axle: wheels grouped by z within 5 cm, front (largest z) = 0
	uint8_t side = 0; // 0 left (x > 0: Jolt's left, its right = forward x up = -X, s2.md §5.4), 1 right (x < 0)
};

struct VehicleModel {
	std::string blueprint;
	// chassis body (RBTypeCollision rigid body + VehicleConfigData)
	float mass = 0;
	float gravity_factor = 1;
	Vec3 center_of_mass{};
	Vec3 inertia_modifier{ 1, 1, 1 }; // multiplies the shape's inertia diagonal (principal axes) [inferred]
	float friction = 0.5f, restitution = 0.4f;
	std::vector<ChassisPiece> chassis; // <= kMaxChassisPieces
	// wheels, in VehicleDriveDef wheel order
	std::array<VehicleWheelModel, kMaxVehicleWheels> wheels{};
	uint8_t wheel_count = 0;
	// engine (the first combustion engine)
	float engine_max_torque = 0; // Nm: max of TorqueCurvePoints
	float engine_min_rpm = 0, engine_max_rpm = 0;
	std::array<std::array<float, 2>, kMaxModelCurvePoints> torque_curve{}; // (rpm / max rpm, torque / max torque)
	uint8_t torque_points = 0;
	float engine_inertia = 0, engine_angular_damping = 0;
	// transmission
	std::array<float, kMaxModelGears> forward_ratios{};
	std::array<float, kMaxModelGears> reverse_ratios{}; // negative, as stored
	uint8_t forward_gears = 0, reverse_gears = 0;
	float shift_up_rpm = 0, shift_down_rpm = 0, switch_time = 0, clutch_release_time = 0, switch_latency = 0, clutch_strength = 0;
	// differentials: one per axle with two driven wheels (Humvee: front and rear, 0.5 torque each)
	std::array<std::array<int8_t, 2>, 4> differential_wheels{}; // (left, right) wheel indices
	std::array<float, 4> differential_ratio{}; // DiffGearRatio x TransmissionEfficiency
	std::array<float, 4> differential_torque_ratio{};
	uint8_t differentials = 0;
	float differential_limited_slip = 0;
	// anti-roll bars (front, rear): (left, right) wheel indices and N/m
	std::array<std::array<int8_t, 2>, 2> anti_roll_wheels{};
	std::array<float, 2> anti_roll_stiffness{};
	uint8_t anti_roll_bars = 0;
	// steering and input
	std::array<std::array<float, 2>, kMaxModelCurvePoints> steer_sensitivity{}; // (km/h, sensitivity)
	uint8_t steer_points = 0;
	SteerAngleModel steer_model = SteerAngleModel::ScaledLock;
	float yaw_inertia_in = 0, yaw_inertia_out = 0, yaw_inertia_min_ratio = 0; // s, s, ratio
	float throttle_inertia_in = 0, throttle_inertia_out = 0;
	float brake_inertia_in = 0, brake_inertia_out = 0;
	bool allow_negative_throttle = true;
	// resistance and driver aids
	float drag_coefficient = 0; // 0.5 rho Cd A x drag_scale (Humvee 2.756 N per (m/s)^2)
	bool stand_still_brake = false;
	uint16_t stand_still_ticks = 0; // StandStillLowSpeedTimeLimit in ticks
	float stand_still_speed = 0, reverse_switch_speed = 0;
	// world and step
	WheelCollisionTester tester = WheelCollisionTester::Cylinder;
	float max_pitch_roll_angle = 0; // rad
	VehicleTuning tuning; // the tuning the model was built with (world-level keys: gravity, ground friction, steps)

	uint64_t model_digest = 0; // FNV-1a over every field above (s2.md §5.5)
};

// The chassis pieces in the vehicle frame (bf3_core res::havok_convex_pieces of root 0 at the identity transform,
// scaled by the HavokAsset Scale). Fails on more than the bounds or a non-finite point.
struct ChassisPieceSource {
	std::span<const Vec3> points;
	float radius = 0;
};

core::Result<std::shared_ptr<const VehicleModel>> build_vehicle_model(const core::gameplay::VehicleDriveDef &p_def,
		std::span<const ChassisPieceSource> p_chassis, const VehicleTuning &p_tuning);

// read_vehicle_drive_def + load_havok (the def's physics asset, root 0) + build_vehicle_model. p_db resolves the
// blueprint (the level's EbxDb); p_mount holds the Havok res (the level mount). Read-only on the install.
core::Result<std::shared_ptr<const VehicleModel>> load_vehicle_model(const core::EbxResolver &p_db, const core::LevelMount &p_mount,
		std::string_view p_blueprint, const VehicleTuning &p_tuning, core::gameplay::WorkBudget *p_budget = nullptr);

// The max steer angle (rad) at forward speed p_speed (m/s) under the model's steer model and curve (piecewise linear,
// clamped at both ends; no trig).
float vehicle_max_steer_angle(const VehicleModel &p_model, float p_speed);

} // namespace bf3::sim
