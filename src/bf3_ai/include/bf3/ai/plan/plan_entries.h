#pragma once

#include "bf3/ai/main/ai_settings.h"

#include <cstdint>
#include <list>
#include <memory>
#include <span>
#include <string_view>

namespace bf3::ai {

class Bot;
class BotScript;

class BotScriptVariable {
public:
	virtual ~BotScriptVariable() = default;
};

class BAPContext {
public:
	void add_variable(std::unique_ptr<BotScriptVariable> p_variable);
	size_t size() const { return variables_.size(); }
	void clear();

private:
	std::list<std::unique_ptr<BotScriptVariable>> variables_;
};

enum class PlanTarget : int {
	AimAt = 6,
	MoveTo = 7,
	MoveToDirection = 8,
	MoveToObject = 9,
	MoveAwayFromObject = 10,
	Trigger = 11,
	TriggerContinously = 12,
	ResetControls = 14,
	InfoWrapper = 15,
	Sense = 20,
	LookAt = 23,
	TurnTo = 24,
	SoldierPose = 26,
};

class BAPEntry {
public:
	virtual ~BAPEntry() = default;
	virtual bool execute(BotScript *p_script, Bot *p_bot, bool &r_first, bool &r_second) = 0;
	virtual PlanTarget target_id() const = 0;
};

class EntryInfanteryMoveToDirection final : public BAPEntry {
public:
	bool execute(BotScript *p_script, Bot *p_bot, bool &r_first, bool &r_second) override;
	PlanTarget target_id() const override { return PlanTarget::MoveToDirection; }
};

class EntryCarMoveToDirection final : public BAPEntry {
public:
	bool execute(BotScript *p_script, Bot *p_bot, bool &r_first, bool &r_second) override;
	PlanTarget target_id() const override { return PlanTarget::MoveToDirection; }
};

struct PlanEntryKind {
	std::string_view name;
	PlanTarget target = PlanTarget::MoveTo;
	AiDomain domain = AiDomain::Any;
	bool has_body = false;
};

std::span<const PlanEntryKind> plan_entry_kinds();
const PlanEntryKind *find_plan_entry_kind(std::string_view p_name);
std::unique_ptr<BAPEntry> create_plan_entry(std::string_view p_name, const AiSettings &p_settings);

}
