#pragma once

#include "bf3/core/math.h"
#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bf3::ai {

using core::Transform;
using core::Vec3;

struct Vec2 {
	float x = 0.0f;
	float y = 0.0f;
};

inline constexpr uint32_t kNoHandle = 0xFFFFFFFFu;

enum class SoldierPose : uint8_t { NotAvailable = 0, Standing = 1, Crouching = 2, Lying = 3, Swimming = 4 };

enum class InformationType : uint8_t {
	Production = 0,
	Ammunition = 1,
	Repair = 2,
	Unit = 3,
	Air = 4,
	Helicopter = 5,
	Ground = 6,
	Naval = 7,
	Controlpoint = 8,
	Radar = 9,
	Airfield = 10,
	Transportation = 11,
	Artillery = 12,
	Fixed = 13,
	Cover = 14,
	Mobile = 15,
	Unmanned = 16,
	LowPriority = 17,
	Structure = 18,
	Biological = 19,
	Vegetation = 20,
	Soldier = 21,
	ObstructedView = 22,
	NoTemperature = 23,
	HasWreck = 24,
	Obstacle = 25,
	HasCarHorn = 26,
};

class AiObject;

class AiWeapon {
public:
	virtual ~AiWeapon() = default;
	virtual const Transform &absolute_transformation() const = 0;
	virtual const Vec3 &position() const = 0;
	virtual const Transform &projectile_origin() const = 0;
	virtual int weapon_activate() const = 0;
	virtual int weapon_fire() const = 0;
	virtual int weapon_zoom() const = 0;
	virtual int weapon_alt_fire() const = 0;
	virtual bool is_zoomed() const = 0;
	virtual int weapon_state() const = 0;
	virtual float drag() const = 0;
	virtual float gravity_modifier() const = 0;
	virtual float exit_velocity() const = 0;
	virtual float min_range() const = 0;
	virtual float max_range() const = 0;
	virtual float optimal_range_percentage() const = 0;
	virtual SoldierPose firing_pose() const = 0;
	virtual float allowed_deviation() const = 0;
	virtual bool fires_through_transparent() const = 0;
	virtual const int *strengths() const = 0;
	virtual int strength(int p_index) const = 0;
	virtual float explosion_radius() const = 0;
	virtual bool is_thrown() const = 0;
	virtual bool use_aimer_only() const = 0;
	virtual int fire_rate() const = 0;
	virtual int ammo() const = 0;
	virtual int current_ammo() const = 0;
	virtual int ammo_in_mag(int p_mag) const = 0;
	virtual int max_ammo() const = 0;
	virtual int max_ammo_in_mag() const = 0;
	virtual int number_of_mags() const = 0;
	virtual bool has_auto_reload() const = 0;
	virtual float reload_time() const = 0;
	virtual int weapon_type() const = 0;
	virtual int weapon_guiding_system() const = 0;
	virtual sim::Tick time_of_firing() const = 0;
	virtual bool burst() const = 0;
	virtual bool indirect() const = 0;
	virtual float heat() const = 0;
	virtual bool has_heat() const = 0;
	virtual bool uses_heat() const = 0;
	virtual bool is_ready_to_fire() const = 0;
	virtual bool set_fake_fire(bool p_fake) = 0;
	virtual float sound_sphere_radius() const = 0;
	virtual int projectile_material() const = 0;
	virtual float replenish_radius() const = 0;
	virtual void set_bot_skill(float p_skill, sim::Tick p_since, float p_scale) = 0;
	virtual bool set_target(uint32_t p_target_id) = 0;
	virtual bool has_target_locked() const = 0;
	virtual AiObject *locked_target() const = 0;
	virtual const std::string &name() const = 0;
};

class AiObjectArmament {
public:
	virtual ~AiObjectArmament() = default;
	virtual int is_anti_aircraft() const = 0;
	virtual int active_weapon() const = 0;
	virtual void set_active_weapon(int p_index, int p_side) = 0;
	virtual AiWeapon *last_fired_weapon() const = 0;
	virtual const std::vector<AiWeapon *> &weapons() const = 0;
	virtual int weapon_id_from_activation_id(int p_activation, int p_fire) const = 0;
	virtual bool rearm(AiWeapon *p_weapon, float p_amount, int p_side) = 0;
	virtual bool projectile_position(Vec3 &r_out) const = 0;
	virtual bool projectile_speed(Vec3 &r_out) const = 0;
};

