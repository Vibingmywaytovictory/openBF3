#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/math.h"
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
	std::string camera_type;
	std::string camera;
	float field_of_view = 0;
	bool first_person = false;

	float left_yaw = 0, right_yaw = 0, up_pitch = 0, down_pitch = 0;
	std::optional<float> zoom_field_of_view;
};

struct VehicleTurretDef {
	int parent = -1;
	int seat = -1;
	int rotation_axis = 1;
	std::string rotation_input;
	bool constrained = false;
	float min_angle = 0, max_angle = 0;
	float world_space_lock_efficiency = 0;
	float angular_momentum_multiplier = 0;
	float angular_momentum_threshold = 0;
	float angular_momentum_damping = 0;
	float mass = 0, inertia = 0;
};

struct VehicleWeaponDef {
	std::string damage_giver_name;
	std::string firing;
	std::string classification;
	int seat = -1;
	int turret = -1;
	float damage_multiplier = 1;
	float explosion_damage_multiplier = 1;
};

struct VehicleSeatDef {
	int entry_order = 0;
	std::string seat_type;
	std::string component_type;
	float entry_radius = 0;
	bool shows_soldier = false;
	bool own_weapon = false;
	std::vector<int> weapons;
	std::vector<VehicleCameraDef> cameras;
};

struct VehicleDef {
	std::string blueprint;
	std::string name_sid;
	std::string minimap_icon;
	std::string controllable_type;

	float body_mass = 0;
	float gravity_modifier = 1;
	core::Vec3 center_of_mass{};
	core::Vec3 inertia_modifier{ 1, 1, 1 };

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

	std::vector<VehicleSeatDef> seats;
	std::vector<VehicleTurretDef> turrets;
	std::vector<VehicleWeaponDef> weapons;
	size_t components = 0;

	float max_zone_health() const;
	const VehicleSeatDef *driver() const;
};

Result<VehicleDef> read_vehicle_def(const EbxResolver &p_db, std::string_view p_blueprint, WorkBudget *p_budget = nullptr);

}
