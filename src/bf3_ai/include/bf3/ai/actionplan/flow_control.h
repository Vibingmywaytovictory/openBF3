#pragma once

#include "bf3/ai/actionplan/statement.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace bf3::ai {

class BAPCombiner : public BAPStatement {
public:
	void add_statement(std::unique_ptr<BAPStatement> p_statement);
	size_t size() const { return statements_.size(); }
	const BAPStatement &statement(size_t p_index) const { return *statements_[p_index]; }

protected:
	BAPCombiner(int p_pattern_id, bool p_wait_for_child) :
			BAPStatement(p_pattern_id), wait_for_child_(p_wait_for_child) {}

	std::vector<std::unique_ptr<BAPStatement>> statements_;
	size_t current_ = 0;
	bool wait_for_child_ = true;
};

class BAPCombinerSerial final : public BAPCombiner {
public:
	explicit BAPCombinerSerial(int p_pattern_id = 0, bool p_wait_for_child = true) :
			BAPCombiner(p_pattern_id, p_wait_for_child) {}

	bool execute(PlanBot &r_bot, bool &r_ok, bool &r_finished) override;
	uint64_t controls() const override;
	BAPStatementType id() const override { return BAPStatementType::Serial; }
	size_t current() const { return current_; }
};

class BAPCombinerParallel final : public BAPCombiner {
public:
	BAPCombinerParallel(int p_pattern_id, bool p_wait_for_child, bool p_never_finish) :
			BAPCombiner(p_pattern_id, p_wait_for_child), never_finish_(p_never_finish) {}

	bool execute(PlanBot &r_bot, bool &r_ok, bool &r_finished) override;
	BAPStatementType id() const override { return BAPStatementType::Parallel; }

private:
	bool never_finish_ = false;
};

class BAPFlowCEnd final : public BAPStatement {
public:
	enum class EndType : int { AbortWhen = 0, EndWhen = 1, EndUnless = 2 };

	BAPFlowCEnd(int p_pattern_id, EndType p_type, std::unique_ptr<BAPCondition> p_condition);

	bool execute(PlanBot &r_bot, bool &r_ok, bool &r_finished) override;
	uint64_t controls() const override { return 0; }
	BAPStatementType id() const override { return BAPStatementType::PlanEnd; }

private:
	std::unique_ptr<BAPCondition> condition_;
	EndType type_ = EndType::AbortWhen;
};

class BAPFlowCIf final : public BAPStatement {
public:
	BAPFlowCIf(std::unique_ptr<BAPCondition> p_condition, std::unique_ptr<BAPStatement> p_true,
			std::unique_ptr<BAPStatement> p_false, int p_pattern_id, bool p_reevaluate, bool p_keep_else);

	bool execute(PlanBot &r_bot, bool &r_ok, bool &r_finished) override;
	uint64_t controls() const override;
	BAPStatementType id() const override { return BAPStatementType::If; }
	const BAPStatement *active_statement() const { return active_; }
	const BAPStatement *true_statement() const { return true_.get(); }
	const BAPStatement *false_statement() const { return false_.get(); }

private:
	std::unique_ptr<BAPCondition> condition_;
	std::unique_ptr<BAPStatement> true_;
	std::unique_ptr<BAPStatement> false_;
	BAPStatement *active_ = nullptr;
	bool reevaluate_ = false;
	bool keep_else_ = false;
};

class BAPFlowCWait final : public BAPStatement {
public:
	BAPFlowCWait(std::unique_ptr<BAPCondition> p_condition, int p_pattern_id);

	bool execute(PlanBot &r_bot, bool &r_ok, bool &r_finished) override;
	uint64_t controls() const override { return 0; }
	BAPStatementType id() const override { return BAPStatementType::Wait; }

private:
	std::unique_ptr<BAPCondition> condition_;
};

class BAPFlowCWhile final : public BAPStatement {
public:
	BAPFlowCWhile(std::unique_ptr<BAPCondition> p_condition, std::unique_ptr<BAPStatement> p_loop, int p_pattern_id,
			bool p_test_on_finish = true);

	bool execute(PlanBot &r_bot, bool &r_ok, bool &r_finished) override;
	uint64_t controls() const override { return loop_ != nullptr ? loop_->controls() : 0; }
	BAPStatementType id() const override { return BAPStatementType::While; }
	void break_loop() { break_ = true; }

private:
	std::unique_ptr<BAPCondition> condition_;
	std::unique_ptr<BAPStatement> loop_;
	bool break_ = false;
	bool test_condition_ = true;
	bool test_on_finish_ = true;
};

class BAPWrapperFinished final : public BAPStatement {
public:
	BAPWrapperFinished(std::unique_ptr<BAPStatement> p_statement, bool p_finished) :
			statement_(std::move(p_statement)), finished_(p_finished) {}

	bool execute(PlanBot &r_bot, bool &r_ok, bool &r_finished) override;
	BAPStatementType id() const override { return BAPStatementType::FinishedWrapper; }

private:
	std::unique_ptr<BAPStatement> statement_;
	bool finished_ = false;
};

class BAPInfoWrapper : public BAPStatement {
public:
	BAPInfoWrapper(std::unique_ptr<BAPStatement> p_statement, int p_pattern_id) :
			BAPStatement(p_pattern_id), statement_(std::move(p_statement)) {}

	bool execute(PlanBot &r_bot, bool &r_ok, bool &r_finished) override;
	uint64_t controls() const override { return statement_->controls(); }
	BAPStatementType id() const override { return BAPStatementType::InfoWrapper; }
	BAPStatement &statement() { return *statement_; }

private:
	std::unique_ptr<BAPStatement> statement_;
};

class BAPAIdle final : public BAPAction {
public:
	BAPAIdle(std::unique_ptr<BAPCondition> p_condition, uint64_t p_controls, int p_pattern_id);

	bool execute(PlanBot &r_bot, bool &r_ok, bool &r_finished) override;
	BAPStatementType id() const override { return BAPStatementType::Idle; }

private:
	std::unique_ptr<BAPCondition> condition_;
};

class BAPAEvaluateCondition final : public BAPStatement {
public:
	BAPAEvaluateCondition(std::unique_ptr<BAPCondition> p_condition, int p_pattern_id);

	bool execute(PlanBot &r_bot, bool &r_ok, bool &r_finished) override;
	uint64_t controls() const override { return 0; }
	BAPStatementType id() const override { return BAPStatementType::EvaluateCondition; }
	bool result() const { return result_; }

private:
	std::unique_ptr<BAPCondition> condition_;
	bool result_ = false;
};

}
