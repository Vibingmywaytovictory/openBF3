#include "bf3/ai/actionplan/flow_control.h"

namespace bf3::ai {

bool BAPAction::execute(PlanBot &, bool &r_ok, bool &r_finished) {
	r_ok = false;
	r_finished = true;
	return false;
}

void BAPCombiner::add_statement(std::unique_ptr<BAPStatement> p_statement) {
	if (p_statement == nullptr) {
		return;
	}
	controls_ |= p_statement->controls();
	statements_.push_back(std::move(p_statement));
	current_ = 0;
}

bool BAPCombinerSerial::execute(PlanBot &r_bot, bool &r_ok, bool &r_finished) {
	if (current_ >= statements_.size()) {
		r_ok = true;
		r_finished = true;
		current_ = 0;
		return true;
	}
	r_bot.plan_interpreter().execute(*statements_[current_], r_bot, r_ok, r_finished);
	if (wait_for_child_) {
		if (!r_finished) {
			return true;
		}
		++current_;
		if (current_ < statements_.size()) {
			r_finished = false;
			return true;
		}
	} else {
		++current_;
		if (!r_finished && current_ < statements_.size()) {
			return true;
		}
	}
	r_finished = true;
	current_ = 0;
	return true;
}

uint64_t BAPCombinerSerial::controls() const {
	return current_ < statements_.size() ? statements_[current_]->controls() : 0;
}

bool BAPCombinerParallel::execute(PlanBot &r_bot, bool &r_ok, bool &r_finished) {
	r_finished = wait_for_child_;
	r_ok = true;
	const PlanInterpreter &interpreter = r_bot.plan_interpreter();
	bool child_ok = true;
	bool child_finished = false;
	for (current_ = 0; current_ < statements_.size(); ++current_) {
		interpreter.execute(*statements_[current_], r_bot, child_ok, child_finished);
		r_ok = r_ok && child_ok;
		r_finished = wait_for_child_ ? r_finished && child_finished : r_finished || child_finished;
	}
	if (never_finish_) {
		r_finished = false;
	}
	return true;
}

BAPFlowCEnd::BAPFlowCEnd(int p_pattern_id, EndType p_type, std::unique_ptr<BAPCondition> p_condition) :
		BAPStatement(p_pattern_id), condition_(std::move(p_condition)), type_(p_type) {}

bool BAPFlowCEnd::execute(PlanBot &r_bot, bool &r_ok, bool &r_finished) {
	switch (type_) {
		case EndType::AbortWhen:
			r_finished = condition_ == nullptr || condition_->evaluate(r_bot);
			r_ok = !r_finished;
			break;
		case EndType::EndWhen:
			r_ok = condition_ != nullptr;
			r_finished = condition_ == nullptr || condition_->evaluate(r_bot);
			break;
		case EndType::EndUnless:
			r_ok = condition_ != nullptr;
			r_finished = condition_ != nullptr && !condition_->evaluate(r_bot);
			break;
	}
	return true;
}

BAPFlowCIf::BAPFlowCIf(std::unique_ptr<BAPCondition> p_condition, std::unique_ptr<BAPStatement> p_true,
		std::unique_ptr<BAPStatement> p_false, int p_pattern_id, bool p_reevaluate, bool p_keep_else) :
		BAPStatement(p_pattern_id), condition_(std::move(p_condition)), true_(std::move(p_true)),
		false_(std::move(p_false)), reevaluate_(p_reevaluate), keep_else_(p_keep_else) {}

bool BAPFlowCIf::execute(PlanBot &r_bot, bool &r_ok, bool &r_finished) {
	const bool test = !reevaluate_ || (keep_else_ && active_ == false_.get()) ? active_ == nullptr : true;
	if (test) {
		active_ = condition_->evaluate(r_bot) ? true_.get() : false_.get();
		if (active_ == nullptr) {
			r_ok = true;
			r_finished = true;
			return true;
		}
	}
	r_bot.plan_interpreter().execute(*active_, r_bot, r_ok, r_finished);
	if (r_finished) {
		active_ = nullptr;
	}
	return true;
}

uint64_t BAPFlowCIf::controls() const {
	return (true_ != nullptr ? true_->controls() : 0) | (false_ != nullptr ? false_->controls() : 0);
}

BAPFlowCWait::BAPFlowCWait(std::unique_ptr<BAPCondition> p_condition, int p_pattern_id) :
		BAPStatement(p_pattern_id), condition_(std::move(p_condition)) {}

bool BAPFlowCWait::execute(PlanBot &r_bot, bool &r_ok, bool &r_finished) {
	r_finished = condition_->evaluate(r_bot);
	r_ok = true;
	return true;
}

BAPFlowCWhile::BAPFlowCWhile(std::unique_ptr<BAPCondition> p_condition, std::unique_ptr<BAPStatement> p_loop,
		int p_pattern_id, bool p_test_on_finish) :
		BAPStatement(p_pattern_id), condition_(std::move(p_condition)), loop_(std::move(p_loop)),
		test_on_finish_(p_test_on_finish) {}

bool BAPFlowCWhile::execute(PlanBot &r_bot, bool &r_ok, bool &r_finished) {
	if (loop_ == nullptr) {
		r_ok = true;
		break_ = false;
		r_finished = !condition_->evaluate(r_bot);
		return true;
	}
	if (break_) {
		r_ok = true;
		r_finished = true;
		break_ = false;
		return true;
	}
	if (test_condition_ && !condition_->evaluate(r_bot)) {
		r_ok = true;
		r_finished = true;
		return true;
	}
	r_bot.plan_interpreter().execute(*loop_, r_bot, r_ok, r_finished);
	test_condition_ = test_on_finish_ ? r_finished : true;
	r_finished = false;
	return true;
}

bool BAPWrapperFinished::execute(PlanBot &r_bot, bool &r_ok, bool &r_finished) {
	if (statement_ != nullptr) {
		r_bot.plan_interpreter().execute(*statement_, r_bot, r_ok, r_finished);
	}
	r_finished = finished_;
	return true;
}

bool BAPInfoWrapper::execute(PlanBot &r_bot, bool &r_ok, bool &r_finished) {
	return r_bot.plan_interpreter().execute(*statement_, r_bot, r_ok, r_finished);
}

BAPAIdle::BAPAIdle(std::unique_ptr<BAPCondition> p_condition, uint64_t p_controls, int p_pattern_id) :
		BAPAction(p_pattern_id, p_controls), condition_(std::move(p_condition)) {}

bool BAPAIdle::execute(PlanBot &r_bot, bool &r_ok, bool &r_finished) {
	r_ok = true;
	r_finished = condition_->evaluate(r_bot);
	return true;
}

BAPAEvaluateCondition::BAPAEvaluateCondition(std::unique_ptr<BAPCondition> p_condition, int p_pattern_id) :
		BAPStatement(p_pattern_id), condition_(std::move(p_condition)) {}

bool BAPAEvaluateCondition::execute(PlanBot &r_bot, bool &r_ok, bool &r_finished) {
	result_ = condition_->evaluate(r_bot);
	r_ok = true;
	r_finished = true;
	return true;
}

}
