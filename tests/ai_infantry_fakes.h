#pragma once

#include "bf3/ai/bot/infantry.h"

#include <string>
#include <vector>

namespace bf3::ai::test {

struct FakeBody final : InfantryBody {
	sim::Tick now = 1;
	Vec3 at{};
	InfantrySenses s;
	bool throw_possible = false;
	bool cover_ok = true;
	bool recovered = false;
	bool cleared = false;
	bool running = true;
	int throws = 0, covers = 0, waits = 0, sees = 0, sensings = 0, plans = 0, ends = 0, resets = 0;
	InfantryActivity began = InfantryActivity::Idle;
	std::vector<std::string> calls;

	sim::Tick tick() const override { return now; }
	Vec3 position() const override { return at; }
	void see() override { ++sees; }
	void update_sensing() override { ++sensings; }
	const InfantrySenses &senses() const override { return s; }
	bool can_throw_grenade() const override { return throw_possible; }
	bool throw_grenade() override {
		++throws;
		throw_possible = false;
		s.grenade_running = true;
		return true;
	}
	bool take_cover() override {
		++covers;
		return cover_ok;
	}
	void wait_in_cover() override { ++waits; }
	bool cover_recovered(bool) const override { return recovered; }
	bool cover_cleared() const override { return cleared; }
	bool cover_running() const override { return running; }
	void begin_plan(InfantryActivity p_activity) override {
		began = p_activity;
		++plans;
		calls.clear();
	}
	bool move_to(MoveGoal p_goal, const Vec3 &) override {
		calls.push_back("move_to " + std::to_string(int(p_goal)));
		return false;
	}
	bool move_to_object(ObjectGoal p_object) override {
		calls.push_back("move_to_object " + std::to_string(int(p_object)));
		return true;
	}
	bool soldier_pose(PoseRequest p_pose) override {
		calls.push_back("pose " + std::to_string(int(p_pose)));
		return true;
	}
	bool trigger(TriggerKind p_trigger) override {
		calls.push_back("trigger " + std::to_string(int(p_trigger)));
		return true;
	}
	bool sense(SenseFocus p_focus) override {
		calls.push_back("sense " + std::to_string(int(p_focus)));
		return true;
	}
	bool held = true;
	bool end_plan() override {
		++ends;
		return held;
	}
	void reset_controls() override { ++resets; }
};

}
