#pragma once

#include "bf3/core/ids.h"
#include "bf3/core/math.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::gameplay {

enum class FireLogic : uint8_t {
	Single = 0,
	SingleWithBoltAction = 1,
	Automatic = 2,
	Burst = 3,
	HoldAndRelease = 4,
	Detonated = 5,
	Unknown = 255,
};

enum class ReloadType : uint8_t {
	SingleBullet = 0,
	Magazine = 1,
	MagazineWithPossibleShorterReload = 2,
	Unknown = 255,
};

enum class Stance : uint8_t { Stand = 0, Crouch = 1, Prone = 2 };
enum class Zoom : uint8_t { NoZoom = 0, Zoom = 1 };

enum class SwayMove : uint8_t { Base = 0, Moving, Jumping, Sprinting, VaultingSmall, VaultingMedium, Count };

enum class StanceTransition : uint8_t { ProneToCrouch = 0, ProneToStand, CrouchToProne, CrouchToStand, StandToCrouch, StandToProne, Count };

enum class ProjectileKind : uint8_t { None, Bullet, Missile, Grenade, Other };

using Vec3 = core::Vec3;

struct Dispersion {
	float min_angle = 0;
	float max_angle = 0;
	float increase_per_shot = 0;
	float decrease_per_second = 0;
};

struct Recoil {
	float up_per_shot = 0;
	float up_max = 0;
	float horizontal_min = 0;
	float horizontal_max = 0;
	float horizontal_amplitude_max = 0;
	float decrease_factor = 0;
};

struct SwayModifier {
	float min_angle = 1;
	float max_angle = 1;
	float increase_per_shot = 1;
	float decrease_per_second = 1;
	float recoil_magnitude = 1;
	float recoil_angle = 1;
	float lag_yaw = 1;
	float lag_pitch = 1;
};

struct SwayState {
	std::array<Dispersion, size_t(SwayMove::Count)> move{};
	std::array<bool, size_t(SwayMove::Count)> has_move{};
	Recoil recoil;
};

struct SwayGatedModifier {
	std::string unlock;
	Guid unlock_instance;
	bool only_in_supported_shooting = false;
	bool only_on_weapon_light_enabled = false;
	std::array<std::array<SwayModifier, 2>, 3> mod{};
};

struct GunSway {
	std::string partition;
	std::array<std::array<SwayState, 2>, 3> state{};
	struct Transition {
		Dispersion max_penalty;
		float cool_down = 0;
	};
	std::array<Transition, size_t(StanceTransition::Count)> transitions{};
	std::array<SwayModifier, 2> suppression{};
	float first_shot_recoil_multiplier = 1;
	float shooting_recoil_decrease_scale = 0;
	std::array<float, 2> deviation_scale{ 1, 1 };
	std::array<float, 2> gameplay_deviation_scale{ 1, 1 };

	float camera_spring_constant = 0;
	float camera_spring_damping = 0;
	float camera_spring_min_threshold_angle = 0;

	std::vector<SwayGatedModifier> applied;

	std::vector<SwayGatedModifier> conditional;
	size_t modifiers_total = 0;
	size_t modifiers_unresolved = 0;
};

struct FireFunction {
	FireLogic logic = FireLogic::Unknown;
	std::vector<FireLogic> extra_modes;
	float rate_of_fire = 0;
	float rate_of_fire_for_burst = 0;
	float client_fire_rate_multiplier = 0;
	float pre_fire_delay = 0;
	float automatic_delay = 0;
	float trigger_pull_weight = 0;

	float bolt_action_delay = 0;
	float bolt_action_time = 0;
	bool unzoom_on_bolt_action = false;
	bool return_to_zoom_after_bolt_action = false;
	bool hold_bolt_action_until_fire_release = false;
	bool hold_bolt_action_until_zoom_release = false;
	bool force_bolt_action_on_fire_trigger = false;

	float hold_delay = 0;
	float hold_max_time = 0;
	float hold_min_power = 0;
	float hold_max_power = 0;
	float hold_power_per_second = 0;
	float hold_killed_power_modifier = 0;
	bool hold_force_fire_when_killed = false;

	ReloadType reload_type = ReloadType::Unknown;
	float reload_time = 0;
	float reload_time_bullets_left = 0;
	float reload_delay = 0;
	float reload_threshold = 0;
	bool always_auto_reload = false;
	std::string reload_logic;

	int32_t magazine_capacity = 0;
	int32_t number_of_magazines = 0;
	int32_t trace_frequency = 0;
	float auto_replenish_delay = 0;

	Vec3 initial_position{};
	Vec3 initial_direction{};
	Vec3 initial_speed{};
	int32_t bullets_per_shell = 0;
	int32_t bullets_per_shot = 0;
	int32_t bullets_per_burst = 0;
	float spawn_delay = 0;
	float inherit_weapon_speed = 0;
	bool active_force_spawn_to_camera = false;
	bool force_spawn_to_camera = false;

