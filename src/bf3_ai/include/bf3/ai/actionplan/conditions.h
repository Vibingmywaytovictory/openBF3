#pragma once

#include "bf3/ai/actionplan/statement.h"
#include "bf3/ai/actionplan/variables.h"

#include <memory>
#include <vector>

namespace bf3::ai {

class BAPBoleanOp : public BAPCondition {
public:
	virtual void add_condition(std::unique_ptr<BAPCondition> p_condition);
	size_t size() const { return conditions_.size(); }

protected:
	std::vector<std::unique_ptr<BAPCondition>> conditions_;
};

class BAPConAnd final : public BAPBoleanOp {
public:
	bool evaluate(PlanBot &r_bot) override;
	BAPConditionType condition_type() const override { return BAPConditionType::And; }
};

class BAPConOr final : public BAPBoleanOp {
public:
	bool evaluate(PlanBot &r_bot) override;
	BAPConditionType condition_type() const override { return BAPConditionType::Or; }
};

class BAPConNot final : public BAPBoleanOp {
public:
	BAPConNot() = default;
	explicit BAPConNot(std::unique_ptr<BAPCondition> p_condition);

	void add_condition(std::unique_ptr<BAPCondition> p_condition) override;
	bool evaluate(PlanBot &r_bot) override;
	BAPConditionType condition_type() const override { return BAPConditionType::Not; }
};

class BAPConTrue final : public BAPCondition {
public:
	bool evaluate(PlanBot &) override { return true; }
	BAPConditionType condition_type() const override { return BAPConditionType::True; }
};

class BAPConFalse final : public BAPCondition {
public:
	bool evaluate(PlanBot &) override { return false; }
	BAPConditionType condition_type() const override { return BAPConditionType::False; }
};

class BAPConTime final : public BAPCondition {
public:
	BAPConTime(double p_now, double p_duration, bool p_armed, bool p_fire_first);

	bool evaluate(PlanBot &r_bot) override;
	BAPConditionType condition_type() const override { return BAPConditionType::Time; }

private:
	double deadline_ = 0.0;
	double duration_ = 0.0;
	bool armed_ = false;
};

class BAPConIteration final : public BAPCondition {
public:
	explicit BAPConIteration(int p_count) : count_(p_count) {}

	bool evaluate(PlanBot &) override { return --count_ <= 0; }
	BAPConditionType condition_type() const override { return BAPConditionType::Iteration; }

private:
	int count_ = 0;
};

class BAPConSharedCondition final : public BAPCondition {
public:
	explicit BAPConSharedCondition(std::unique_ptr<BAPCondition> p_condition);

	std::unique_ptr<BAPConSharedCondition> new_shared_condition() const;
	bool evaluate(PlanBot &r_bot) override;
	BAPConditionType condition_type() const override { return BAPConditionType::SharedCondition; }

private:
	explicit BAPConSharedCondition(std::shared_ptr<bool> p_value) : value_(std::move(p_value)) {}

	std::shared_ptr<bool> value_;
	std::unique_ptr<BAPCondition> condition_;
};

template <class T> class BAPConOpLessThan final : public BAPCondition {
public:
	BAPConOpLessThan(const BAPVariable<T> &p_left, const BAPVariable<T> &p_right) : left_(p_left), right_(p_right) {}

	bool evaluate(PlanBot &) override { return left_.value < right_.value; }
	BAPConditionType condition_type() const override { return BAPConditionType::OpLessThan; }

private:
	const BAPVariable<T> &left_;
	const BAPVariable<T> &right_;
};

}
