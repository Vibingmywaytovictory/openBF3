#pragma once

#include "bf3/ai/actionplan/flow_control.h"
#include "bf3/ai/actionplan/variables.h"
#include "bf3/ai/bot/infantry.h"
#include "bf3/ai/interpreter/bap_interpreter.h"

#include <memory>

namespace bf3::ai {

struct PlanArgs {
	MoveGoal move = MoveGoal::Roam;
	Vec3 point{};
	ObjectGoal object = ObjectGoal::Target;
	PoseRequest pose = PoseRequest::Crouch;
	TriggerKind trigger = TriggerKind::Aimed;
	SenseFocus focus = SenseFocus::Target;
};

class BotPlan final : public PlanBot {
public:
	explicit BotPlan(InfantryBody &r_body, const PlanInterpreter &p_interpreter = infantry_interpreter());

	void clear(InfantryActivity p_activity);
	bool add(BAPStatementType p_type, const PlanArgs &p_args);
	bool execute(Bot *p_bot);

	InfantryBody &body() const { return body_; }
	InfantryActivity activity() const { return activity_; }
	const BAPCombinerParallel &root() const { return *root_; }
	BAPContext &context() { return context_; }

	const PlanInterpreter &plan_interpreter() const override { return interpreter_; }
	bool is_just_spawned() override;
	BotLodLevel lod_level() const override;
	double game_time() const override;
	InfantryBody *infantry() override { return &body_; }

private:
	InfantryBody &body_;
	const PlanInterpreter &interpreter_;
	Bot *bot_ = nullptr;
	std::unique_ptr<BAPCombinerParallel> root_;
	BAPContext context_;
	InfantryActivity activity_ = InfantryActivity::Idle;
	bool spawn_pending_ = false;
};

}
