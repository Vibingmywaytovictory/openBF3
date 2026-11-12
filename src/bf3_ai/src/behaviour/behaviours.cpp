#include "bf3/ai/behaviour/behaviours.h"

#include "bf3/ai/interpreter/bot_plan.h"
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
	{ "bb.medic_assist.weight", KeyKind::Float, 0.0, 1000.0, BF3_BB_FIELD(curves[7].weight) },
	{ "bb.medic_assist.slope", KeyKind::Float, -100.0, 100.0, BF3_BB_FIELD(curves[7].slope) },
	{ "bb.medic_assist.offset", KeyKind::Float, -100.0, 100.0, BF3_BB_FIELD(curves[7].offset) },
	{ "bb.call_for_medic.weight", KeyKind::Float, 0.0, 1000.0, BF3_BB_FIELD(curves[8].weight) },
	{ "bb.call_for_medic.slope", KeyKind::Float, -100.0, 100.0, BF3_BB_FIELD(curves[8].slope) },
	{ "bb.call_for_medic.offset", KeyKind::Float, -100.0, 100.0, BF3_BB_FIELD(curves[8].offset) },
	{ "bb.resupply.weight", KeyKind::Float, 0.0, 1000.0, BF3_BB_FIELD(curves[9].weight) },
	{ "bb.resupply.slope", KeyKind::Float, -100.0, 100.0, BF3_BB_FIELD(curves[9].slope) },
	{ "bb.resupply.offset", KeyKind::Float, -100.0, 100.0, BF3_BB_FIELD(curves[9].offset) },
	{ "bb.squad_far_m", KeyKind::Float, 1.0, 1000.0, BF3_BB_FIELD(squad_far_m) },
	{ "bb.squad_arrive_m", KeyKind::Float, 0.1, 100.0, BF3_BB_FIELD(squad_arrive_m) },
	{ "bb.mission_arrive_m", KeyKind::Float, 0.1, 1000.0, BF3_BB_FIELD(mission_arrive_m) },
	{ "bb.hunt_near_m", KeyKind::Float, 0.0, 1000.0, BF3_BB_FIELD(hunt_near_m) },
	{ "bb.assist_radius_m", KeyKind::Float, 0.0, 1000.0, BF3_BB_FIELD(assist_radius_m) },
	{ "bb.assist_below_hp", KeyKind::Float, 0.0, 1000.0, BF3_BB_FIELD(assist_below_hp) },
	{ "bb.call_below_hp", KeyKind::Float, 0.0, 1000.0, BF3_BB_FIELD(call_below_hp) },
	{ "bb.call_radius_m", KeyKind::Float, 0.0, 1000.0, BF3_BB_FIELD(call_radius_m) },
	{ "bb.call_s", KeyKind::Float, 0.1, 600.0, BF3_BB_FIELD(call_s) },
	{ "bb.assist_s", KeyKind::Float, 0.1, 600.0, BF3_BB_FIELD(assist_s) },
	{ "bb.rethrow_s", KeyKind::Float, 0.0, 600.0, BF3_BB_FIELD(rethrow_s) },
	{ "bb.throw_m", KeyKind::Float, 0.5, 50.0, BF3_BB_FIELD(throw_m) },
	{ "bb.resupply_used_mags", KeyKind::U32, 0, 100, BF3_BB_FIELD(resupply_used_mags) },
	{ "bb.order_detour_m", KeyKind::Float, 0.0, 1000.0, BF3_BB_FIELD(order_detour_m) },
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
		case InfantryBehaviour::MedicAssist:
			return "BBMedicAssist";
		case InfantryBehaviour::CallForMedic:
			return "BBCallForMedic";
		case InfantryBehaviour::Resupply:
			return "BBResupply";
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
	return urgency_;
}

float BotBehaviour::active_urgency(Bot *) {
	return active_urgency_;
}

float BotBehaviour::update_active_urgency(Bot *) {
	active_urgency_ = urgency_;
	return active_urgency_;
}

