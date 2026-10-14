#pragma once
// SoldierDef: the movement, health and specialization tables of a SoldierBlueprint, read from EBX
// (plan §3.1, §7.2; research/topics/02-soldier-kits-modes.md §1, §2.3; probes/ebx §4).
//
// Chain: SoldierBlueprint (characters/soldiers/mpsoldier) -> SoldierEntityData {HealthModule ->
// VeniceSoldierHealthModuleData, CharacterPhysics -> CharacterPhysicsData (poses, states, sprint),
// CollisionInfo -> CollisionData (fall damage), SprintSettings, Components (body, suppression,
// spotting, 16 UnlockComponentData = specializations)}. A specialization is a ValueUnlockAsset whose
// UnlockValues pairs {Id, UnlockedValue} an UnlockComponentData feeds through Float/IntHub entities
// into component fields (the blueprint's PropertyConnections); target field names are resolved by
// hashing the target instance's own field names. Which output port carries which pair is not named
// in data (the port ids are designer hashes): a component with one target gets every pair, else a
// pair goes to the target whose field name shares the longest prefix with the pair Id
// (UnderslungBoostL1 -> UnderslungGrenadeIncrease), else to the target in the same position.
//
// Units as in the data: metres, seconds, m/s, degrees, kg, hit points.

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/result.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::core::gameplay {

enum class SoldierPose : uint8_t { Stand = 0, Crouch = 1, Prone = 2 };
inline constexpr size_t kSoldierPoseCount = 3;
// "CharacterPoseType_Stand" etc.; nullopt for other names.
std::optional<SoldierPose> soldier_pose_from_name(std::string_view p_name);
const char *soldier_pose_name(SoldierPose p_pose);

struct CurvePoint {
	float x = 0;
	float y = 0;
};

// Piecewise-linear evaluation: clamped to the first/last point outside the range; 0 for no points.
float eval_curve(const std::vector<CurvePoint> &p_points, float p_x);

struct PoseDef {
	SoldierPose pose = SoldierPose::Stand;
	float height = 0; // capsule height (prone is below 2 x radius: a footprint, not a capsule)
	float eye_height = 0; // EyePosition.y
	float step_height = 0;
	std::string collision_type; // CharacterPoseCollisionType_*
	std::array<float, 3> box_min_expand{};
	std::array<float, 3> box_max_expand{};
	float min_look_yaw = 0, max_look_yaw = 0, min_look_pitch = 0, max_look_pitch = 0;
	std::vector<CurvePoint> throttle_curve; // stick magnitude -> speed scale
	// Transition time to each pose, by SoldierPose index; nullopt = no transition listed.
	std::array<std::optional<float>, kSoldierPoseCount> transition_to{};
};

// One CharacterStatePoseInfo: the speed of a movement state in one pose.
struct StatePoseSpeed {
	SoldierPose pose = SoldierPose::Stand;
	float velocity = 0; // m/s at full forward input
	float forward = 1, backward = 1, left = 1, right = 1; // SpeedModifier constants
	float acceleration_gain = 0;
	float deceleration_gain = 0;
	float sprint_gain = 0;
	float sprint_multiplier = 0;
};

// A CharacterStateData (OnGround, Jump, InAir, Falling, Parachute, Swimming, Climbing, Sliding,
// AnimationControlled): its per-pose speeds and its own scalar fields (JumpHeight,
// FreeFallVelocity, TerminalVelocity, ...).
struct MovementStateDef {
	std::string type; // e.g. "OnGroundStateData"
	std::vector<StatePoseSpeed> poses;
	std::vector<std::pair<std::string, double>> scalars;

	const StatePoseSpeed *pose(SoldierPose p_pose) const;
	std::optional<double> scalar(std::string_view p_name) const;
};

// One UnlockValues pair of a specialization unlock.
struct SpecializationDef {
	std::string unlock; // ValueUnlockAsset partition, e.g. persistence/unlocks/soldiers/specializations/sprintboostl1
	std::string id; // the pair's Id, e.g. SprintBoostL1, UnderslungBoostL1
	double value = 0; // UnlockedValue
	double default_value = 0; // DefaultValue
	bool squad = false; // UnlockUserData {CheckSquad} (the L2 squad variant)
	std::string target_type; // e.g. SoldierBodyComponentData
	std::string target_field; // e.g. SprintMultiplier; empty when the chain did not resolve
};

struct SoldierDef {
	std::string blueprint; // partition name
	std::string physics; // CharacterPhysicsData partition

	// SoldierEntityData
	float max_health = 0;
	float low_health_threshold = 0;
	float explosion_damage_modifier = 1;
	float regeneration_delay_modifier = 1;
	bool use_prediction = false;

	// VeniceSoldierHealthModuleData
	float regeneration_delay = 0;
	float regeneration_rate = 0; // hp/s [inferred unit]
	float immortal_time_after_spawn = 0;
	std::vector<int32_t> abort_spawn_immortality_inputs; // EntryInputActionEnum values
	float man_down_time = 0;
	float man_down_health_points = 0;
	float post_revive_health = 0;
	float time_for_corpse = 0;
	bool interactive_man_down = false;

	// CharacterPhysicsData
	float mass = 0;
	float radius = 0; // PhysicalRadius
	float max_ascend_angle = 0; // walkable slope, degrees
	float water_depth_limit = 0;
	float jump_penalty_time = 0;
	float jump_penalty_factor = 0;
	float ladder_accept_angle = 0;
	float ladder_accept_angle_pitch = 0;
	float input_acceleration = 0;
	float radius_to_predict_collision_on_characters = 0;
	bool allow_pose_change_during_transition = false;
	std::string default_state;
	// CharacterSprintData
	bool continuous_sprint = false;
	float sprint_power_decrease_per_second = 0;
	float sprint_power_increase_per_second = 0;
	float sprint_minimum_power = 0;

	std::vector<PoseDef> poses;
	std::vector<MovementStateDef> states;

	// CollisionData: damage (hp) at impact speed (m/s)
	std::vector<CurvePoint> fall_damage_vertical;
	std::vector<CurvePoint> fall_damage_horizontal;

	// SoldierSprintSettingsData, SoldierBodyComponentData, SoldierSuppressionComponentData,
	// SpottingComponentData / SpottingTargetComponentData: their scalar fields.
	std::vector<std::pair<std::string, double>> sprint_settings;
	float body_sprint_multiplier = 1;
	std::vector<std::pair<std::string, double>> suppression;
	std::vector<std::pair<std::string, double>> spotting;
	std::vector<std::pair<std::string, double>> spotting_target;

	std::vector<SpecializationDef> specializations;

	const PoseDef *pose(SoldierPose p_pose) const;
	const MovementStateDef *state(std::string_view p_type) const;
	// OnGround velocity of a pose x its forward constant; sprint = x SprintMultiplier.
	std::optional<float> ground_speed(SoldierPose p_pose) const;
	std::optional<float> sprint_speed() const;
	// JumpStateData.JumpHeight.
	std::optional<float> jump_height() const;
	float fall_damage(float p_vertical_speed) const { return eval_curve(fall_damage_vertical, p_vertical_speed); }
	// The specialization pair with that Id (case-insensitive, e.g. "SprintBoostL1").
	const SpecializationDef *specialization(std::string_view p_id) const;
};

// Reads the SoldierBlueprint partition (default characters/soldiers/mpsoldier) through p_db.
// p_budget: the work budget to charge (a WorkBudget of the default limit when null).
Result<SoldierDef> read_soldier_def(const EbxResolver &p_db, std::string_view p_blueprint = "characters/soldiers/mpsoldier", WorkBudget *p_budget = nullptr);

} // namespace bf3::core::gameplay
