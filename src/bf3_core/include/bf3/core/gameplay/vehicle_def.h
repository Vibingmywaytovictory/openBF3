#pragma once
// VehicleDef (M0, minimal): chassis, health and the seat / turret / weapon / camera layout of a
// VehicleBlueprint (plan §3.4, §7.4; research/topics/04-vehicles.md §2.0, §3.4, §4.1-§4.3).
//
// The component tree of VehicleEntityData is walked depth first like IW4L level_bf3
// vehicles/layout.rs (ported, Apache-2.0): Excluded components and components that only a player
// unlock adds (UnlockComponentData without InvertUnlockTest) are left out with everything below
// them. Roles: chassis (carries VehicleConfig), seat (EntryComponentData), turret
// (ChildComponentData whose MovingBody is a ChildRotationBodyData), weapon (WeaponComponentData),
// camera (CameraComponentData). A component's seat is the component itself, a seat beside it, else
// the nearest seat beside one of its parents (IW4L's rule: a tank's turret ring sits beside the
// driver's seat, a roof gun's ring beside the gunner's). Seats are ordered by EntryOrderNumber; the
// driver is the EST_Driver seat, not necessarily order 0.
//
// Engine, wheel, rotor and wing models are not read here (M0 scope).

#include "bf3/core/ebx_db.h"
#include "bf3/core/result.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::gameplay {

struct VehicleHealthZoneDef {
	float max_health = 0;
	float max_shield_health = 0;
	float min_damage_angle = 0;
	float damage_angle_multiplier = 1;
	bool use_damage_angle = false;
};

struct VehicleCameraDef {
	std::string camera_type; // StaticCameraData, ChaseCameraData, ...
	std::string camera; // the camera data's partition (shared configs live in vehicles/common)
	float field_of_view = 0;
	bool first_person = false;
	// StaticCameraData free-look limits (degrees); 0 for other camera types.
	float left_yaw = 0, right_yaw = 0, up_pitch = 0, down_pitch = 0;
	std::optional<float> zoom_field_of_view; // AlternateView FieldOfView
};

struct VehicleTurretDef {
	int parent = -1; // enclosing turret index, -1 for none
	int seat = -1; // seat index (sorted order), -1 when none found
	int rotation_axis = 1; // 0 pitch, 1 yaw (ChildRotationBodyData.RotationAxis)
	std::string rotation_input; // EIARoll (yaw), EIAPitch
	bool constrained = false;
	float min_angle = 0, max_angle = 0; // AngularConstraintMin/Max, degrees
	float world_space_lock_efficiency = 0;
	float angular_momentum_multiplier = 0;
	float angular_momentum_threshold = 0;
	float angular_momentum_damping = 0;
	float mass = 0, inertia = 0;
};

struct VehicleWeaponDef {
	std::string damage_giver_name; // e.g. "M1A2 Remote HMG"
	std::string firing; // WeaponFiringData partition
	std::string classification; // WCPrimary, WCSecondary
	int seat = -1;
	int turret = -1; // the turret that aims it
	float damage_multiplier = 1;
	float explosion_damage_multiplier = 1;
};

struct VehicleSeatDef {
	int entry_order = 0; // EntryOrderNumber
	std::string seat_type; // EST_Driver, EST_Gunner, EST_Passenger
	std::string component_type; // PlayerEntryComponentData, SoldierEntryComponentData, ...
	float entry_radius = 0;
	bool shows_soldier = false; // ShowSoldierInEntry
	bool own_weapon = false; // SoldierEntryComponentData: the soldier keeps his weapon
	std::vector<int> weapons;
	std::vector<VehicleCameraDef> cameras;
};

struct VehicleDef {
	std::string blueprint; // partition
	std::string name_sid;
	std::string minimap_icon; // HudData.MinimapIcon
	std::string controllable_type;

	// VehicleConfigData of the chassis
	float body_mass = 0;
	float gravity_modifier = 1;
	std::array<float, 3> center_of_mass{};
	std::array<float, 3> inertia_modifier{ 1, 1, 1 };

	// VehicleEntityData health
	VehicleHealthZoneDef front, rear, left, right;
	bool health_zones_share_damage = false;
	float disabled_damage_threshold = 0;
	float pre_destruction_damage_threshold = 0;
	float emergency_repair_health = 0;
	float regeneration_delay = 0;
	float regeneration_rate = 0;
	float decay_delay = 0;
	float decay_rate = 0;
	float armor_multiplier = 1;
	float low_health_threshold = 0;
	float velocity_damage_threshold = 0;
	float velocity_damage_magnifier = 0;
	float water_damage = 0;
	float upside_down_angle = 0;
	float upside_down_damage = 0;
	float upside_down_damage_delay = 0;
	float kill_soldier_collision_speed_threshold = 0;
	bool use_prediction = false;

	std::vector<VehicleSeatDef> seats; // by entry order
	std::vector<VehicleTurretDef> turrets; // depth first
	std::vector<VehicleWeaponDef> weapons; // depth first
	size_t components = 0; // active components walked

	float max_zone_health() const;
	const VehicleSeatDef *driver() const;
};

// Reads a VehicleBlueprint partition (e.g. vehicles/m1a2/m1abrams).
Result<VehicleDef> read_vehicle_def(const EbxResolver &p_db, std::string_view p_blueprint);

} // namespace bf3::core::gameplay
