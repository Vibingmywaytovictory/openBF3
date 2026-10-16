#pragma once
// VehicleDriveDef (S2): what a wheeled VehicleBlueprint says about driving it (plan §3.4 "wheeled", §7.4;
// research/topics/04-vehicles.md §2.0, §2.1; docs/design/s2.md §4). Godot-free; plain data, no Jolt.
//
// The component tree of VehicleEntityData is walked depth first like vehicle_def.h (the same exclusion rules:
// Excluded components and player-unlock-only components are left out with everything below them), but this walk
// also composes the component transforms (IW4L level_bf3 vehicles/layout.rs `local.in_parent(frame)`, ported,
// Apache-2.0, the user's own), so every wheel and engine carries its position in the chassis (vehicle) frame.
// Read, per component type:
//   ChassisComponentData      -> chassis (its VehicleConfigData: mass, gravity, CoM, inertia, air terms,
//                                anti-roll bars, input filters, stand-still and down-force switches, motion damping)
//                                and gearbox (its GearboxConfigData)
//   WheelComponentData        -> wheels (PhysicsType, composed transform, index into wheel_configs)
//   EngineComponentData       -> engines (CombustionEngineConfigData: rpm -> torque curve, rpm limits, boost)
//   PhysicsEntityData         -> rigid_bodies (RigidBodyData in RigidBodies order) and the Havok asset
//                                (Asset, else ScaledAssets[0]; the Humvee's Asset is null and ScaledAssets names it)
// Units are as stored; what a field means where the data does not say is the consumer's call (bf3_sim
// vehicle_model.h maps them, every mapping tagged there). Checked on vehicles/humveearmored/humveearmored in
// MP_007 (bf3dump dump, 2026-10-15; docs/design/s2.md §4.2): 4 wptNormal wheels on 2 configs, 1 combustion engine,
// a 5-forward / 2-reverse gearbox, 2 rigid bodies (RBTypeCollision dynamic 1500 kg, RBTypeRaycast fixed).
//
// Bounds (hostile data): every array element, reference and examined object charges the WorkBudget like the other
// gameplay readers; at most kMaxDriveWheels wheels, kMaxDriveCurvePoints points per curve, kMaxDriveGears gears per
// direction and kMaxDriveRigidBodies rigid bodies (more fails with an error, never truncates); the walk depth is the
// vehicle_def.h walk's (64). Non-finite floats fail.

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

inline constexpr size_t kMaxDriveWheels = 16; // the sim takes at most 8 physics wheels (bf3_sim kMaxVehicleWheels)
inline constexpr size_t kMaxDriveCurvePoints = 32;
inline constexpr size_t kMaxDriveGears = 16;
inline constexpr size_t kMaxDriveRigidBodies = 16;
inline constexpr size_t kMaxDriveEngines = 4;

using DriveVec3 = std::array<float, 3>;

// A LinearTransform composed into the chassis frame (rows right, up, forward, trans; Frostbite axes, +Z forward).
struct DriveTransform {
	DriveVec3 right{ 1, 0, 0 };
	DriveVec3 up{ 0, 1, 0 };
	DriveVec3 forward{ 0, 0, 1 };
	DriveVec3 trans{ 0, 0, 0 };
};

struct SteeringSensitivityPoint {
	float velocity = 0; // SensitivityAtVelocity.Velocity (unit not stated; km/h inferred, s2.md §5.3)
	float sensitivity = 0; // SensitivityAtVelocity.SteeringSensitivity
};

// WheelConfigData (shared by the wheels that name it).
struct WheelConfigDef {
	float radius = 0;
	float width = 0;
	float mass = 0;
	DriveVec3 inertia{};
	DriveVec3 offset{}; // Offset
	// SpringData
	float spring_length = 0;
	float spring_stiffness = 0;
	float spring_damping = 0;
	float spring_attach_offset_y = 0;
	float spring_max_force = 0;
	float spring_progressive_exponent = 0;
	float spring_progressive_start_ratio = 0;
	float spring_disabled_strength_modifier = 0;
	// brakes, drive, steering
	float brake_force = 0;
	float brake_factor = 0;
	float hand_brake_force = 0;
	float hand_brake_factor = 0;
	float diff_gear_ratio = 0;
	int32_t driving_type = 0; // DrivingType (Humvee 1 on all wheels)
	int32_t steering_type = 0; // SteeringType (Humvee front 1, rear 0)
	int32_t engine_index = 0;
	bool has_steering_inverted = false;
	std::vector<SteeringSensitivityPoint> steering_sensitivity; // by velocity as stored
	// grip
	float lateral_friction_force_max_factor = 0;
	float longitudinal_friction_force_max_factor = 0;
	float longitudinal_pos_k = 0, longitudinal_neg_k = 0;
	float lateral_pos_k = 0, lateral_neg_k = 0;
	float align_mom_scale = 0;
	// resistance and engine brake
	float rolling_resistance_base_factor = 0;
	float rolling_resistance_velocity_factor = 0;
	bool use_engine_brake = false;
	float engine_brake_min_factor = 0, engine_brake_max_factor = 0, engine_brake_velocity_factor = 0;
	std::vector<std::array<float, 2>> transmission_loss; // TransmissionLoss Vec2 (x, y)
};