enum class PlayerInputMap : uint8_t {
	Yaw = 0,
	Pitch = 1,
	Roll = 2,
	Throttle = 3,
	MouseLookX = 4,
	MouseLookY = 5,
	Fire = 6,
	Action = 7,
	Use = 8,
	MouseLook = 9,
	Sprint = 10,
	WeaponSelect1 = 11,
	WeaponSelect2 = 12,
	WeaponSelect3 = 13,
	WeaponSelect4 = 14,
	WeaponSelect5 = 15,
	WeaponSelect6 = 16,
	WeaponSelect7 = 17,
	WeaponSelect8 = 18,
	WeaponSelect9 = 19,
	PositionSelect1 = 20,
	PositionSelect2 = 21,
	PositionSelect3 = 22,
	PositionSelect4 = 23,
	PositionSelect5 = 24,
	PositionSelect6 = 25,
	PositionSelect7 = 26,
	PositionSelect8 = 27,
	AltFire = 28,
	Reload = 29,
	Lie = 30,
	Crouch = 31,
	FlareFire = 32,
	None = 64
};

class AiObjectControlInfo {
public:
	virtual ~AiObjectControlInfo() = default;
	virtual uint64_t drive_controls() const = 0;
	virtual PlayerInputMap drive_throttle_control() const = 0;
	virtual PlayerInputMap drive_turn_control() const = 0;
	virtual PlayerInputMap drive_roll_control() const = 0;
	virtual PlayerInputMap drive_pitch_control() const = 0;
	virtual uint64_t aim_controls() const = 0;
	virtual PlayerInputMap aim_vertical_control() const = 0;
	virtual PlayerInputMap aim_horizontal_control() const = 0;
	virtual PlayerInputMap aim_throttle_control() const = 0;
	virtual PlayerInputMap aim_roll_control() const = 0;
	virtual uint64_t look_controls() const = 0;
	virtual PlayerInputMap look_vertical_control() const = 0;
	virtual PlayerInputMap look_horizontal_control() const = 0;
	virtual float throttle_sensitivity() const = 0;
	virtual float pitch_sensitivity() const = 0;
	virtual float roll_sensitivity() const = 0;
	virtual float yaw_sensitivity() const = 0;
	virtual float look_vertical_sensitivity() const = 0;
	virtual float look_horizontal_sensitivity() const = 0;
	virtual float proportional_constant() const = 0;
	virtual float derivative_constant() const = 0;
	virtual float angle_speed_constant() const = 0;
	virtual float throttle_look_ahead() const = 0;
	virtual float pitch_look_ahead() const = 0;
	virtual float roll_look_ahead() const = 0;
	virtual float yaw_look_ahead() const = 0;
	virtual float look_vertical_look_ahead() const = 0;
	virtual float look_horizontal_look_ahead() const = 0;
	virtual float throttle_scale() const = 0;
	virtual float pitch_scale() const = 0;
	virtual float roll_scale() const = 0;
	virtual float yaw_scale() const = 0;
	virtual float look_vertical_scale() const = 0;
	virtual float look_horizontal_scale() const = 0;
	virtual float max_speed_bumpiness_fall_off() const = 0;
	virtual float max_climb_angle() const = 0;
	virtual float max_roll_angle() const = 0;
	virtual float camera_relative_dof_rotation_offset() const = 0;
	virtual const Vec3 &camera_relative_min_rotation() const = 0;
	virtual const Vec3 &camera_relative_max_rotation() const = 0;
	virtual Vec3 camera_relative_rotation() const = 0;
	virtual const Transform &camera_transformation() const = 0;
	virtual Transform camera_base_transformation() const = 0;
	virtual bool validate_camera_direction(const Vec3 &p_direction) const = 0;
	virtual bool validate_camera_direction_yaw(const Vec3 &p_direction) const = 0;
	virtual bool validate_camera_direction_pitch(const Vec3 &p_direction) const = 0;
	virtual bool validate_camera_direction_by_pos(const Vec3 &p_position) const = 0;
};

class AiObjectCover {
public:
	virtual ~AiObjectCover() = default;
	virtual std::vector<Vec2> object_outline() const = 0;
	virtual std::vector<Vec2> local_object_outline() const = 0;
	virtual float cover_value() const = 0;
	virtual float height() const = 0;
	virtual float transparency() const = 0;
};

