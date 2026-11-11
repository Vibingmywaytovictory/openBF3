#include "bf3/ai/strategic/strategy_model.h"

#include <cmath>

namespace bf3::ai {

void TemperatureTreeModifiers::reset() {
	node.fill(1.0f);
	for (std::array<float, kCombatStrengthTypes> &row : combat_strength) {
		row.fill(1.0f);
	}
}

StrategyCondition::StrategyCondition(std::string p_name, float p_target, Compare p_compare, Side p_side,
		CompareType p_compare_type, Conditional p_conditional) :
		target_(p_target), compare_(p_compare), compare_type_(p_compare_type), side_(p_side),
		conditional_(p_conditional), name_(std::move(p_name)) {}

float StrategyCondition::internal_compare(float p_a, float p_b) const {
	switch (compare_type_) {
		case CompareType::Crisp:
			return internal_compare_crisp(p_a, p_b);
		case CompareType::Fuzzy:
			return internal_compare_fuzzy(p_a, p_b);
		case CompareType::FuzzySqr: {
			const float r = internal_compare_fuzzy(p_a, p_b);
			const float sign = r > 0.0f ? 1.0f : (r < 0.0f ? -1.0f : 0.0f);
			return sign * (r * r);
		}
	}
	return 0.0f;
}

float StrategyCondition::internal_compare_crisp(float p_a, float p_b) const {
	switch (compare_) {
		case Compare::Equal:
		case Compare::Difference:
			return p_a - p_b == target_ ? 1.0f : -1.0f;
		case Compare::EqualGreater:
		case Compare::DifferenceGreater:
			return p_a - p_b < target_ ? -1.0f : 1.0f;
		case Compare::EqualSmaller:
		case Compare::DifferenceSmaller:
			return target_ < p_a - p_b ? -1.0f : 1.0f;
		case Compare::Quotient:
			if (p_b == 0.0f) {
				return -1.0f;
			}
			return p_a / p_b == target_ ? 1.0f : -1.0f;
		case Compare::QuotientGreater:
			if (p_b == 0.0f) {
				return 1.0f;
			}
			return p_a / p_b < target_ ? -1.0f : 1.0f;
		case Compare::QuotientSmaller:
			if (p_b == 0.0f) {
				return -1.0f;
			}
			return target_ < p_a / p_b ? -1.0f : 1.0f;
		case Compare::None:
			break;
	}
	return 0.0f;
}

float StrategyCondition::internal_compare_fuzzy(float p_a, float p_b) const {
	switch (compare_) {
		case Compare::Equal:
		case Compare::Difference:
			return -std::fabs(p_a - p_b - target_);
		case Compare::EqualGreater:
		case Compare::DifferenceGreater:
			return p_a - p_b - target_;
		case Compare::EqualSmaller:
		case Compare::DifferenceSmaller:
			return -(p_a - p_b - target_);
		case Compare::Quotient:
			if (p_b == 0.0f) {
				return -1000.0f;
			}
			return -std::fabs(p_a / p_b - target_);
		case Compare::QuotientGreater:
			if (p_b == 0.0f) {
				return 1000.0f;
			}
			return p_a / p_b - target_;
		case Compare::QuotientSmaller:
			if (p_b == 0.0f) {
				return -1000.0f;
			}
			return -(p_a / p_b - target_);
		case Compare::None:
			break;
	}
	return 0.0f;
}

const float *StrategyCondition::operand(const float *p_friendly, const float *p_enemy, Side p_side, bool p_second) {
	switch (p_side) {
		case Side::Friendly:
			return p_friendly;
		case Side::Enemy:
			return p_enemy;
		case Side::Both:
			return p_second ? p_enemy : p_friendly;
	}
	return nullptr;
}

SCConstant::SCConstant(std::string p_name, CompareType p_compare_type, Compare p_compare, Side p_side,
		Conditional p_conditional, float p_constant, float p_target) :
		StrategyCondition(std::move(p_name), p_target, p_compare, p_side, p_compare_type, p_conditional),
		constant_(p_constant) {}

float SCConstant::compare(const float *p_friendly, const float *p_enemy, int) const {
	const float *values = side_ == Side::Friendly ? p_friendly : p_enemy;
	return internal_compare(values[conditional_], constant_);
}

SCHomogeneous::SCHomogeneous(
		std::string p_name, CompareType p_compare_type, Compare p_compare, Conditional p_conditional, float p_target) :
		StrategyCondition(std::move(p_name), p_target, p_compare, Side::Both, p_compare_type, p_conditional) {}

float SCHomogeneous::compare(const float *p_friendly, const float *p_enemy, int) const {
	const float *a = operand(p_friendly, p_enemy, side_, false);
	const float *b = operand(p_friendly, p_enemy, side_, true);
	if (a == nullptr || b == nullptr) {
		return 0.0f;
	}
	return internal_compare(a[conditional_], b[conditional_]);
}

SCHeterogeneous::SCHeterogeneous(std::string p_name, CompareType p_compare_type, Compare p_compare, Side p_side,
		Conditional p_first, Conditional p_second, float p_target) :
		StrategyCondition(std::move(p_name), p_target, p_compare, p_side, p_compare_type, p_first), second_(p_second) {}

float SCHeterogeneous::compare(const float *p_friendly, const float *p_enemy, int) const {
	const float *a = operand(p_friendly, p_enemy, side_, false);
	const float *b = operand(p_friendly, p_enemy, side_, true);
	if (a == nullptr || b == nullptr) {
		return 0.0f;
	}
	return internal_compare(a[conditional_], b[second_]);
}

SCArea::SCArea(std::string p_name, std::string p_area_name, int p_object, StrategicStatus p_status,
		const AIStrategicObjectManager *p_objects) :
		StrategyCondition(std::move(p_name), 0.0f, Compare::None, Side::Friendly, CompareType::Crisp, NoConditional),
		object_(p_object), area_name_(std::move(p_area_name)), status_(p_status), objects_(p_objects) {}

float SCArea::compare(const float *, const float *, int p_side) const {
	const AIStrategicObject *object = objects_ != nullptr ? objects_->strategic_object(object_) : nullptr;
	if (object == nullptr || object->type() != 0) {
		return -1.0f;
	}
	return object->status(p_side) == status_ ? 1.0f : -1.0f;
}

int SCArea::update_area() {
	const AIStrategicArea *area = objects_ != nullptr ? objects_->strategic_area(area_name_) : nullptr;
	object_ = area != nullptr ? area->index() : -1;
	return object_;
}

void StrategyPrerequisite::add_condition(StrategyCondition *p_condition, float p_weight) {
	if (p_condition != nullptr) {
		conditions_.emplace_back(p_condition, p_weight);
	}
}

StrategyCondition *StrategyPrerequisite::pop_first_condition() {
	if (conditions_.empty()) {
		return nullptr;
	}
	StrategyCondition *const condition = conditions_.front().first;
	conditions_.pop_front();
	return condition;
}

float StrategyPrerequisite::evaluate(const float *p_friendly, const float *p_enemy, bool p_active, int p_side) const {
	float sum = 0.0f;
	for (const auto &[condition, weight] : conditions_) {
		if (!p_active && condition->is_abort_condition()) {
			continue;
		}
		float value = condition->compare(p_friendly, p_enemy, p_side) * weight;
		switch (condition->strength()) {
			case StrategyCondition::Strength::Required:
			case StrategyCondition::Strength::RequiredPositive:
				if (0.0f > value) {
					return 0.0f;
				}
				break;
			case StrategyCondition::Strength::RequiredNegative:
				if (value > 0.0f) {
					return 0.0f;
				}
				break;
			case StrategyCondition::Strength::Advisory:
				break;
			case StrategyCondition::Strength::AdvisoryPositive:
				value = value < 0.0f ? 0.0f : value;
				break;
			case StrategyCondition::Strength::AdvisoryNegative:
				value = value > 0.0f ? 0.0f : value;
				break;
		}
		sum += value;
	}
	return sum;
}

Strategy::Strategy(std::string p_name) : name_(std::move(p_name)) {
	clear_all_strategic_objects_modifiers();
}

float Strategy::calculate_prerequisite(
		const float *p_friendly, const float *p_enemy, const Strategy *p_previous, int p_side) const {
	if (!required_preceding_.empty() && !required_preceding_.contains(p_previous)) {
		return 0.0f;
	}
	if (!prohibited_preceding_.empty() && prohibited_preceding_.contains(p_previous)) {
		return 0.0f;
	}
	if (prerequisite_ == nullptr) {
		return 0.0f;
	}
	return prerequisite_->evaluate(p_friendly, p_enemy, p_previous == this, p_side) * prerequisite_weight_;
}

float Strategy::calculate_abort_condition(
		const float *p_friendly, const float *p_enemy, const Strategy *p_previous, int p_side) const {
	if (abort_condition_ == nullptr) {
		return abort_level_;
	}
	return abort_level_ - abort_condition_->evaluate(p_friendly, p_enemy, p_previous == this, p_side);
}

void Strategy::set_tree_modifier(int p_node, float p_value) {
	if (uint32_t(p_node) < uint32_t(kTemperatureTreeNodes)) {
		tree_.node[size_t(p_node)] = p_value;
	}
}

void Strategy::set_tree_modifier(int p_node, int p_type, float p_value) {
	if (uint32_t(p_node) >= uint32_t(kTemperatureTreeNodes) || p_type < 0 || p_type >= kCombatStrengthTypes) {
		return;
	}
	tree_.combat_strength[size_t(p_node)][size_t(p_type)] = p_value;
}

bool Strategy::add_required_preceding_strategy(const Strategy *p_strategy) {
	return p_strategy != nullptr && required_preceding_.insert(p_strategy).second;
}

bool Strategy::add_prohibited_preceding_strategy(const Strategy *p_strategy) {
	return p_strategy != nullptr && prohibited_preceding_.insert(p_strategy).second;
}

void Strategy::set_strategic_objects_modifier(AIStrategicObject::Type p_type, float p_value) {
	for (std::array<float, kStrategicObjectTypes> &row : object_modifiers_) {
		row[size_t(p_type)] = p_value;
	}
}

float Strategy::strategic_objects_modifier(AIStrategicObject::Type p_type) const {
	float sum = 0.0f;
	for (const std::array<float, kStrategicObjectTypes> &row : object_modifiers_) {
		sum += row[size_t(p_type)];
	}
	return sum / 3.0f;
}

void Strategy::set_strategic_objects_modifier(AIStrategicObject::Type p_type, float p_value, StrategicStatus p_status) {
	object_modifiers_[size_t(p_status)][size_t(p_type)] = p_value;
}

float Strategy::strategic_objects_modifier(AIStrategicObject::Type p_type, StrategicStatus p_status) const {
	return object_modifiers_[size_t(p_status)][size_t(p_type)];
}

void Strategy::set_specific_strategic_object_modifier(int p_object, float p_value) {
	if (p_object < 0) {
		return;
	}
	if (size_t(p_object) >= specific_modifiers_.size()) {
		specific_modifiers_.resize(size_t(p_object) + 1, 1.0f);
	}
	specific_modifiers_[size_t(p_object)] = p_value;
}

float Strategy::specific_strategic_object_modifier(int p_object) const {
	if (p_object >= 0 && size_t(p_object) < specific_modifiers_.size()) {
		return specific_modifiers_[size_t(p_object)];
	}
	return 1.0f;
}

void Strategy::clear_all_strategic_objects_modifiers() {
	for (std::array<float, kStrategicObjectTypes> &row : object_modifiers_) {
		row.fill(1.0f);
	}
}

}
