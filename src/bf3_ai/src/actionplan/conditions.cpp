#include "bf3/ai/actionplan/conditions.h"

namespace bf3::ai {

void BAPContext::add_variable(std::unique_ptr<BotScriptVariable> p_variable) {
	variables_.push_front(std::move(p_variable));
}

void BAPContext::clear() {
	variables_.clear();
}

void BAPBoleanOp::add_condition(std::unique_ptr<BAPCondition> p_condition) {
	conditions_.push_back(std::move(p_condition));
}

bool BAPConAnd::evaluate(PlanBot &r_bot) {
	for (const std::unique_ptr<BAPCondition> &c : conditions_) {
		if (!c->evaluate(r_bot)) {
			return false;
		}
	}
	return true;
}

bool BAPConOr::evaluate(PlanBot &r_bot) {
	for (const std::unique_ptr<BAPCondition> &c : conditions_) {
		if (c->evaluate(r_bot)) {
			return true;
		}
	}
	return false;
}

BAPConNot::BAPConNot(std::unique_ptr<BAPCondition> p_condition) {
	conditions_.push_back(std::move(p_condition));
}

void BAPConNot::add_condition(std::unique_ptr<BAPCondition> p_condition) {
	if (conditions_.empty()) {
		conditions_.push_back(std::move(p_condition));
	}
}

bool BAPConNot::evaluate(PlanBot &r_bot) {
	return !conditions_.front()->evaluate(r_bot);
}

BAPConTime::BAPConTime(double p_now, double p_duration, bool p_armed, bool p_fire_first) :
		deadline_(p_fire_first ? -1.0 : p_now + p_duration), duration_(p_duration), armed_(p_armed || p_fire_first) {}

bool BAPConTime::evaluate(PlanBot &r_bot) {
	const double now = r_bot.game_time();
	if (!armed_) {
		armed_ = true;
		deadline_ = now + duration_;
	}
	if (now < deadline_) {
		return false;
	}
	armed_ = false;
	return true;
}

BAPConSharedCondition::BAPConSharedCondition(std::unique_ptr<BAPCondition> p_condition) :
		value_(std::make_shared<bool>(false)), condition_(std::move(p_condition)) {}

std::unique_ptr<BAPConSharedCondition> BAPConSharedCondition::new_shared_condition() const {
	return std::unique_ptr<BAPConSharedCondition>(new BAPConSharedCondition(value_));
}

bool BAPConSharedCondition::evaluate(PlanBot &r_bot) {
	if (condition_ != nullptr) {
		*value_ = condition_->evaluate(r_bot);
	}
	return *value_;
}

}
