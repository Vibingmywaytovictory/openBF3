#include "bf3/ai/interpreter/bot_plan.h"

#include "bf3/ai/actionplan/conditions.h"
#include "bf3/ai/actionplan/infantry_actions.h"

namespace bf3::ai {

const char *infantry_activity_name(InfantryActivity p_activity) {
	switch (p_activity) {
		case InfantryActivity::Idle:
			return "idle";
		case InfantryActivity::Roam:
			return "roam";
		case InfantryActivity::Hunt:
			return "hunt";
		case InfantryActivity::Fight:
			return "fight";
		case InfantryActivity::Retreat:
			return "retreat";
		case InfantryActivity::RegenWait:
			return "regen_wait";
		case InfantryActivity::Revive:
			return "revive";
		case InfantryActivity::Grenade:
			return "grenade";
		case InfantryActivity::Support:
			return "support";
		case InfantryActivity::CallMedic:
			return "call_medic";
	}
	return "?";
}

BotPlan::BotPlan(InfantryBody &r_body, const PlanInterpreter &p_interpreter) :
		body_(r_body), interpreter_(p_interpreter), root_(std::make_unique<BAPCombinerParallel>(0, true, true)) {}

void BotPlan::clear(InfantryActivity p_activity) {
	activity_ = p_activity;
	root_ = std::make_unique<BAPCombinerParallel>(0, true, true);
	context_.clear();
}

bool BotPlan::add(BAPStatementType p_type, const PlanArgs &p_args) {
	switch (p_type) {
		case BAPStatementType::MoveTo:
			root_->add_statement(std::make_unique<BAPAMoveTo>(p_args.move, p_args.point, 0));
			return true;
		case BAPStatementType::MoveToObject:
			root_->add_statement(std::make_unique<BAPAMoveToObject>(p_args.object, 0));
			return true;
		case BAPStatementType::SoldierPose:
			root_->add_statement(std::make_unique<BAPASoldierPose>(p_args.pose, 0));
			return true;
		case BAPStatementType::Trigger:
			root_->add_statement(std::make_unique<BAPATrigger>(
					p_args.trigger, std::make_unique<BAPConTrue>(), nullptr, 0, 0, game_time(), 0.0, false));
			return true;
		case BAPStatementType::Sense:
			root_->add_statement(std::make_unique<BAPASense>(
					p_args.focus, std::make_unique<BAPConTrue>(), std::make_unique<BAPConFalse>(), 0));
			return true;
		default:
			return false;
	}
}

bool BotPlan::execute(Bot *p_bot) {
	bot_ = p_bot;
	bool ok = true;
	bool finished = false;
	interpreter_.execute(*root_, *this, ok, finished);
	bot_ = nullptr;
	return ok;
}

bool BotPlan::is_just_spawned() {
	if (bot_ == nullptr) {
		return false;
	}
	if (bot_->is_dead()) {
		spawn_pending_ = true;
		return true;
	}
	if (!spawn_pending_) {
		return false;
	}
	spawn_pending_ = false;
	return true;
}

BotLodLevel BotPlan::lod_level() const {
	return bot_ != nullptr ? bot_->lod_level() : BotLodLevel::Full;
}

double BotPlan::game_time() const {
	return double(body_.tick()) * sim::kTickSeconds;
}

}
