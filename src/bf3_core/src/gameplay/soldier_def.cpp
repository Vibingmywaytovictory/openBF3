// SoldierDef reader. Field map from research/topics/02-soldier-kits-modes.md §1 and the EBX probe
// §4; the specialization wiring follows the same PropertyConnections walk as logic_world.cpp.
#include "bf3/core/gameplay/soldier_def.h"

#include "bf3/core/gameplay/logic_world.h"
#include "bf3/core/ids.h"
#include "gameplay_read.h"

#include <algorithm>

namespace bf3::core::gameplay {

using namespace detail;

namespace {

constexpr size_t kComponentDepth = 16;
constexpr size_t kComponentLimit = 4096;
constexpr size_t kHubHops = 4;

std::vector<CurvePoint> read_vec2_curve(const EbxStruct &p_object, std::string_view p_field) {
	std::vector<CurvePoint> out;
	const std::optional<EbxValue> v = p_object.get(p_field);
	const OptionalEbxArray array = v ? v->as_array() : OptionalEbxArray();
	if (!array) {
		return out;
	}
	for (const EbxValue point : *array) {
		const std::optional<EbxValue> x = point.get("x");
		const std::optional<EbxValue> y = point.get("y");
		if (x && y && x->as_f32() && y->as_f32()) {
			out.push_back({ *x->as_f32(), *y->as_f32() });
		}
	}
	return out;
}

// ValueAtX {X, Value} lists (CollisionData).
std::vector<CurvePoint> read_value_at_x(const EbxStruct &p_object, std::string_view p_field) {
	std::vector<CurvePoint> out;
	const std::optional<EbxValue> v = p_object.get(p_field);
	const OptionalEbxArray array = v ? v->as_array() : OptionalEbxArray();
	if (!array) {
		return out;
	}
	for (const EbxValue point : *array) {
		const std::optional<EbxValue> x = point.get("X");
		const std::optional<EbxValue> y = point.get("Value");
		const std::optional<double> nx = x ? number(*x) : std::nullopt;
		const std::optional<double> ny = y ? number(*y) : std::nullopt;
		if (nx && ny) {
			out.push_back({ static_cast<float>(*nx), static_cast<float>(*ny) });
		}
	}
	return out;
}

PoseDef read_pose(const EbxInstance &p_pose) {
	PoseDef out;
	const EbxStruct d = p_pose.data();
	out.pose = soldier_pose_from_name(text(d, "PoseType")).value_or(SoldierPose::Stand);
	out.height = static_cast<float>(number_or(d, "Height", 0));
	out.eye_height = static_cast<float>(number_or(d, "EyePosition.y", 0));
	out.step_height = static_cast<float>(number_or(d, "StepHeight", 0));
	out.collision_type = text(d, "CollisionType");
	out.box_min_expand = vec3(d, "CollisionBoxMinExpand").value_or(std::array<float, 3>{});
	out.box_max_expand = vec3(d, "CollisionBoxMaxExpand").value_or(std::array<float, 3>{});
	out.min_look_yaw = static_cast<float>(number_or(d, "LookConstraints.MinLookYaw", 0));
	out.max_look_yaw = static_cast<float>(number_or(d, "LookConstraints.MaxLookYaw", 0));
	out.min_look_pitch = static_cast<float>(number_or(d, "LookConstraints.MinLookPitch", 0));
	out.max_look_pitch = static_cast<float>(number_or(d, "LookConstraints.MaxLookPitch", 0));
	out.throttle_curve = read_vec2_curve(d, "ThrottleModifierCurve");
	const std::optional<EbxValue> transitions = d.get("TransitionTimes");
	if (const OptionalEbxArray array = transitions ? transitions->as_array() : OptionalEbxArray()) {
		for (const EbxValue t : *array) {
			const std::optional<EbxStruct> s = t.as_struct();
			if (!s) {
				continue;
			}
			const std::optional<SoldierPose> to = soldier_pose_from_name(text(*s, "ToPose"));
			const std::optional<double> time = number(*s, "TransitionTime");
			if (to && time) {
				out.transition_to[static_cast<size_t>(*to)] = static_cast<float>(*time);
			}
		}
	}
	return out;
}

StatePoseSpeed read_state_pose(const EbxInstance &p_info) {
	StatePoseSpeed out;
	const EbxStruct d = p_info.data();
	out.pose = soldier_pose_from_name(text(d, "PoseType")).value_or(SoldierPose::Stand);
	out.velocity = static_cast<float>(number_or(d, "Velocity", 0));
	out.forward = static_cast<float>(number_or(d, "SpeedModifier.ForwardConstant", 1));
	out.backward = static_cast<float>(number_or(d, "SpeedModifier.BackwardConstant", 1));
	out.left = static_cast<float>(number_or(d, "SpeedModifier.LeftConstant", 1));
	out.right = static_cast<float>(number_or(d, "SpeedModifier.RightConstant", 1));
	out.acceleration_gain = static_cast<float>(number_or(d, "AccelerationGain", 0));
	out.deceleration_gain = static_cast<float>(number_or(d, "DecelerationGain", 0));
	out.sprint_gain = static_cast<float>(number_or(d, "SprintGain", 0));
	out.sprint_multiplier = static_cast<float>(number_or(d, "SprintMultiplier", 0));
	return out;
}

// Every component under p_object.Components, depth first, bounded.
void collect_components(const EbxResolver &p_db, const EbxInstance &p_object, size_t p_depth, std::vector<EbxInstance> &r_out) {
	if (p_depth > kComponentDepth) {
		return;
	}
	for (const EbxInstance &c : p_db.follow_all(p_object.data(), "Components")) {
		if (r_out.size() >= kComponentLimit) {
			return;
		}
		r_out.push_back(c);
		collect_components(p_db, c, p_depth + 1, r_out);
	}
}

std::optional<EbxInstance> first_of(const std::vector<EbxInstance> &p_components, std::string_view p_type) {
	for (const EbxInstance &c : p_components) {
		if (c.is_a(p_type)) {
			return c;
		}
	}
	return std::nullopt;
}

std::string field_name_of(const EbxInstance &p_target, int32_t p_field) {
	for (const EbxField &f : p_target.fields()) {
		if (field_id(f.name) == p_field) {
			return std::string(f.name);
		}
	}
	return {};
}

struct UnlockTarget {
	std::string type;
	std::string field;
};

// Where an UnlockComponentData's values land: through *HubEntityData outputs to the first non-hub
// targets of property connections, in connection order.
std::vector<UnlockTarget> unlock_targets(const LogicConnections &p_connections, const EbxInstance &p_unlock) {
	std::vector<UnlockTarget> out;
	std::vector<EbxInstance> frontier{ p_unlock };
	for (size_t hop = 0; hop < kHubHops && !frontier.empty(); ++hop) {
		std::vector<EbxInstance> next;
		for (const LogicConnection &c : p_connections.property) {
			if (std::find(frontier.begin(), frontier.end(), c.source) == frontier.end()) {
				continue;
			}
			if (c.target.type_name().ends_with("HubEntityData")) {
				if (std::find(next.begin(), next.end(), c.target) == next.end()) {
					next.push_back(c.target);
				}
				continue;
			}
			out.push_back({ std::string(c.target.type_name()), field_name_of(c.target, c.target_field) });
		}
		frontier = std::move(next);
	}
	return out;
}

size_t common_prefix(std::string_view p_a, std::string_view p_b) {
	size_t n = 0;
	const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
	while (n < p_a.size() && n < p_b.size() && lower(p_a[n]) == lower(p_b[n])) {
		++n;
	}
	return n;
}

} // namespace

std::optional<SoldierPose> soldier_pose_from_name(std::string_view p_name) {
	if (p_name.ends_with("_Stand") || p_name == "Stand") {
		return SoldierPose::Stand;
	}
	if (p_name.ends_with("_Crouch") || p_name == "Crouch") {
		return SoldierPose::Crouch;
	}
	if (p_name.ends_with("_Prone") || p_name == "Prone") {
		return SoldierPose::Prone;
	}
	return std::nullopt;
}

const char *soldier_pose_name(SoldierPose p_pose) {
	switch (p_pose) {
		case SoldierPose::Stand:
			return "stand";
		case SoldierPose::Crouch:
			return "crouch";
		case SoldierPose::Prone:
			return "prone";
	}
	return "?";
}

float eval_curve(const std::vector<CurvePoint> &p_points, float p_x) {
	if (p_points.empty()) {
		return 0;
	}
	if (p_x <= p_points.front().x) {
		return p_points.front().y;
	}
	for (size_t i = 1; i < p_points.size(); ++i) {
		const CurvePoint &a = p_points[i - 1];
		const CurvePoint &b = p_points[i];
		if (p_x <= b.x) {
			const float span = b.x - a.x;
			if (!(span > 0)) {
				return b.y;
			}
			return a.y + (b.y - a.y) * ((p_x - a.x) / span);
		}
	}
	return p_points.back().y;
}

const StatePoseSpeed *MovementStateDef::pose(SoldierPose p_pose) const {
	for (const StatePoseSpeed &p : poses) {
		if (p.pose == p_pose) {
			return &p;
		}
	}
	return nullptr;
}

std::optional<double> MovementStateDef::scalar(std::string_view p_name) const {
	for (const auto &[name, value] : scalars) {
		if (name == p_name) {
			return value;
		}
	}
	return std::nullopt;
}

const PoseDef *SoldierDef::pose(SoldierPose p_pose) const {
	for (const PoseDef &p : poses) {
		if (p.pose == p_pose) {
			return &p;
		}
	}
	return nullptr;
}

const MovementStateDef *SoldierDef::state(std::string_view p_type) const {
	for (const MovementStateDef &s : states) {
		if (s.type == p_type) {
			return &s;
		}
	}
	return nullptr;
}

std::optional<float> SoldierDef::ground_speed(SoldierPose p_pose) const {
	const MovementStateDef *s = state("OnGroundStateData");
	const StatePoseSpeed *p = s ? s->pose(p_pose) : nullptr;
	return p ? std::optional<float>(p->velocity * p->forward) : std::nullopt;
}

std::optional<float> SoldierDef::sprint_speed() const {
	const MovementStateDef *s = state("OnGroundStateData");
	const StatePoseSpeed *p = s ? s->pose(SoldierPose::Stand) : nullptr;
	return p ? std::optional<float>(p->velocity * p->forward * p->sprint_multiplier) : std::nullopt;
}

std::optional<float> SoldierDef::jump_height() const {
	const MovementStateDef *s = state("JumpStateData");
	const std::optional<double> h = s ? s->scalar("JumpHeight") : std::nullopt;
	return h ? std::optional<float>(static_cast<float>(*h)) : std::nullopt;
}

const SpecializationDef *SoldierDef::specialization(std::string_view p_id) const {
	for (const SpecializationDef &s : specializations) {
		if (iequals_ascii(s.id, p_id)) {
			return &s;
		}
	}
	return nullptr;
}

Result<SoldierDef> read_soldier_def(const EbxResolver &p_db, std::string_view p_blueprint) {
	const std::string name = to_lower_ascii(p_blueprint);
	const EbxPartition *partition = p_db.partition_by_name(name);
	if (!partition) {
		return make_error("soldier: no partition `" + name + "`");
	}
	const EbxInstance blueprint = partition->primary();
	const std::optional<EbxInstance> entity = p_db.follow(blueprint.data(), "Object");
	if (!entity || !entity->is_a("SoldierEntityData")) {
		return make_error("soldier: `" + name + "` has no SoldierEntityData (primary " + std::string(blueprint.type_name()) + ")");
	}
	SoldierDef out;
	out.blueprint = name;
	const EbxStruct e = entity->data();
	out.max_health = static_cast<float>(number_or(e, "MaxHealth", 0));
	out.low_health_threshold = static_cast<float>(number_or(e, "LowHealthThreshold", 0));
	out.explosion_damage_modifier = static_cast<float>(number_or(e, "ExplosionDamageModifier", 1));
	out.regeneration_delay_modifier = static_cast<float>(number_or(e, "RegenerationDelayModifier", 1));
	out.use_prediction = boolean(e, "UsePrediction").value_or(false);

	if (const std::optional<EbxInstance> health = p_db.follow(e, "HealthModule")) {
		const EbxStruct h = health->data();
		out.regeneration_delay = static_cast<float>(number_or(h, "RegenerationDelay", 0));
		out.regeneration_rate = static_cast<float>(number_or(h, "RegenerationRate", 0));
		out.immortal_time_after_spawn = static_cast<float>(number_or(h, "ImmortalTimeAfterSpawn", 0));
		out.man_down_time = static_cast<float>(number_or(h, "ManDownStateTime", 0));
		out.man_down_health_points = static_cast<float>(number_or(h, "ManDownStateHealthPoints", 0));
		out.post_revive_health = static_cast<float>(number_or(h, "PostReviveHealth", 0));
		out.time_for_corpse = static_cast<float>(number_or(h, "TimeForCorpse", 0));
		out.interactive_man_down = boolean(h, "InteractiveManDown").value_or(false);
		const std::optional<EbxValue> inputs = h.get("AbortSpawnImmortalityInputs");
		if (const OptionalEbxArray array = inputs ? inputs->as_array() : OptionalEbxArray()) {
			for (const EbxValue v : *array) {
				if (const std::optional<int32_t> i = v.as_i32()) {
					out.abort_spawn_immortality_inputs.push_back(*i);
				}
			}
		}
	}

	if (const std::optional<EbxInstance> physics = p_db.follow(e, "CharacterPhysics")) {
		const EbxStruct p = physics->data();
		out.physics = partition_name(p_db, *physics);
		out.mass = static_cast<float>(number_or(p, "Mass", 0));
		out.radius = static_cast<float>(number_or(p, "PhysicalRadius", 0));
		out.max_ascend_angle = static_cast<float>(number_or(p, "MaxAscendAngle", 0));
		out.water_depth_limit = static_cast<float>(number_or(p, "WaterDepthLimit", 0));
		out.jump_penalty_time = static_cast<float>(number_or(p, "JumpPenaltyTime", 0));
		out.jump_penalty_factor = static_cast<float>(number_or(p, "JumpPenaltyFactor", 0));
		out.ladder_accept_angle = static_cast<float>(number_or(p, "LadderAcceptAngle", 0));
		out.ladder_accept_angle_pitch = static_cast<float>(number_or(p, "LadderAcceptAnglePitch", 0));
		out.input_acceleration = static_cast<float>(number_or(p, "InputAcceleration", 0));
		out.radius_to_predict_collision_on_characters = static_cast<float>(number_or(p, "RadiusToPredictCollisionOnCharacters", 0));
		out.allow_pose_change_during_transition = boolean(p, "AllowPoseChangeDuringTransition").value_or(false);
		out.default_state = text(p, "DefaultState");
		if (const std::optional<EbxInstance> sprint = p_db.follow(p, "Sprint")) {
			const EbxStruct s = sprint->data();
			out.continuous_sprint = boolean(s, "AllowContinousSprinting").value_or(false);
			out.sprint_power_decrease_per_second = static_cast<float>(number_or(s, "SprintPowerDecreasePerSecond", 0));
			out.sprint_power_increase_per_second = static_cast<float>(number_or(s, "SprintPowerIncreasePerSecond", 0));
			out.sprint_minimum_power = static_cast<float>(number_or(s, "SprintMinimumPower", 0));
		}
		for (const EbxInstance &pose : p_db.follow_all(p, "Poses")) {
			out.poses.push_back(read_pose(pose));
		}
		for (const EbxInstance &state : p_db.follow_all(p, "States")) {
			MovementStateDef def;
			def.type = std::string(state.type_name());
			for (const EbxInstance &info : p_db.follow_all(state.data(), "PoseInfo")) {
				def.poses.push_back(read_state_pose(info));
			}
			def.scalars = scalars(state);
			out.states.push_back(std::move(def));
		}
	}

	if (const std::optional<EbxInstance> collision = p_db.follow(e, "CollisionInfo")) {
		out.fall_damage_vertical = read_value_at_x(collision->data(), "DamageAtVerticalVelocity");
		out.fall_damage_horizontal = read_value_at_x(collision->data(), "DamageAtHorizVelocity");
	}
	if (const std::optional<EbxInstance> sprint = p_db.follow(e, "SprintSettings")) {
		out.sprint_settings = scalars(*sprint);
	}

	std::vector<EbxInstance> components;
	collect_components(p_db, *entity, 0, components);
	if (const std::optional<EbxInstance> body = first_of(components, "SoldierBodyComponentData")) {
		out.body_sprint_multiplier = static_cast<float>(number_or(body->data(), "SprintMultiplier", 1));
	}
	if (const std::optional<EbxInstance> c = first_of(components, "SoldierSuppressionComponentData")) {
		out.suppression = scalars(*c);
		if (const std::optional<EbxValue> reaction = c->get("ReactionToSuppression"); reaction && reaction->as_struct()) {
			for (const char *f : { "SuppressionLowThreshold", "SuppressionHighThreshold", "SuppressionUIThreshold" }) {
				if (const std::optional<double> v = number(*reaction->as_struct(), f)) {
					out.suppression.emplace_back(f, *v);
				}
			}
		}
	}
	if (const std::optional<EbxInstance> c = first_of(components, "SpottingComponentData")) {
		out.spotting = scalars(*c);
	}
	if (const std::optional<EbxInstance> c = first_of(components, "SpottingTargetComponentData")) {
		out.spotting_target = scalars(*c);
	}

	const LogicConnections connections = LogicConnections::of(p_db, blueprint);
	for (const EbxInstance &unlock : components) {
		if (!unlock.is_a("UnlockComponentData")) {
			continue;
		}
		const std::optional<EbxInstance> asset = p_db.follow(unlock.data(), "UnlockAsset");
		if (!asset || !asset->is_a("ValueUnlockAsset")) {
			continue;
		}
		bool squad = false;
		if (const std::optional<EbxInstance> user = p_db.follow(asset->data(), "UnlockUserData")) {
			squad = boolean(user->data(), "CheckSquad").value_or(false);
		}
		const std::vector<UnlockTarget> targets = unlock_targets(connections, unlock);
		const std::vector<EbxInstance> pairs = p_db.follow_all(asset->data(), "UnlockValues");
		for (size_t i = 0; i < pairs.size(); ++i) {
			SpecializationDef spec;
			spec.unlock = partition_name(p_db, *asset);
			spec.id = text(pairs[i].data(), "Id");
			spec.value = number_or(pairs[i].data(), "UnlockedValue", 0);
			spec.default_value = number_or(pairs[i].data(), "DefaultValue", 0);
			spec.squad = squad;
			if (!targets.empty()) {
				size_t pick = std::min(i, targets.size() - 1);
				size_t best = 0;
				for (size_t t = 0; t < targets.size() && targets.size() > 1; ++t) {
					const size_t n = common_prefix(spec.id, targets[t].field);
					if (n > best) {
						best = n;
						pick = t;
					}
				}
				spec.target_type = targets[pick].type;
				spec.target_field = targets[pick].field;
			}
			out.specializations.push_back(std::move(spec));
		}
	}
	return out;
}

} // namespace bf3::core::gameplay