bool BotBehaviour::has_changed_target(Bot *) {
	return target_changed_;
}

bool BotBehaviour::quotient_changed(Bot *, float p_low, float p_high) {
	if (active_urgency_ == 0.0f) {
		return urgency_ > 0.0f;
	}
	if (urgency_ == 0.0f) {
		return true;
	}
	const float quotient = urgency_ / active_urgency_;
	return p_low > quotient || quotient > p_high;
}

bool BotBehaviour::has_changed(Bot *p_bot, float p_low, float p_high) {
	return has_changed_target(p_bot) && quotient_changed(p_bot, p_low, p_high);
}

void BotBehaviour::reset(Bot *) {
	situation_ = 0.0f;
	urgency_ = 0.0f;
	active_urgency_ = 1e-4f;
	target_changed_ = true;
	target_ = 0;
	refused_tick_ = UINT32_MAX;
}

float BotBehaviour::calculate_urgency(Bot *, float p_modifier) {
	situation_ = refused_tick_ == body_.tick() ? 0.0f : situation();
	const float u = situation_ > 0.0f ? situation_ * p_modifier : 0.0f;
	urgency_ = u > 0.0f ? u : 0.0f;
	target_changed_ = target() != target_;
	target_ = target();
	return urgency_;
}

float BBRevive::situation() {
	return body_.senses().revive_target ? 1.0f : 0.0f;
}

