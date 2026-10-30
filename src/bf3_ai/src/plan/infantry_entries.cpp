#include "bf3/ai/plan/infantry_entries.h"

namespace bf3::ai {

namespace {

EntryInfanteryMoveTo g_move_to;
EntryInfanteryMoveToObject g_move_to_object;
EntrySoldierPose g_soldier_pose;
EntryTrigger g_trigger;
EntrySense g_sense;

}

const char *infantry_activity_name(InfantryActivity p_activity) {
	switch (p_activity) {
		case InfantryActivity::Idle:
			return "idle";
		case InfantryActivity::Roam:
			return "roam";
		case InfantryActivity::Hunt:
			return "hunt";
		case InfantryActivity::Fight:
			return "fight";
		case InfantryActivity::Retreat:
			return "retreat";
		case InfantryActivity::RegenWait:
			return "regen_wait";
		case InfantryActivity::Revive:
			return "revive";
		case InfantryActivity::Grenade:
			return "grenade";
	}
	return "?";
}

void BotScript::clear(InfantryActivity p_activity) {
	activity_ = p_activity;
	count_ = 0;
	cursor_ = 0;
	done_ = 0;
}

bool BotScript::add(PlanTarget p_target, const PlanArgs &p_args) {
	BAPEntry *const entry = infantry_entry(p_target);
	if (entry == nullptr || count_ >= kMaxSteps) {
		return false;
	}
	steps_[count_++] = PlanStep{ entry, p_args };
	return true;
}

bool BotScript::execute(Bot *p_bot) {
	done_ = 0;
	for (cursor_ = 0; cursor_ < count_; ++cursor_) {
		bool done = false;
		bool second = false;
		if (!steps_[cursor_].entry->execute(this, p_bot, done, second)) {
			cursor_ = 0;
			return false;
		}
		done_ += done ? 1u : 0u;
	}
	cursor_ = 0;
	return true;
}

bool EntryInfanteryMoveTo::execute(BotScript *p_script, Bot *, bool &r_first, bool &r_second) {
	const PlanArgs &a = p_script->current();
	r_first = p_script->body().move_to(a.move, a.point);
	r_second = false;
	return true;
}

bool EntryInfanteryMoveToObject::execute(BotScript *p_script, Bot *, bool &r_first, bool &r_second) {
	r_first = p_script->body().move_to_object(p_script->current().object);
	r_second = false;
	return true;
}

bool EntrySoldierPose::execute(BotScript *p_script, Bot *, bool &r_first, bool &r_second) {
	r_first = p_script->body().soldier_pose(p_script->current().pose);
	r_second = false;
	return true;
}

bool EntryTrigger::execute(BotScript *p_script, Bot *, bool &r_first, bool &r_second) {
	r_first = p_script->body().trigger(p_script->current().trigger);
	r_second = false;
	return true;
}

bool EntrySense::execute(BotScript *p_script, Bot *, bool &r_first, bool &r_second) {
	r_first = p_script->body().sense(p_script->current().focus);
	r_second = false;
	return true;
}

BAPEntry *infantry_entry(PlanTarget p_target) {
	switch (p_target) {
		case PlanTarget::MoveTo:
			return &g_move_to;
		case PlanTarget::MoveToObject:
			return &g_move_to_object;
		case PlanTarget::SoldierPose:
			return &g_soldier_pose;
		case PlanTarget::Trigger:
			return &g_trigger;
		case PlanTarget::Sense:
			return &g_sense;
		default:
			return nullptr;
	}
}

std::unique_ptr<BAPEntry> create_infantry_entry(std::string_view p_name) {
	if (p_name == "EntryInfanteryMoveTo") {
		return std::make_unique<EntryInfanteryMoveTo>();
	}
	if (p_name == "EntryInfanteryMoveToObject") {
		return std::make_unique<EntryInfanteryMoveToObject>();
	}
	if (p_name == "EntrySoldierPose") {
		return std::make_unique<EntrySoldierPose>();
	}
	if (p_name == "EntryTrigger") {
		return std::make_unique<EntryTrigger>();
	}
	if (p_name == "EntrySense") {
		return std::make_unique<EntrySense>();
	}
	return nullptr;
}

}