class AiObjectMobile {
public:
	virtual ~AiObjectMobile() = default;
	virtual const Vec3 &speed() const = 0;
	virtual const Vec3 &rotational_speed() const = 0;
	virtual int vehicle_number() const = 0;
	virtual float max_speed() const = 0;
	virtual float turn_radius() const = 0;
	virtual float drag() const = 0;
	virtual float cover_search_radius() const = 0;
	virtual bool is_turnable() const = 0;
	virtual float sound_sphere_radius() const = 0;
	virtual float sound_sphere_radius_min() const = 0;
	virtual float sound_sphere_radius_max() const = 0;
	virtual float hearing_probability_max() const = 0;
	virtual float hearing_probability_min() const = 0;
	virtual float avoid_collision_look_ahead() const = 0;
	virtual AiObject *sector() const = 0;
	virtual int last_passed_portal() const = 0;
	virtual float sprint_stamina() const = 0;
};

class AiObjectPhysical {
public:
	virtual ~AiObjectPhysical() = default;
	virtual float health() const = 0;
	virtual float max_health() const = 0;
	virtual float mass() const = 0;
	virtual float volume() const = 0;
	virtual float armour(const Vec3 &p_direction) const = 0;
	virtual float best_armour() const = 0;
	virtual float radius() const = 0;
	virtual Vec3 sphere_local_offset() const = 0;
	virtual float smallest_radius() const = 0;
	virtual void sub_spheres_2d(std::vector<Vec2> &r_out) const = 0;
	virtual int strength_type() const = 0;
	virtual bool is_touching_terrain(Vec3 &r_position) const = 0;
	virtual bool is_in_air() const = 0;
	virtual bool is_in_water() const = 0;
	virtual int material() const = 0;
	virtual float man_down_time_left() const = 0;
	virtual float max_man_down_time() const = 0;
	virtual bool man_down() const = 0;
};

class AiObjectUnit {
public:
	virtual ~AiObjectUnit() = default;
	virtual int max_ammo() const = 0;
	virtual const int *battle_strengths() const = 0;
	virtual const int *strategic_strengths() const = 0;
	virtual int equipment_type() const = 0;
	virtual bool use_no_pathfinding_to_get_to_object() const = 0;
	virtual bool has_exposed_soldier() const = 0;
	virtual bool always_target_soldier() const = 0;
	virtual bool is_occupied() const = 0;
	virtual float cull_distance() const = 0;
	virtual float cull_distance_sqr() const = 0;
	virtual float entry_radius() const = 0;
	virtual AiObject *occupier() const = 0;
	virtual AiObject *occupied_object() const = 0;
	virtual sim::Tick last_player_possession_tick() const = 0;
	virtual int team_lock() const = 0;
	virtual bool must_borrow_armament(const AiObject *p_vehicle) const = 0;
	virtual int occupying_player_id() const = 0;
	virtual void set_missile_lock_status(int p_status) = 0;
	virtual int missile_lock_status() const = 0;
	virtual const Vec3 &entry_position_of_non_mobile() const = 0;
};

class AiObject {
public:
	virtual ~AiObject() = default;
	virtual const std::string &name() const = 0;
	virtual const std::string &ai_name() const = 0;
	virtual const std::string &object_type_name() const = 0;
	virtual AiObjectArmament *armament() const = 0;
	virtual AiObjectCover *cover() const = 0;
	virtual AiObjectMobile *mobile() const = 0;
	virtual AiObjectPhysical *physical() const = 0;
	virtual AiObjectUnit *unit() const = 0;
	virtual AiObjectControlInfo *control_info() const { return nullptr; }
	virtual uint32_t information(int p_side) const = 0;
	virtual const Vec3 &position() const = 0;
	virtual const Transform &transformation() const = 0;
	virtual uint32_t object_id() const = 0;
	virtual SoldierPose soldier_pose() const = 0;
	virtual int kit_type() const = 0;
	virtual bool is_primary() const = 0;
	virtual AiObject *primary_object() const = 0;
	virtual AiObject *secondary_object(int &r_iterator) const = 0;
	virtual AiObject *first_secondary_object(int &r_iterator) const = 0;
	virtual int first_secondary_object_iterator() const = 0;
	virtual int side() const = 0;
	virtual bool object_spawner_holding() const = 0;
	virtual AiObject *object_spawner() const = 0;
	virtual bool is_root_object() const = 0;
	virtual int player_id() const = 0;
};

}