bool BBRevive::plan(BotPlan &r_script, bool) {
	r_script.clear(InfantryActivity::Revive);
	r_script.add(BAPStatementType::MoveToObject, object_args(ObjectGoal::Corpse));
	r_script.add(BAPStatementType::SoldierPose, pose_args(PoseRequest::NearCorpse));
	r_script.add(BAPStatementType::Trigger, trigger_args(TriggerKind::Defib));
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

bool BBFireInfantery::plan(BotPlan &r_script, bool) {
	switch (mode_) {
		case Mode::Throw:
			if (!body_.throw_grenade()) {
				return false;
			}
			[[fallthrough]];
		case Mode::Throwing:
			r_script.clear(InfantryActivity::Grenade);
			r_script.add(BAPStatementType::Trigger, trigger_args(TriggerKind::Grenade));
			return true;
		case Mode::Sight:
			r_script.clear(InfantryActivity::Fight);
			r_script.add(BAPStatementType::Sense, sense_args(SenseFocus::Target));
			r_script.add(BAPStatementType::Trigger, trigger_args(TriggerKind::Aimed));
			r_script.add(BAPStatementType::MoveToObject, object_args(ObjectGoal::Target));
			return true;
		case Mode::Blind:
			r_script.clear(InfantryActivity::Fight);
			r_script.add(BAPStatementType::Sense, sense_args(SenseFocus::BlindPoint));
			r_script.add(BAPStatementType::Trigger, trigger_args(TriggerKind::Blind));
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

bool BBTakeCoverInfantry::plan(BotPlan &r_script, bool) {
	switch (want_) {
		case Want::Start:
			if (!body_.take_cover()) {
				return false;
			}
			[[fallthrough]];
		case Want::Move:
			phase_ = Phase::Move;
			r_script.clear(InfantryActivity::Retreat);
			r_script.add(BAPStatementType::MoveTo, move_args(MoveGoal::Cover));
			return true;
		case Want::WaitFromMove:
			body_.wait_in_cover();
			[[fallthrough]];
		case Want::Wait:
			phase_ = Phase::Wait;
			r_script.clear(InfantryActivity::RegenWait);
			r_script.add(BAPStatementType::SoldierPose, pose_args(PoseRequest::Crouch));
			r_script.add(BAPStatementType::Sense, sense_args(SenseFocus::Threat));
			r_script.add(BAPStatementType::Sense, sense_args(SenseFocus::ShotOrigin));
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

bool BBAvoid::plan(BotPlan &r_script, bool) {
	const bool hunt = body_.senses().hunt_target;
	r_script.clear(hunt ? InfantryActivity::Hunt : InfantryActivity::Roam);
	r_script.add(BAPStatementType::MoveTo, move_args(hunt ? MoveGoal::Hunt : MoveGoal::Roam));
	r_script.add(BAPStatementType::Sense, sense_args(SenseFocus::ShotOrigin));
	return true;
}

float BBMoveTo::situation() {
	squad_ = false;
	hold_ = false;
	float x = body_.senses().hunt_target ? 1.0f : 0.0f;
	const bool goal =
			goal_.kind != SquadGoalKind::None && std::isfinite(goal_.position.x) && std::isfinite(goal_.position.z);
	const bool waiting = goal_.kind == SquadGoalKind::SquadTarget && goal_.wait_for_members;
	const bool in_formation = goal_.kind != SquadGoalKind::Slot ||
			(goal && distance(body_.position(), goal_.position) <= tuning_.squad_far_m);
	if (body_.senses().hunt_near && in_formation) {
		return x;
	}
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

bool BBMoveTo::plan(BotPlan &r_script, bool) {
	if (holding()) {
		r_script.clear(InfantryActivity::Idle);
		r_script.add(BAPStatementType::Sense, sense_args(SenseFocus::Around));
		r_script.add(BAPStatementType::Sense, sense_args(SenseFocus::ShotOrigin));
	} else if (squad_) {
		r_script.clear(InfantryActivity::Roam);
		r_script.add(BAPStatementType::MoveTo, move_args(MoveGoal::Squad, goal_.position));
	} else {
		r_script.clear(InfantryActivity::Hunt);
		r_script.add(BAPStatementType::MoveTo, move_args(MoveGoal::Hunt));
	}
	if (body_.senses().call_medic) {
		r_script.add(BAPStatementType::Trigger, trigger_args(TriggerKind::CallMedic));
	}
	return true;
}

float BBRandom::situation() {
	return body_.senses().navigable ? 1.0f : 0.0f;
}

bool BBRandom::plan(BotPlan &r_script, bool) {
	r_script.clear(InfantryActivity::Roam);
	r_script.add(BAPStatementType::MoveTo, move_args(MoveGoal::Roam));
	return true;
}

float BBIdle::situation() {
	return 1.0f;
}

bool BBIdle::plan(BotPlan &r_script, bool) {
	r_script.clear(InfantryActivity::Idle);
	r_script.add(BAPStatementType::Sense, sense_args(SenseFocus::Around));
	return true;
}

float BBMedicAssist::situation() {
	return body_.senses().heal_target ? 1.0f : 0.0f;
}

bool BBMedicAssist::plan(BotPlan &r_script, bool) {
	r_script.clear(InfantryActivity::Support);
	r_script.add(BAPStatementType::MoveToObject, object_args(ObjectGoal::Patient));
	r_script.add(BAPStatementType::Trigger, trigger_args(TriggerKind::Medkit));
	return true;
}

float BBCallForMedic::situation() {
	return body_.senses().call_medic ? 1.0f : 0.0f;
}

bool BBCallForMedic::plan(BotPlan &r_script, bool) {
	r_script.clear(InfantryActivity::CallMedic);
	r_script.add(BAPStatementType::Trigger, trigger_args(TriggerKind::CallMedic));
	r_script.add(BAPStatementType::MoveTo, move_args(MoveGoal::Medkit));
	return true;
}

float BBResupply::situation() {
	return body_.senses().supply_target ? 1.0f : 0.0f;
}

bool BBResupply::plan(BotPlan &r_script, bool) {
	r_script.clear(InfantryActivity::Support);
	r_script.add(BAPStatementType::MoveToObject, object_args(ObjectGoal::SupplyMate));
	r_script.add(BAPStatementType::Trigger, trigger_args(TriggerKind::AmmoBag));
	return true;
}

}