	float heat_per_bullet = 0;
	float heat_drop_per_second = 0;
	float overheat_penalty_time = 0;
	float overheat_threshold = 0;

	std::array<Dispersion, 3> weapon_dispersion{};
	float jump_dispersion_angle = 0;
	float move_dispersion_angle = 0;
	float move_zoomed_dispersion_angle = 0;
	float weapon_dispersion_decrease_per_second = 0;
	size_t firing_dispersions = 0;

	bool fire_logic_recoil_nonzero = false;

	bool hold_off_reload_until_fire_release = false;
	bool force_reload_action_on_fire_trigger = false;
};

struct MaterialPairRef {
	bool present = false;
	bool resolved = false;
	Guid partition;
	Guid instance;
	uint32_t flags_and_index = 0;
	int32_t physics_material_index = 0;
	int32_t physics_property_index = 0;
	uint8_t property() const { return uint8_t(physics_property_index & 0xFF); }
	uint8_t material() const { return uint8_t(physics_material_index & 0xFF); }
	bool penetrable() const { return ((flags_and_index >> 24) & 1u) != 0; }
};

struct Explosion {
	bool present = false;
	std::string partition;
	float blast_damage = 0;
	float blast_radius = 0;
	float inner_blast_radius = 0;
	float blast_impulse = 0;
	float shockwave_damage = 0;
	float shockwave_radius = 0;
	float shockwave_impulse = 0;
	float shockwave_time = 0;
	float max_occlusion_raycast_radius = 0;
	bool disable_occlusion = false;
	bool disable_static_entity_occlusion = false;
	bool is_causing_suppression = false;

	MaterialPairRef material;
};

struct Suppression {
	bool present = false;
	float min_multiplier = 0;
	float max_multiplier = 0;
	float min_distance = 0;
	float max_distance = 0;
};

struct ProjectileDef {
	ProjectileKind kind = ProjectileKind::None;
	std::string type;
	std::string partition;
	std::string ammunition_type;
	float gravity = 0;
	float time_to_live = 0;
	float record_initial_speed = 0;
	float visual_converge_distance = 0;
	float impact_impulse = 0;
	bool detonate_on_timeout = false;
	bool server_projectile_disabled = false;

	float start_damage = 0;
	float end_damage = 0;
	float damage_falloff_start = 0;
	float damage_falloff_end = 0;
	float time_to_arm_explosion = 0;
	float distribute_damage_over_time = 0;
	bool instant_hit = false;

	float missile_damage = 0;
	float engine_strength = 0;
	float max_speed = 0;
	float engine_time_to_ignition = 0;
	float engine_time_to_live = 0;
	float time_to_arm = 0;
	float drag = 0;
	float max_turn_angle = 0;
	float time_to_activate_guiding = 0;

	float collision_speed_multiplier = 0;
	float min_bounce_speed = 0;
	std::string grenade_type;
	MaterialPairRef material;
	Explosion explosion;
	Explosion dud_explosion;
	Suppression suppression;
};

struct WeaponDef {

	std::string variant;
	uint32_t unlock_identifier = 0;
	std::string blueprint;
	std::string blueprint_name;
	std::string damage_giver_name;
	std::string name_sid;
	uint32_t name_sid_hash = 0;
	std::vector<std::string> active_unlocks;
	std::vector<std::string> applied_modifiers;

	std::string weapon_class;
	std::string anim_base_set;
	std::string weapon_anim_type;
	std::string aiming_controller;
	std::string sound;
	float render_fov = 0;
	float zoom_render_fov = 0;
	bool silenced = false;
	bool enable_breath_control = false;
	bool can_be_in_supported_shooting = false;
	bool allow_switching_out_of_ammo = true;

	std::string firing_data;
	float deploy_time = 0;
	float alt_deploy_time = 0;
	float disable_zoom_on_deploy_time = 0;
	float support_delay_stand = 0;
	float support_delay_prone = 0;
	bool reload_whole_mags = false;
	FireFunction fire;
	ProjectileDef projectile;
	GunSway sway;
};

const char *fire_logic_name(FireLogic p_logic);
const char *reload_type_name(ReloadType p_type);
const char *projectile_kind_name(ProjectileKind p_kind);
FireLogic fire_logic_from(int32_t p_value);

double shot_interval(const FireFunction &p_fire);

double burst_interval(const FireFunction &p_fire);

double effective_rpm(const FireFunction &p_fire);

double muzzle_speed(const WeaponDef &p_weapon);

double damage_at(const ProjectileDef &p_projectile, double p_distance);

double drop_at(double p_speed, double p_gravity, double p_distance);

double max_range(const WeaponDef &p_weapon);

uint32_t loc_sid_hash(std::string_view p_sid);

void apply_sway_modifier(std::array<std::array<SwayState, 2>, 3> &r_state, const SwayGatedModifier &p_modifier);

}
