// SOURCE: OpenBF2 Code/BF2/AI/AIDLL/AI/AIPlanInterpreter/ EntryInfanteryMoveTo, EntryInfanteryMoveToObject,
// EntrySoldierPose, EntryTrigger and EntrySense .cpp @c47317b
#include "bf3/ai/interpreter/infantry_entries.h"

#include "bf3/ai/actionplan/infantry_actions.h"

namespace bf3::ai {

namespace {

bool without_body(bool &r_ok, bool &r_finished) {
	r_finished = true;
	r_ok = false;
	return true;
}

}

// STAND-IN: the entries below drive the host's InfantryBody where BF2's write PlayerInput and run the move actions.
// Settle: port the BF2 bodies once a bot's input is a PlayerInput.
bool EntryInfanteryMoveTo::execute(BAPStatement &r_statement, PlanBot &r_bot, bool &r_ok, bool &r_finished) const {
	InfantryBody *const body = r_bot.infantry();
	if (body == nullptr) {
		return without_body(r_ok, r_finished);
	}
	const BAPAMoveTo &move = static_cast<const BAPAMoveTo &>(r_statement);
	r_finished = body->move_to(move.goal(), move.point());
	r_ok = true;
	return true;
}

bool EntryInfanteryMoveToObject::execute(
		BAPStatement &r_statement, PlanBot &r_bot, bool &r_ok, bool &r_finished) const {
	InfantryBody *const body = r_bot.infantry();
	if (body == nullptr) {
		return without_body(r_ok, r_finished);
	}
	r_finished = body->move_to_object(static_cast<const BAPAMoveToObject &>(r_statement).object());
	r_ok = true;
	return true;
}

bool EntrySoldierPose::execute(BAPStatement &r_statement, PlanBot &r_bot, bool &r_ok, bool &r_finished) const {
	InfantryBody *const body = r_bot.infantry();
	if (body == nullptr) {
		return false;
	}
	r_finished = body->soldier_pose(static_cast<const BAPASoldierPose &>(r_statement).pose());
	r_ok = true;
	return true;
}

bool EntryTrigger::execute(BAPStatement &r_statement, PlanBot &r_bot, bool &r_ok, bool &r_finished) const {
	BAPATrigger &trigger = static_cast<BAPATrigger &>(r_statement);
	InfantryBody *const body = r_bot.infantry();
	if (body == nullptr) {
		return without_body(r_ok, r_finished);
	}
	if (r_bot.game_time() < trigger.end_time()) {
		return true;
	}
	if (trigger.stop_condition() != nullptr && trigger.stop_condition()->evaluate(r_bot)) {
		trigger.set_started(false);
		r_ok = true;
		r_finished = true;
		return true;
	}
	if (!trigger.started() || !trigger.release_after_press()) {
		if (trigger.start_condition()->evaluate(r_bot)) {
			// WHY: there is no release call; the body's intent starts unpressed each tick, as BF2's zeroed input does.
			body->trigger(trigger.input());
			trigger.set_started(true);
			r_ok = true;
			r_finished = !trigger.release_after_press();
			return true;
		}
		trigger.set_started(false);
		r_ok = true;
		r_finished = false;
		return true;
	}
	trigger.set_started(false);
	r_ok = true;
	r_finished = true;
	return true;
}

bool EntrySense::execute(BAPStatement &r_statement, PlanBot &r_bot, bool &r_ok, bool &r_finished) const {
	const BAPASense &sense = static_cast<const BAPASense &>(r_statement);
	if (InfantryBody *const body = r_bot.infantry(); body != nullptr) {
		body->sense(sense.focus());
	}
	r_ok = true;
	if (sense.second_condition().evaluate(r_bot)) {
		r_finished = true;
		return true;
	}
	r_finished = sense.first_condition().evaluate(r_bot);
	return true;
}

}