struct VehicleWheelDef {
	uint32_t component = 0; // depth-first index among the walked components
	std::string physics_type; // wptNormal, ...
	DriveTransform transform; // composed into the chassis frame (wheel centre = trans)
	uint32_t config = 0; // index into VehicleDriveDef::wheel_configs
};

// CombustionEngineConfigData (others are listed by type with no curve).
struct EngineDef {
	std::string config_type; // CombustionEngineConfigData, ...
	DriveTransform transform;
	std::vector<float> rpm_points; // RpmCurvePoints
	std::vector<float> torque_points; // TorqueCurvePoints (same count; a mismatch is an error)
	float rpm_min = 0, rpm_max = 0, rpm_cut = 0;
	float power_multiplier = 1; // EnginePowerMultiplier
	float internal_acceleration = 0; // InternalAccelerationFactor
	float internal_deceleration = 0; // InternalDeaccelerationFactor
	// Boost
	float boost_acceleration_scale = 1, boost_forward_strength = 1, boost_reverse_strength = 1;
	float boost_crawl_strength = 1, boost_dissipation_time = 1, boost_recovery_time = 1;
};

// GearboxConfigData.
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
	bool present = false; // false: the chassis names no GearboxConfig
};

// One VehicleInputData axis (Throttle, Brake, Yaw, Pitch, Roll).
struct InputAxisDef {
	float deadzone = 0;
	float inertia_in = 0; // <Axis>InertiaInDuration (s)
	float inertia_out = 0; // <Axis>InertiaOutDuration (s)
	float inertia_min_ratio = 0;
};

// VehicleConfigData of the chassis.
struct ChassisConfigDef {
	float body_mass = 0;
	float gravity_modifier = 1;
	DriveVec3 center_of_mass{};
	DriveVec3 center_of_mass_handling_offset{};
	DriveVec3 inertia_modifier{ 1, 1, 1 };
	float air_density = 0, air_drag_area = 0, coefficient_of_air_friction = 0;
	float anti_roll_front = 0, anti_roll_rear = 0; // AntiRollBars.Front / Rear Stiffness (0 when null)
	float anti_roll_front_torque_limit = 0, anti_roll_rear_torque_limit = 0;
	InputAxisDef throttle, brake, yaw, pitch, roll;
	float yaw_min = -1, yaw_max = 1;
	bool use_gearbox = false, use_stand_still_brake = false, use_stand_still_sleep = false;
	bool use_turn_around_force = false, use_down_force = false, use_wind_resistance = false;
	float stand_still_low_speed_time_limit = 0;
	float down_force_base_factor = 0, down_force_wheel_factor = 0;
	// MotionDampingData (0 when null)
	float damping_linear = 0, damping_pitch = 0, damping_roll = 0, damping_yaw = 0;
	DriveVec3 damping_linear_modifier{ 1, 1, 1 };
	// ChassisComponentData
	bool allow_negative_throttle = false, always_full_throttle = false;
};

struct DriveRigidBodyDef {
	std::string body_type; // RBTypeCollision, RBTypeRaycast, ...
	std::string collision_layer; // RigidBodyCollisionLayer_VehicleLayer, ...
	std::string motion_type; // RigidBodyMotionType_Dynamic, _Fixed, ...
	float mass = 0, friction = 0, restitution = 0;
	float linear_damping = -1, angular_damping = -1; // -1 = engine default
	DriveVec3 inertia_modifier{ 1, 1, 1 };
};

struct VehicleDriveDef {
	std::string blueprint; // partition
	ChassisConfigDef chassis;
	GearboxDef gearbox;
	std::vector<VehicleWheelDef> wheels; // depth-first order
	std::vector<WheelConfigDef> wheel_configs; // first-use order
	std::vector<EngineDef> engines;
	std::vector<DriveRigidBodyDef> rigid_bodies; // PhysicsEntityData.RigidBodies order = Havok root order
	std::string physics_asset; // HavokAsset Name, e.g. Vehicles/HumveeArmored/HumveeArmored_Physics_0_Win32
	float physics_asset_scale = 1;
	size_t components = 0; // active components walked
};

// Reads a VehicleBlueprint partition (e.g. vehicles/humveearmored/humveearmored).
// p_budget: the work budget to charge (a WorkBudget of the default limit when null).
Result<VehicleDriveDef> read_vehicle_drive_def(const EbxResolver &p_db, std::string_view p_blueprint, WorkBudget *p_budget = nullptr);

} // namespace bf3::core::gameplay
