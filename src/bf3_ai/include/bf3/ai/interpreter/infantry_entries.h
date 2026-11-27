// SOURCE: OpenBF2 Code/BF2/AI/AIDLL/AI/AIPlanInterpreter/ EntryInfanteryMoveTo, EntryInfanteryMoveToObject,
// EntryInfanteryMoveToDirection, EntryCarMoveToDirection, EntrySoldierPose, EntryTrigger and EntrySense .h @c47317b
#pragma once

#include "bf3/ai/interpreter/bap_interpreter.h"

namespace bf3::ai {

class EntryInfanteryMoveTo final : public BAPEntry {
public:
	bool execute(BAPStatement &r_statement, PlanBot &r_bot, bool &r_ok, bool &r_finished) const override;
	BAPStatementType target_id() const override { return BAPStatementType::MoveTo; }
};

class EntryInfanteryMoveToObject final : public BAPEntry {
public:
	bool execute(BAPStatement &r_statement, PlanBot &r_bot, bool &r_ok, bool &r_finished) const override;
	BAPStatementType target_id() const override { return BAPStatementType::MoveToObject; }
};

// STAND-IN: the two direction-move entries run nothing; no plan issues a direction move. Settle: port their bodies.
class EntryInfanteryMoveToDirection final : public BAPEntry {
public:
	bool execute(BAPStatement &, PlanBot &, bool &, bool &) const override { return true; }
	BAPStatementType target_id() const override { return BAPStatementType::MoveToDirection; }
};

class EntryCarMoveToDirection final : public BAPEntry {
public:
	bool execute(BAPStatement &, PlanBot &, bool &, bool &) const override { return true; }
	BAPStatementType target_id() const override { return BAPStatementType::MoveToDirection; }
};

class EntrySoldierPose final : public BAPEntry {
public:
	bool execute(BAPStatement &r_statement, PlanBot &r_bot, bool &r_ok, bool &r_finished) const override;
	BAPStatementType target_id() const override { return BAPStatementType::SoldierPose; }
};

class EntryTrigger final : public BAPEntry {
public:
	bool execute(BAPStatement &r_statement, PlanBot &r_bot, bool &r_ok, bool &r_finished) const override;
	BAPStatementType target_id() const override { return BAPStatementType::Trigger; }
};

class EntrySense final : public BAPEntry {
public:
	bool execute(BAPStatement &r_statement, PlanBot &r_bot, bool &r_ok, bool &r_finished) const override;
	BAPStatementType target_id() const override { return BAPStatementType::Sense; }
};

}
