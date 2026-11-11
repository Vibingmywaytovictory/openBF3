#pragma once

#include "bf3/ai/strategic/strategic_object.h"

#include <array>
#include <list>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace bf3::ai {

class AIStrategicObjectManager;

inline constexpr int kTemperatureTreeNodes = 41;
inline constexpr int kTemperatureTreeRows = 56;
inline constexpr int kCombatStrengthTypes = 6;
inline constexpr int kStrategicObjectTypes = 27;
inline constexpr int kStrategicStatuses = 3;

class TemperatureTreeModifiers {
public:
	TemperatureTreeModifiers() { reset(); }
	void reset();

	std::array<float, kTemperatureTreeNodes> node{};
	std::array<std::array<float, kCombatStrengthTypes>, kTemperatureTreeRows> combat_strength{};
};

class StrategyCondition {
public:
	enum class Kind : int { Constant = 0, Homogeneous = 1, Heterogeneous = 2, Area = 3 };
	enum class Strength : int {
		Required = 0,
		RequiredPositive = 1,
		RequiredNegative = 2,
		Advisory = 3,
		AdvisoryPositive = 4,
		AdvisoryNegative = 5
	};
	enum class CompareType : int { Crisp = 0, Fuzzy = 1, FuzzySqr = 2 };
	enum class Side : int { Friendly = 0, Enemy = 1, Both = 2 };
	enum class Compare : int {
		Equal = 0,
		EqualGreater = 1,
		EqualSmaller = 2,
		Quotient = 3,
		QuotientGreater = 4,
		QuotientSmaller = 5,
		Difference = 6,
		DifferenceGreater = 7,
		DifferenceSmaller = 8,
		None = 9
	};
	enum Conditional : int {
		Carrier = 0,
		BattleShip = 1,
		LCVP = 2,
		Naval = 3,
		Bomber = 4,
		Fighter = 5,
		AntiAir = 6,
		Air = 7,
		Infantery = 8,
		Artillery = 9,
		AntiTank = 10,
		HeavyAttack = 11,
		FastAttack = 12,
		Ticket = 13,
		ControlPoint = 14,
		Time = 15,
		StartTime = 16,
		Attacks = 17,
		Defences = 18,
		NumberOfFriendlyAreas = 19,
		NumberOfNeutralAreas = 20,
		NumberOfHostileAreas = 21,
		Flank = 22,
		Base = 23,
		Close = 24,
		Centre = 25,
		Remote = 26,
		Route = 27,
		Bridge = 28,
		North = 29,
		West = 30,
		South = 31,
		East = 32,
		FrontFlank = 33,
		FrontBase = 34,
		FrontClose = 35,
		FrontCentre = 36,
		FrontRemote = 37,
		FrontNeutral = 38,
		Front = 39,
		Safe = 40,
		EnemyObject = 41,
		UnReachable = 42,
		UnitConstant = 43,
		NoConditional = 44
	};

	StrategyCondition(std::string p_name, float p_target, Compare p_compare, Side p_side, CompareType p_compare_type,
			Conditional p_conditional);
	virtual ~StrategyCondition() = default;
	StrategyCondition(const StrategyCondition &) = delete;
	StrategyCondition &operator=(const StrategyCondition &) = delete;

	virtual Kind kind() const = 0;
	virtual float compare(const float *p_friendly, const float *p_enemy, int p_side) const = 0;

	void set_target_value(float p_value) { target_ = p_value; }
	float target_value() const { return target_; }
	void set_strength(Strength p_strength) { strength_ = p_strength; }
	Strength strength() const { return strength_; }
	void set_abort_condition(bool p_abort) { abort_ = p_abort; }
	bool is_abort_condition() const { return abort_; }
	const std::string &name() const { return name_; }

	float internal_compare(float p_a, float p_b) const;
	float internal_compare_crisp(float p_a, float p_b) const;
	float internal_compare_fuzzy(float p_a, float p_b) const;

protected:
	static const float *operand(const float *p_friendly, const float *p_enemy, Side p_side, bool p_second);

	bool abort_ = false;
	float target_ = 0.0f;
	Compare compare_ = Compare::Equal;
	CompareType compare_type_ = CompareType::Crisp;
	Side side_ = Side::Friendly;
	Conditional conditional_ = Carrier;
	Strength strength_ = Strength::Required;
	std::string name_;
};

class SCConstant final : public StrategyCondition {
public:
	SCConstant(std::string p_name, CompareType p_compare_type, Compare p_compare, Side p_side,
			Conditional p_conditional, float p_constant, float p_target);

	Kind kind() const override { return Kind::Constant; }
	float compare(const float *p_friendly, const float *p_enemy, int p_side) const override;

private:
	float constant_ = 0.0f;
};

class SCHomogeneous final : public StrategyCondition {
public:
	SCHomogeneous(std::string p_name, CompareType p_compare_type, Compare p_compare, Conditional p_conditional,
			float p_target);

	Kind kind() const override { return Kind::Homogeneous; }
	float compare(const float *p_friendly, const float *p_enemy, int p_side) const override;
};

class SCHeterogeneous final : public StrategyCondition {
public:
	SCHeterogeneous(std::string p_name, CompareType p_compare_type, Compare p_compare, Side p_side, Conditional p_first,
			Conditional p_second, float p_target);

