#pragma once

#include "bf3/ai/bot/infantry.h"
#include "bf3/ai/plan/plan_entries.h"

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

namespace bf3::ai {

struct PlanArgs {
	MoveGoal move = MoveGoal::Roam;
	Vec3 point{};
	ObjectGoal object = ObjectGoal::Target;
	PoseRequest pose = PoseRequest::Crouch;
	TriggerKind trigger = TriggerKind::Aimed;
	SenseFocus focus = SenseFocus::Target;
};

struct PlanStep {
	BAPEntry *entry = nullptr;
	PlanArgs args;
};

class BotScript {
public:
	static constexpr size_t kMaxSteps = 4;

	explicit BotScript(InfantryBody &r_body) : body_(r_body) {}

	void clear(InfantryActivity p_activity);
	bool add(PlanTarget p_target, const PlanArgs &p_args);
	bool execute(Bot *p_bot);

	InfantryBody &body() const { return body_; }
	InfantryActivity activity() const { return activity_; }
	std::span<const PlanStep> steps() const { return { steps_.data(), count_ }; }
	const PlanArgs &current() const { return steps_[cursor_].args; }
	size_t done_steps() const { return done_; }

private:
	InfantryBody &body_;
	InfantryActivity activity_ = InfantryActivity::Idle;
	std::array<PlanStep, kMaxSteps> steps_{};
	size_t count_ = 0;
	size_t cursor_ = 0;
	size_t done_ = 0;
};

class EntryInfanteryMoveTo final : public BAPEntry {
public:
	bool execute(BotScript *p_script, Bot *p_bot, bool &r_first, bool &r_second) override;
	PlanTarget target_id() const override { return PlanTarget::MoveTo; }
};

class EntryInfanteryMoveToObject final : public BAPEntry {
public:
	bool execute(BotScript *p_script, Bot *p_bot, bool &r_first, bool &r_second) override;
	PlanTarget target_id() const override { return PlanTarget::MoveToObject; }
};

class EntrySoldierPose final : public BAPEntry {
public:
	bool execute(BotScript *p_script, Bot *p_bot, bool &r_first, bool &r_second) override;
	PlanTarget target_id() const override { return PlanTarget::SoldierPose; }
};

class EntryTrigger final : public BAPEntry {
public:
	bool execute(BotScript *p_script, Bot *p_bot, bool &r_first, bool &r_second) override;
	PlanTarget target_id() const override { return PlanTarget::Trigger; }
};

class EntrySense final : public BAPEntry {
public:
	bool execute(BotScript *p_script, Bot *p_bot, bool &r_first, bool &r_second) override;
	PlanTarget target_id() const override { return PlanTarget::Sense; }
};

BAPEntry *infantry_entry(PlanTarget p_target);
std::unique_ptr<BAPEntry> create_infantry_entry(std::string_view p_name);

}
