#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/soldier_camera.h"
#include "bf3/core/gameplay/vehicle_damage_visuals.h"
#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/math.h"
#include "bf3/core/result.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::core::gameplay {

struct VehicleHealthZoneDef {
	float max_health = 0;
	float max_shield_health = 0;
	float min_damage_angle = 0;
	float damage_angle_multiplier = 1;
	bool use_damage_angle = false;
};

struct VehicleAngleOfImpactDef {
	bool enabled = false;
	float zone12_delimiter = 0;
	float zone23_delimiter = 0;
	std::array<float, 3> multiplier{ 1, 1, 1 };
};

struct VehicleCameraDef {
	std::string camera_type;
	std::string camera;
	float field_of_view = 0;
	float regular_field_of_view = 0;
	bool first_person = false;
	core::Transform transform;
	int turret = -1;

	float left_yaw = 0, right_yaw = 0, up_pitch = 0, down_pitch = 0;
	std::optional<ChaseCameraDef> chase;
	std::optional<float> zoom_field_of_view;
	float zoom_fov_transition_time = 0;
	bool zoom_fade_to_black = false;
	float zoom_fade_out = 0, zoom_black = 0, zoom_fade_in = 0;
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

	float force_modifier = 0;
	float reset_force_modifier = 0, reset_force_input_threshold = 0;
	float damping_acc = 1, damping_deacc = 1;
	bool use_linear_input = false, use_threshold = false, use_damping = false, post_satisfy = false;
	bool extended = false;
	float extended_heading = 0, extended_width = 0, extended_falloff = 0;
	float extended_min_angle = 0, extended_max_angle = 0;
	std::string align_action;
	float align_speed = 0, align_stop_tolerance = 0;
	int frame_turret = -1;
	core::Transform frame;
	std::string sound;
};

struct VehicleWeaponDef {
	std::string damage_giver_name;
	std::string firing;
	std::string classification;
	int seat = -1;
	int turret = -1;
	float damage_multiplier = 1;
	float explosion_damage_multiplier = 1;

	int frame_turret = -1;
	core::Transform frame;
	core::Vec3 projectile_spawn_offset{};
	float impulse_strength = 0;
	float reload_time_multiplier = 1;
	float overheat_drop_multiplier = 1;
	std::vector<int> stances;
};

struct VehicleSeatDef {
	int entry_order = 0;
	std::string seat_type;
	std::string component_type;
	float entry_radius = 0;
	bool shows_soldier = false;
	bool own_weapon = false;
	core::Transform transform;
	int stance_count = 1;
	std::vector<int> weapons;
	std::vector<VehicleCameraDef> cameras;
};

struct VehiclePartTurretDef {
	uint16_t part = 0;
	int turret = -1;
};

struct VehicleInputModifierDef {
	std::string action;
	float scale = 1;
	float offset = 0;
};

struct VehicleDef {
	std::string blueprint;
	std::string name_sid;
	std::string minimap_icon;
	std::string controllable_type;
	std::string mesh;

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
	float regeneration_delay_multiplier = 1;
	float regeneration_rate_multiplier = 1;
	float critical_damage_time = 0;
	float pre_explosion_time = 0;
	VehicleAngleOfImpactDef angle_of_impact;
	MaterialPairRef material;
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
	bool ignore_soldier_collision_normal = false;
	float min_speed_for_mine_activation = 0;
	bool lockable = false;
	float radar_signature = 0;
	float heat_signature = 0;
	float locking_time_multiplier = 1;
	bool never_report_empty = false;
	bool use_prediction = false;
	Explosion explosion;
	std::string explosion_effect;
	std::vector<VehicleInputModifierDef> disabled_input;
	VehicleDamageVisualsDef damage_visuals;

	std::vector<VehicleSeatDef> seats;
	std::vector<core::Transform> exit_points;
	std::vector<std::pair<std::string, core::Transform>> component_meshes;
	std::vector<int> component_mesh_turrets;
	std::vector<VehicleTurretDef> turrets;
	std::vector<VehiclePartTurretDef> part_turrets;
	std::vector<uint16_t> unheld_parts;
	std::vector<VehicleWeaponDef> weapons;
	size_t components = 0;

	float max_zone_health() const;
	const VehicleSeatDef *driver() const;
};

Result<VehicleDef> read_vehicle_def(const EbxResolver &p_db, std::string_view p_blueprint, WorkBudget *p_budget = nullptr);

Result<std::vector<VehicleInputModifierDef>> read_disabled_input(
		const EbxResolver &p_db, const EbxInstance &p_blueprint, const EbxInstance &p_entity, WorkBudget &r_budget);

float apply_input_modifiers(
		std::span<const VehicleInputModifierDef> p_modifiers, std::string_view p_action, float p_level);
}