	Kind kind() const override { return Kind::Heterogeneous; }
	float compare(const float *p_friendly, const float *p_enemy, int p_side) const override;

private:
	Conditional second_ = Carrier;
};

class SCArea final : public StrategyCondition {
public:
	SCArea(std::string p_name, std::string p_area_name, int p_object, StrategicStatus p_status,
			const AIStrategicObjectManager *p_objects);

	Kind kind() const override { return Kind::Area; }
	float compare(const float *p_friendly, const float *p_enemy, int p_side) const override;
	int update_area();
	int object() const { return object_; }

private:
	int object_ = -1;
	std::string area_name_;
	StrategicStatus status_ = StrategicStatus::Owned;
	const AIStrategicObjectManager *objects_ = nullptr;
};

class StrategyPrerequisite {
public:
	explicit StrategyPrerequisite(std::string p_name) : name_(std::move(p_name)) {}

	void add_condition(StrategyCondition *p_condition, float p_weight);
	StrategyCondition *pop_first_condition();
	float evaluate(const float *p_friendly, const float *p_enemy, bool p_active, int p_side) const;
	const std::string &name() const { return name_; }
	size_t size() const { return conditions_.size(); }

private:
	std::list<std::pair<StrategyCondition *, float>> conditions_;
	std::string name_;
};

class Strategy {
public:
	explicit Strategy(std::string p_name);
	Strategy(const Strategy &) = delete;
	Strategy &operator=(const Strategy &) = delete;

	const TemperatureTreeModifiers &temperature_tree_modifiers() const { return tree_; }
	float aggression() const { return aggression_; }
	void set_aggression(float p_aggression) { aggression_ = p_aggression; }
	uint32_t number_of_attacks() const { return attacks_; }
	void set_number_of_attacks(uint32_t p_count) { attacks_ = p_count; }
	uint32_t number_of_attack_considerations() const { return attack_considerations_; }
	void set_number_of_attack_considerations(uint32_t p_count) { attack_considerations_ = p_count; }
	uint32_t number_of_defences() const { return defences_; }
	void set_number_of_defences(uint32_t p_count) { defences_ = p_count; }
	uint32_t number_of_defence_considerations() const { return defence_considerations_; }
	void set_number_of_defence_considerations(uint32_t p_count) { defence_considerations_ = p_count; }
	void set_prerequisite(const StrategyPrerequisite *p_prerequisite) { prerequisite_ = p_prerequisite; }
	void set_abort_condition(const StrategyPrerequisite *p_condition) { abort_condition_ = p_condition; }
	float calculate_prerequisite(
			const float *p_friendly, const float *p_enemy, const Strategy *p_previous, int p_side) const;
	float calculate_abort_condition(
			const float *p_friendly, const float *p_enemy, const Strategy *p_previous, int p_side) const;
	void set_abort_level(float p_level) { abort_level_ = p_level; }
	float abort_level() const { return abort_level_; }
	void set_prerequisite_weight(float p_weight) { prerequisite_weight_ = p_weight; }
	float prerequisite_weight() const { return prerequisite_weight_; }
	float minimum_active_time() const { return minimum_active_time_; }

	void set_tree_modifier(int p_node, float p_value);
	float tree_modifier(int p_node) const { return tree_.node[size_t(p_node)]; }
	void set_tree_modifier(int p_node, int p_type, float p_value);
	float tree_modifier(int p_node, int p_type) const { return tree_.combat_strength[size_t(p_node)][size_t(p_type)]; }
	bool add_required_preceding_strategy(const Strategy *p_strategy);
	bool add_prohibited_preceding_strategy(const Strategy *p_strategy);
	void set_strategic_objects_modifier(AIStrategicObject::Type p_type, float p_value);
	float strategic_objects_modifier(AIStrategicObject::Type p_type) const;
	void set_strategic_objects_modifier(AIStrategicObject::Type p_type, float p_value, StrategicStatus p_status);
	float strategic_objects_modifier(AIStrategicObject::Type p_type, StrategicStatus p_status) const;
	void set_specific_strategic_object_modifier(int p_object, float p_value);
	float specific_strategic_object_modifier(int p_object) const;
	const std::array<float, kStrategicObjectTypes> &strategic_objects_modifiers(StrategicStatus p_status) const {
		return object_modifiers_[size_t(p_status)];
	}
	void clear_all_strategic_objects_modifiers();
	const std::string &name() const { return name_; }

private:
	TemperatureTreeModifiers tree_;
	float aggression_ = 0.5f;
	uint32_t attacks_ = 0;
	uint32_t defences_ = 0;
	uint32_t attack_considerations_ = 0;
	uint32_t defence_considerations_ = 0;
	float abort_level_ = 0.0f;
	float prerequisite_weight_ = 1.0f;
	float minimum_active_time_ = 1e9f;
	const StrategyPrerequisite *prerequisite_ = nullptr;
	const StrategyPrerequisite *abort_condition_ = nullptr;
	std::set<const Strategy *> required_preceding_;
	std::set<const Strategy *> prohibited_preceding_;
	std::array<std::array<float, kStrategicObjectTypes>, kStrategicStatuses> object_modifiers_{};
	std::vector<float> specific_modifiers_;
	std::string name_;
};

}
