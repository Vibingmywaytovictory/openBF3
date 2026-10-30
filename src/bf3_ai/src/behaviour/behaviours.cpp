#include "bf3/ai/behaviour/behaviours.h"

#include "bf3/ai/plan/infantry_entries.h"
#include "bf3/ai/squad/squad_world.h"

#include <algorithm>
#include <cmath>

namespace bf3::ai {

namespace {

using core::KeyDef;
using core::KeyKind;

constexpr float kFireThrowing = 1.0f;
constexpr float kFireSight = 0.55f;
constexpr float kFireThrow = 0.4f;
constexpr float kFireBlind = 0.3f;
constexpr float kCoverHold = 1.0f;
constexpr float kCoverWait = 0.9f;
constexpr float kCoverStart = 0.8f;

#define BF3_BB_FIELD(f) [](BehaviourTuning &t) -> void * { return &t.f; }

const KeyDef<BehaviourTuning> kKeys[] = {
	{ "bb.revive.weight", KeyKind::Float, 0.0, 1000.0, BF3_BB_FIELD(curves[0].weight) },
	{ "bb.revive.slope", KeyKind::Float, -100.0, 100.0, BF3_BB_FIELD(curves[0].slope) },
	{ "bb.revive.offset", KeyKind::Float, -100.0, 100.0, BF3_BB_FIELD(curves[0].offset) },
	{ "bb.fire_infantery.weight", KeyKind::Float, 0.0, 1000.0, BF3_BB_FIELD(curves[1].weight) },
	{ "bb.fire_infantery.slope", KeyKind::Float, -100.0, 100.0, BF3_BB_FIELD(curves[1].slope) },
	{ "bb.fire_infantery.offset", KeyKind::Float, -100.0, 100.0, BF3_BB_FIELD(curves[1].offset) },
	{ "bb.take_cover_infantry.weight", KeyKind::Float, 0.0, 1000.0, BF3_BB_FIELD(curves[2].weight) },
	{ "bb.take_cover_infantry.slope", KeyKind::Float, -100.0, 100.0, BF3_BB_FIELD(curves[2].slope) },
	{ "bb.take_cover_infantry.offset", KeyKind::Float, -100.0, 100.0, BF3_BB_FIELD(curves[2].offset) },
	{ "bb.avoid.weight", KeyKind::Float, 0.0, 1000.0, BF3_BB_FIELD(curves[3].weight) },
	{ "bb.avoid.slope", KeyKind::Float, -100.0, 100.0, BF3_BB_FIELD(curves[3].slope) },
	{ "bb.avoid.offset", KeyKind::Float, -100.0, 100.0, BF3_BB_FIELD(curves[3].offset) },
	{ "bb.move_to.weight", KeyKind::Float, 0.0, 1000.0, BF3_BB_FIELD(curves[4].weight) },
	{ "bb.move_to.slope", KeyKind::Float, -100.0, 100.0, BF3_BB_FIELD(curves[4].slope) },
	{ "bb.move_to.offset", KeyKind::Float, -100.0, 100.0, BF3_BB_FIELD(curves[4].offset) },
	{ "bb.random.weight", KeyKind::Float, 0.0, 1000.0, BF3_BB_FIELD(curves[5].weight) },
	{ "bb.random.slope", KeyKind::Float, -100.0, 100.0, BF3_BB_FIELD(curves[5].slope) },
	{ "bb.random.offset", KeyKind::Float, -100.0, 100.0, BF3_BB_FIELD(curves[5].offset) },
	{ "bb.idle.weight", KeyKind::Float, 0.0, 1000.0, BF3_BB_FIELD(curves[6].weight) },
	{ "bb.idle.slope", KeyKind::Float, -100.0, 100.0, BF3_BB_FIELD(curves[6].slope) },
	{ "bb.idle.offset", KeyKind::Float, -100.0, 100.0, BF3_BB_FIELD(curves[6].offset) },
	{ "bb.decision_ticks", KeyKind::U32, 1, 60, BF3_BB_FIELD(decision_ticks) },
	{ "bb.squad_far_m", KeyKind::Float, 1.0, 1000.0, BF3_BB_FIELD(squad_far_m) },
	{ "bb.squad_arrive_m", KeyKind::Float, 0.1, 100.0, BF3_BB_FIELD(squad_arrive_m) },
	{ "bb.mission_arrive_m", KeyKind::Float, 0.1, 1000.0, BF3_BB_FIELD(mission_arrive_m) },
};

#undef BF3_BB_FIELD

PlanArgs move_args(MoveGoal p_goal, const Vec3 &p_point = {}) {
	PlanArgs a;
	a.move = p_goal;
	a.point = p_point;
	return a;
}

PlanArgs object_args(ObjectGoal p_object) {
	PlanArgs a;
	a.object = p_object;
	return a;
}

PlanArgs pose_args(PoseRequest p_pose) {
	PlanArgs a;
	a.pose = p_pose;
	return a;
}

PlanArgs trigger_args(TriggerKind p_trigger) {
	PlanArgs a;
	a.trigger = p_trigger;
	return a;
}

PlanArgs sense_args(SenseFocus p_focus) {
	PlanArgs a;
	a.focus = p_focus;
	return a;
}

}

std::string_view infantry_behaviour_kind(InfantryBehaviour p_behaviour) {
	switch (p_behaviour) {
		case InfantryBehaviour::Revive:
			return "BBRevive";
		case InfantryBehaviour::FireInfantery:
			return "BBFireInfantery";
		case InfantryBehaviour::TakeCoverInfantry:
			return "BBTakeCoverInfantry";
		case InfantryBehaviour::Avoid:
			return "BBAvoid";
		case InfantryBehaviour::MoveTo:
			return "BBMoveTo";
		case InfantryBehaviour::Random:
			return "BBRandom";
		case InfantryBehaviour::Idle:
			return "BBIdle";
	}
	return "?";
}

bool is_behaviour_key(std::string_view p_key) {
	return p_key.substr(0, kBehaviourSettingsPrefix.size()) == kBehaviourSettingsPrefix;
}

core::KeyOutcome parse_behaviour_setting(std::string_view p_key, std::string_view p_value, BehaviourTuning &r_tuning) {
	const auto key = std::find_if(std::begin(kKeys), std::end(kKeys),
			[&](const KeyDef<BehaviourTuning> &p_def) { return p_def.name == p_key; });
	if (key == std::end(kKeys)) {
		return { core::KeyFault::UnknownKey };
	}
	BehaviourTuning scratch = r_tuning;
	const core::KeyOutcome outcome = core::parse_key_value(*key, p_value, scratch);
	if (outcome.fault == core::KeyFault::None) {
		r_tuning = scratch;
	}
	return outcome;
}

std::string format_behaviour_tuning(const BehaviourTuning &p_tuning) {
	return core::format_key_values(p_tuning, std::span<const KeyDef<BehaviourTuning>>(kKeys));
}

BotBehaviour::BotBehaviour(InfantryBehaviour p_kind, const UrgeCurve &p_curve, InfantryBody &r_body) :
		kind_(p_kind), curve_(p_curve), body_(r_body) {}

float BotBehaviour::urgency(Bot *) {
	situation_ = refused_tick_ == body_.tick() ? 0.0f : situation();
	const float u = situation_ > 0.0f ? curve_.calculate(situation_) : 0.0f;
	urgency_ = u > 0.0f ? u : 0.0f;
	return urgency_;
}

float BotBehaviour::active_urgency(Bot *) {
	return active_urgency_;
}

void BotBehaviour::update_active_urgency(Bot *) {
	active_urgency_ = urgency_;
}

bool BotBehaviour::has_changed_target(Bot *) {
	return false;
}

void BotBehaviour::reset(Bot *) {
	situation_ = 0.0f;
	urgency_ = 0.0f;
	active_urgency_ = 0.0f;
	refused_tick_ = UINT32_MAX;
}

float BBRevive::situation() {
	return body_.senses().revive_target ? 1.0f : 0.0f;
}

bool BBRevive::plan(BotScript &r_script, bool) {
	r_script.clear(InfantryActivity::Revive);
	r_script.add(PlanTarget::MoveToObject, object_args(ObjectGoal::Corpse));
	r_script.add(PlanTarget::SoldierPose, pose_args(PoseRequest::NearCorpse));
	r_script.add(PlanTarget::Trigger, trigger_args(TriggerKind::Defib));
	return true;
}

float BBFireInfantery::situation() {
	const InfantrySenses &s = body_.senses();
	if (s.grenade_running) {
		mode_ = Mode::Throwing;
		return kFireThrowing;
	}
	if (s.enemy_visible) {
		mode_ = Mode::Sight;
		return kFireSight;
	}
	if (body_.can_throw_grenade()) {
		mode_ = Mode::Throw;
		return kFireThrow;
	}
	if (s.blind_fire) {
		mode_ = Mode::Blind;
		return kFireBlind;
	}
	mode_ = Mode::None;
	return 0.0f;
}

bool BBFireInfantery::plan(BotScript &r_script, bool) {
	switch (mode_) {
		case Mode::Throw:
			if (!body_.throw_grenade()) {
				return false;
			}
			[[fallthrough]];
		case Mode::Throwing:
			r_script.clear(InfantryActivity::Grenade);
			r_script.add(PlanTarget::Trigger, trigger_args(TriggerKind::Grenade));
			return true;
		case Mode::Sight:
			r_script.clear(InfantryActivity::Fight);
			r_script.add(PlanTarget::Sense, sense_args(SenseFocus::Target));
			r_script.add(PlanTarget::Trigger, trigger_args(TriggerKind::Aimed));
			r_script.add(PlanTarget::MoveToObject, object_args(ObjectGoal::Target));
			return true;
		case Mode::Blind:
			r_script.clear(InfantryActivity::Fight);
			r_script.add(PlanTarget::Sense, sense_args(SenseFocus::BlindPoint));
			r_script.add(PlanTarget::Trigger, trigger_args(TriggerKind::Blind));
			return true;
		case Mode::None:
			break;
	}
	return false;
}

void BBTakeCoverInfantry::reset(Bot *p_bot) {
	BotBehaviour::reset(p_bot);
	phase_ = Phase::None;
	want_ = Want::None;
}

float BBTakeCoverInfantry::situation() {
	const InfantrySenses &s = body_.senses();
	want_ = Want::None;
	if (phase_ != Phase::None && body_.cover_recovered(phase_ == Phase::Wait)) {
		phase_ = Phase::None;
	}
	if (phase_ == Phase::Move && !s.enemy_visible && body_.cover_cleared()) {
		want_ = Want::WaitFromMove;
		return kCoverHold;
	}
	if (phase_ == Phase::Move && body_.cover_running() && s.low_health) {
		want_ = Want::Move;
		return kCoverHold;
	}
	if (phase_ == Phase::Wait && !s.enemy_visible && !s.shot_at) {
		want_ = Want::Wait;
		return kCoverWait;
	}
	if (phase_ == Phase::None && s.low_health && (s.enemy_visible || s.shot_at) && s.navigable) {
		want_ = Want::Start;
		return kCoverStart;
	}
	return 0.0f;
}

bool BBTakeCoverInfantry::plan(BotScript &r_script, bool) {
	switch (want_) {
		case Want::Start:
			if (!body_.take_cover()) {
				return false;
			}
			[[fallthrough]];
		case Want::Move:
			phase_ = Phase::Move;
			r_script.clear(InfantryActivity::Retreat);
			r_script.add(PlanTarget::MoveTo, move_args(MoveGoal::Cover));
			return true;
		case Want::WaitFromMove:
			body_.wait_in_cover();
			[[fallthrough]];
		case Want::Wait:
			phase_ = Phase::Wait;
			r_script.clear(InfantryActivity::RegenWait);
			r_script.add(PlanTarget::SoldierPose, pose_args(PoseRequest::Crouch));
			r_script.add(PlanTarget::Sense, sense_args(SenseFocus::Threat));
			r_script.add(PlanTarget::Sense, sense_args(SenseFocus::ShotOrigin));
			return true;
		case Want::None:
			break;
	}
	return false;
}

float BBAvoid::situation() {
	const InfantrySenses &s = body_.senses();
	return s.shot_at && !s.enemy_visible ? 1.0f : 0.0f;
}

bool BBAvoid::plan(BotScript &r_script, bool) {
	const bool hunt = body_.senses().hunt_target;
	r_script.clear(hunt ? InfantryActivity::Hunt : InfantryActivity::Roam);
	r_script.add(PlanTarget::MoveTo, move_args(hunt ? MoveGoal::Hunt : MoveGoal::Roam));
	r_script.add(PlanTarget::Sense, sense_args(SenseFocus::ShotOrigin));
	return true;
}

float BBMoveTo::situation() {
	squad_ = false;
	hold_ = false;
	float x = body_.senses().hunt_target ? 1.0f : 0.0f;
	const bool goal =
			goal_.kind != SquadGoalKind::None && std::isfinite(goal_.position.x) && std::isfinite(goal_.position.z);
	const bool waiting = goal_.kind == SquadGoalKind::SquadTarget && goal_.wait_for_members;
	if (ordered_ == int(InfantryBehaviour::MoveTo)) {
		hold_ = !goal || waiting || distance(body_.position(), goal_.position) <= tuning_.squad_arrive_m;
		squad_ = true;
		return std::max(x, 1.0f);
	}
	if (goal && !waiting) {
		const float d = distance(body_.position(), goal_.position);
		if (d > tuning_.squad_arrive_m) {
			const float squad = std::clamp(d / std::max(tuning_.squad_far_m, 1e-3f), 0.0f, 1.0f);
			if (squad > x || squad >= 1.0f) {
				x = squad;
				squad_ = true;
			}
		}
	}
	return x;
}

bool BBMoveTo::plan(BotScript &r_script, bool) {
	if (holding()) {
		r_script.clear(InfantryActivity::Idle);
		r_script.add(PlanTarget::Sense, sense_args(SenseFocus::Around));
		r_script.add(PlanTarget::Sense, sense_args(SenseFocus::ShotOrigin));
		return true;
	}
	if (squad_) {
		r_script.clear(InfantryActivity::Roam);
		r_script.add(PlanTarget::MoveTo, move_args(MoveGoal::Squad, goal_.position));
		return true;
	}
	r_script.clear(InfantryActivity::Hunt);
	r_script.add(PlanTarget::MoveTo, move_args(MoveGoal::Hunt));
	return true;
}

float BBRandom::situation() {
	return body_.senses().navigable ? 1.0f : 0.0f;
}

bool BBRandom::plan(BotScript &r_script, bool) {
	r_script.clear(InfantryActivity::Roam);
	r_script.add(PlanTarget::MoveTo, move_args(MoveGoal::Roam));
	return true;
}

float BBIdle::situation() {
	return 1.0f;
}

bool BBIdle::plan(BotScript &r_script, bool) {
	r_script.clear(InfantryActivity::Idle);
	r_script.add(PlanTarget::Sense, sense_args(SenseFocus::Around));
	return true;
}

}
