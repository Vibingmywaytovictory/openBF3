#pragma once

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

std::optional<SoldierPose> soldier_pose_from_name(std::string_view p_name);
const char *soldier_pose_name(SoldierPose p_pose);

struct CurvePoint {
	float x = 0;
	float y = 0;
};

float eval_curve(const std::vector<CurvePoint> &p_points, float p_x);

struct PoseDef {
	SoldierPose pose = SoldierPose::Stand;
	float height = 0;
	float eye_height = 0;
	float step_height = 0;
	std::string collision_type;
	std::array<float, 3> box_min_expand{};
	std::array<float, 3> box_max_expand{};
	float min_look_yaw = 0, max_look_yaw = 0, min_look_pitch = 0, max_look_pitch = 0;
	std::vector<CurvePoint> throttle_curve;

	std::array<std::optional<float>, kSoldierPoseCount> transition_to{};
};

struct StatePoseSpeed {
	SoldierPose pose = SoldierPose::Stand;
	float velocity = 0;
	float forward = 1, backward = 1, left = 1, right = 1;
	float acceleration_gain = 0;
	float deceleration_gain = 0;
	float sprint_gain = 0;
	float sprint_multiplier = 0;
};

struct MovementStateDef {
	std::string type;
	std::vector<StatePoseSpeed> poses;
	std::vector<std::pair<std::string, double>> scalars;

	const StatePoseSpeed *pose(SoldierPose p_pose) const;
	std::optional<double> scalar(std::string_view p_name) const;
};

struct SpecializationDef {
	std::string unlock;
	std::string id;
	double value = 0;
	double default_value = 0;
	bool squad = false;
	std::string target_type;
	std::string target_field;
};

struct SoldierDef {
	std::string blueprint;
	std::string physics;

	float max_health = 0;
	float low_health_threshold = 0;
	float explosion_damage_modifier = 1;
	float regeneration_delay_modifier = 1;
	bool use_prediction = false;

	float regeneration_delay = 0;
	float regeneration_rate = 0;
	float immortal_time_after_spawn = 0;
	std::vector<int32_t> abort_spawn_immortality_inputs;
	float man_down_time = 0;
	float man_down_health_points = 0;
	float post_revive_health = 0;
	float time_for_corpse = 0;
	bool interactive_man_down = false;

	float mass = 0;
	float radius = 0;
	float max_ascend_angle = 0;
	float water_depth_limit = 0;
	float jump_penalty_time = 0;
	float jump_penalty_factor = 0;
	float ladder_accept_angle = 0;
	float ladder_accept_angle_pitch = 0;
	float input_acceleration = 0;
	float radius_to_predict_collision_on_characters = 0;
	bool allow_pose_change_during_transition = false;
	std::string default_state;

	bool continuous_sprint = false;
	float sprint_power_decrease_per_second = 0;
	float sprint_power_increase_per_second = 0;
	float sprint_minimum_power = 0;

	std::vector<PoseDef> poses;
	std::vector<MovementStateDef> states;

	std::vector<CurvePoint> fall_damage_vertical;
	std::vector<CurvePoint> fall_damage_horizontal;

	std::vector<std::pair<std::string, double>> sprint_settings;
	float body_sprint_multiplier = 1;
	std::vector<std::pair<std::string, double>> suppression;
	std::vector<std::pair<std::string, double>> spotting;
	std::vector<std::pair<std::string, double>> spotting_target;

	std::vector<SpecializationDef> specializations;

	const PoseDef *pose(SoldierPose p_pose) const;
	const MovementStateDef *state(std::string_view p_type) const;

	std::optional<float> ground_speed(SoldierPose p_pose) const;
	std::optional<float> sprint_speed() const;

	std::optional<float> jump_height() const;
	float fall_damage(float p_vertical_speed) const { return eval_curve(fall_damage_vertical, p_vertical_speed); }

	const SpecializationDef *specialization(std::string_view p_id) const;
};

Result<SoldierDef> read_soldier_def(const EbxResolver &p_db, std::string_view p_blueprint = "characters/soldiers/mpsoldier", WorkBudget *p_budget = nullptr);

}
