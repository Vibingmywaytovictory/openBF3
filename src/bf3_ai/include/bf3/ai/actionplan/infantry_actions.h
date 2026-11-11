#pragma once

#include "bf3/ai/actionplan/statement.h"
#include "bf3/ai/bot/infantry.h"

#include <cstdint>
#include <memory>

namespace bf3::ai {

inline constexpr uint64_t kSoldierPoseControls = 0x6000000000ull;

class BAPAMoveTo final : public BAPAction {
public:
	BAPAMoveTo(MoveGoal p_goal, const Vec3 &p_point, int p_pattern_id) :
			BAPAction(p_pattern_id), goal_(p_goal), point_(p_point) {}

	BAPStatementType id() const override { return BAPStatementType::MoveTo; }
	MoveGoal goal() const { return goal_; }
	const Vec3 &point() const { return point_; }

private:
	MoveGoal goal_ = MoveGoal::Roam;
	Vec3 point_{};
};

class BAPAMoveToObject final : public BAPAction {
public:
	BAPAMoveToObject(ObjectGoal p_object, int p_pattern_id) : BAPAction(p_pattern_id), object_(p_object) {}

	BAPStatementType id() const override { return BAPStatementType::MoveToObject; }
	ObjectGoal object() const { return object_; }

private:
	ObjectGoal object_ = ObjectGoal::Target;
};

class BAPASoldierPose final : public BAPAction {
public:
	BAPASoldierPose(PoseRequest p_pose, int p_pattern_id) :
			BAPAction(p_pattern_id, kSoldierPoseControls), pose_(p_pose) {}

	BAPStatementType id() const override { return BAPStatementType::SoldierPose; }
	PoseRequest pose() const { return pose_; }

private:
	PoseRequest pose_ = PoseRequest::Crouch;
};

class BAPATrigger final : public BAPAction {
public:
	BAPATrigger(TriggerKind p_input, std::unique_ptr<BAPCondition> p_start, std::unique_ptr<BAPCondition> p_stop,
			uint64_t p_controls, int p_pattern_id, double p_now, double p_duration, bool p_release_after_press);

	BAPStatementType id() const override { return BAPStatementType::Trigger; }
	TriggerKind input() const { return input_; }
	BAPCondition *start_condition() const { return start_.get(); }
	BAPCondition *stop_condition() const { return stop_.get(); }
	double end_time() const { return end_time_; }
	bool release_after_press() const { return release_after_press_; }
	bool started() const { return started_; }
	void set_started(bool p_started) { started_ = p_started; }

private:
	TriggerKind input_ = TriggerKind::Aimed;
	std::unique_ptr<BAPCondition> start_;
	std::unique_ptr<BAPCondition> stop_;
	bool started_ = false;
	double end_time_ = 0.0;
	bool release_after_press_ = false;
};

class BAPConIsTriggered final : public BAPCondition {
public:
	explicit BAPConIsTriggered(const BAPATrigger &p_trigger) : trigger_(p_trigger) {}

	bool evaluate(PlanBot &) override { return trigger_.started(); }
	BAPConditionType condition_type() const override { return BAPConditionType::IsTriggered; }

private:
	const BAPATrigger &trigger_;
};

class BAPASense final : public BAPAction {
public:
	BAPASense(SenseFocus p_focus, std::unique_ptr<BAPCondition> p_first, std::unique_ptr<BAPCondition> p_second,
			int p_pattern_id);

	uint64_t controls() const override { return 0; }
	BAPStatementType id() const override { return BAPStatementType::Sense; }
	SenseFocus focus() const { return focus_; }
	BAPCondition &first_condition() const { return *first_; }
	BAPCondition &second_condition() const { return *second_; }

private:
	SenseFocus focus_ = SenseFocus::Target;
	std::unique_ptr<BAPCondition> first_;
	std::unique_ptr<BAPCondition> second_;
};

}
