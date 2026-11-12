#include <doctest/doctest.h>

#include "ai_bot_fake.h"
#include "bf3/ai/actionplan/conditions.h"
#include "bf3/ai/actionplan/infantry_actions.h"
#include "bf3/ai/interpreter/bot_plan.h"
#include "bf3/ai/interpreter/infantry_entries.h"

#include <memory>
#include <string>
#include <vector>

using namespace bf3;
using namespace bf3::ai;

namespace {

struct LogBody final : InfantryBody {
	sim::Tick now = 0;
	InfantrySenses sensed;
	std::vector<std::string> calls;
	sim::Tick tick() const override { return now; }
	Vec3 position() const override { return {}; }
	void see() override {}
	void update_sensing() override {}
	const InfantrySenses &senses() const override { return sensed; }
	bool can_throw_grenade() const override { return false; }
	bool throw_grenade() override { return false; }
	bool take_cover() override { return false; }
	void wait_in_cover() override {}
	bool cover_recovered(bool) const override { return false; }
	bool cover_cleared() const override { return false; }
	bool cover_running() const override { return false; }
	void begin_plan(InfantryActivity) override {}
	bool move_to(MoveGoal p_goal, const Vec3 &p_point) override {
		calls.push_back("move_to " + std::to_string(int(p_goal)) + " " + std::to_string(int(p_point.x)));
		return p_goal == MoveGoal::Cover;
	}
	bool move_to_object(ObjectGoal p_object) override {
		calls.push_back("move_to_object " + std::to_string(int(p_object)));
		return false;
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
	bool end_plan() override { return true; }
	void reset_controls() override {}
};

struct StubBot final : test::NullBot {
	StubBot() {
		team = 0;
		player = 0;
	}
};

struct Counter final : BAPStatement {
	BAPStatementType type = BAPStatementType::Idle;
	int runs = 0;
	explicit Counter(BAPStatementType p_type) : type(p_type) {}
	bool execute(PlanBot &, bool &r_ok, bool &r_finished) override {
		++runs;
		r_ok = true;
		r_finished = true;
		return true;
	}
	BAPStatementType id() const override { return type; }
};

struct CountingEntry final : BAPEntry {
	BAPStatementType type = BAPStatementType::Idle;
	mutable int runs = 0;
	explicit CountingEntry(BAPStatementType p_type) : type(p_type) {}
	bool execute(BAPStatement &, PlanBot &, bool &r_ok, bool &r_finished) const override {
		++runs;
		r_ok = true;
		r_finished = false;
		return true;
	}
	BAPStatementType target_id() const override { return type; }
};

PlanArgs args() {
	return PlanArgs{};
}

}

TEST_SUITE("ai-interpreter") {
	TEST_CASE("BAPInterpreter: an entry added at a LOD serves it and the lower ones, a type with no entry runs its "
			  "own body once active, and an inactive type fails") {
		LogBody body;
		StubBot stub;
		BAPInterpreter interpreter;
		BotPlan plan(body, interpreter);
		Counter idle(BAPStatementType::Idle);
		bool ok = true;
		bool finished = false;
		CHECK_FALSE(interpreter.execute(idle, plan, ok, finished));
		CHECK((ok == false && finished == true));
		CHECK(interpreter.activate_entry(BAPStatementType::Idle, BotLodLevel::Medium));
		CHECK_FALSE(interpreter.is_active(BAPStatementType::Idle, BotLodLevel::Full));
		CHECK(interpreter.is_active(BAPStatementType::Idle, BotLodLevel::Light));
		CHECK(plan.execute(&stub) == false);

		const CountingEntry full(BAPStatementType::Trigger);
		const CountingEntry light(BAPStatementType::Trigger);
		CHECK(interpreter.add_entry(light, BotLodLevel::Light));
		CHECK(interpreter.add_entry(full));
		CHECK_FALSE(interpreter.add_entry(full));
		CHECK(interpreter.entry(BAPStatementType::Trigger, BotLodLevel::Full) == &full);
		CHECK(interpreter.entry(BAPStatementType::Trigger, BotLodLevel::Medium) == &full);
		CHECK(interpreter.entry(BAPStatementType::Trigger, BotLodLevel::Light) == &light);
		CHECK_FALSE(interpreter.activate_entry(BAPStatementType::Trigger));
		Counter press(BAPStatementType::Trigger);

		struct LodBot final : PlanBot {
			const PlanInterpreter *interpreter = nullptr;
			BotLodLevel lod = BotLodLevel::Full;
			bool spawned = false;
			const PlanInterpreter &plan_interpreter() const override { return *interpreter; }
			bool is_just_spawned() override { return spawned; }
			BotLodLevel lod_level() const override { return lod; }
			double game_time() const override { return 0.0; }
			InfantryBody *infantry() override { return nullptr; }
		} bot;
		bot.interpreter = &interpreter;
		CHECK(interpreter.execute(press, bot, ok, finished));
		bot.lod = BotLodLevel::Medium;
		interpreter.execute(press, bot, ok, finished);
		bot.lod = BotLodLevel::Light;
		interpreter.execute(press, bot, ok, finished);
		CHECK(full.runs == 2);
		CHECK(light.runs == 1);
		CHECK(press.runs == 0);
		bot.lod = BotLodLevel::Medium;
		interpreter.execute(idle, bot, ok, finished);
		CHECK(idle.runs == 1);

		CHECK(interpreter.remove_entry(BotLodLevel::Full, full));
		CHECK_FALSE(interpreter.is_active(BAPStatementType::Trigger, BotLodLevel::Medium));
		CHECK(interpreter.is_active(BAPStatementType::Trigger, BotLodLevel::Light));

		bot.spawned = true;
		ok = false;
		finished = true;
		CHECK(interpreter.execute(press, bot, ok, finished));
		CHECK((ok == true && finished == false));
		CHECK(light.runs == 1);
	}

	TEST_CASE("BotPlan: a bot dead at a plan call skips it and the first one alive after it, as isJustSpawned") {
		LogBody body;
		StubBot stub;
		BotPlan plan(body);
		plan.clear(InfantryActivity::Roam);
		REQUIRE(plan.add(BAPStatementType::Sense, args()));
		stub.dead = true;
		CHECK(plan.execute(&stub));
		stub.dead = false;
		CHECK(plan.execute(&stub));
		CHECK(body.calls.empty());
		CHECK(plan.execute(&stub));
		CHECK(body.calls == std::vector<std::string>{ "sense 0" });
	}

	TEST_CASE("infantry interpreter: OpenBF2's entries with a body drive the body in plan order, every LOD") {
		const BAPInterpreter &interpreter = infantry_interpreter();
		for (BotLodLevel lod : { BotLodLevel::Full, BotLodLevel::Medium, BotLodLevel::Light }) {
			for (BAPStatementType type :
					{ BAPStatementType::MoveTo, BAPStatementType::MoveToObject, BAPStatementType::MoveToDirection,
							BAPStatementType::SoldierPose, BAPStatementType::Trigger, BAPStatementType::Sense }) {
				REQUIRE(interpreter.entry(type, lod) != nullptr);
				CHECK(interpreter.entry(type, lod)->target_id() == type);
			}
			for (BAPStatementType type : { BAPStatementType::Parallel, BAPStatementType::Serial, BAPStatementType::If,
						 BAPStatementType::While, BAPStatementType::Wait, BAPStatementType::PlanEnd,
						 BAPStatementType::Idle, BAPStatementType::InfoWrapper, BAPStatementType::FinishedWrapper,
						 BAPStatementType::EvaluateCondition }) {
				CHECK(interpreter.is_active(type, lod));
				CHECK(interpreter.entry(type, lod) == nullptr);
			}
			CHECK_FALSE(interpreter.is_active(BAPStatementType::AimAt, lod));
		}

		LogBody body;
		StubBot stub;
		BotPlan plan(body);
		plan.clear(InfantryActivity::Fight);
		plan.context().add(1.0f);
		PlanArgs a;
		a.focus = SenseFocus::Threat;
		CHECK(plan.add(BAPStatementType::Sense, a));
		a.trigger = TriggerKind::Blind;
		CHECK(plan.add(BAPStatementType::Trigger, a));
		a.object = ObjectGoal::Patient;
		CHECK(plan.add(BAPStatementType::MoveToObject, a));
		a.move = MoveGoal::Squad;
		a.point = Vec3{ 7.0f, 0.0f, 0.0f };
		CHECK(plan.add(BAPStatementType::MoveTo, a));
		a.pose = PoseRequest::NearCorpse;
		CHECK(plan.add(BAPStatementType::SoldierPose, a));
		CHECK_FALSE(plan.add(BAPStatementType::AimAt, a));
		CHECK(plan.root().size() == 5u);
		CHECK(plan.root().controls() == kSoldierPoseControls);
		CHECK(plan.activity() == InfantryActivity::Fight);
		CHECK(plan.execute(&stub));
		CHECK(body.calls ==
				std::vector<std::string>{ "sense 2", "trigger 1", "move_to_object 2", "move_to 3 7", "pose 1" });
		plan.clear(InfantryActivity::Idle);
		CHECK(plan.root().size() == 0u);
		CHECK(plan.context().size() == 0u);
		CHECK(plan.activity() == InfantryActivity::Idle);
	}

	TEST_CASE("EntryTrigger: it waits for its end time, presses while its start condition holds, releases on its stop "
			  "condition, and a tap presses once then releases") {
		LogBody body;
		BotPlan plan(body);
		const EntryTrigger entry;
		bool hold = false;
		struct Flag final : BAPCondition {
			const bool *value = nullptr;
			bool evaluate(PlanBot &) override { return *value; }
			BAPConditionType condition_type() const override { return BAPConditionType::Generic1Arg; }
		};
		auto start = std::make_unique<Flag>();
		start->value = &hold;
		bool stop_now = false;
		auto stop = std::make_unique<Flag>();
		stop->value = &stop_now;
		body.now = 0;
		BAPATrigger trigger(TriggerKind::Grenade, std::move(start), std::move(stop), 0, 0, 0.0, 1.0, false);
		BAPConIsTriggered pressed(trigger);
		bool ok = false;
		bool finished = false;
		CHECK(entry.execute(trigger, plan, ok, finished));
		CHECK((ok == false && finished == false));
		CHECK(body.calls.empty());
		body.now = sim::Tick(1.0 / sim::kTickSeconds) + 1;
		CHECK(entry.execute(trigger, plan, ok, finished));
		CHECK((ok == true && finished == false));
		CHECK_FALSE(pressed.evaluate(plan));
		hold = true;
		entry.execute(trigger, plan, ok, finished);
		CHECK((ok == true && finished == true));
		CHECK(pressed.evaluate(plan));
		entry.execute(trigger, plan, ok, finished);
		CHECK(body.calls == std::vector<std::string>{ "trigger 2", "trigger 2" });
		stop_now = true;
		entry.execute(trigger, plan, ok, finished);
		CHECK((ok == true && finished == true));
		CHECK_FALSE(pressed.evaluate(plan));
		CHECK(body.calls.size() == 2u);

		BAPATrigger tap(TriggerKind::Defib, std::make_unique<BAPConTrue>(), nullptr, 0, 0, 0.0, 0.0, true);
		body.calls.clear();
		entry.execute(tap, plan, ok, finished);
		CHECK((ok == true && finished == false));
		CHECK(tap.started());
		entry.execute(tap, plan, ok, finished);
		CHECK((ok == true && finished == true));
		CHECK_FALSE(tap.started());
		CHECK(body.calls == std::vector<std::string>{ "trigger 3" });
	}

	TEST_CASE("EntrySense: the body senses its focus, the second condition ends it, else the first decides") {
		LogBody body;
		BotPlan plan(body);
		const EntrySense entry;
		BAPASense done(SenseFocus::Around, std::make_unique<BAPConFalse>(), std::make_unique<BAPConTrue>(), 0);
		BAPASense waiting(SenseFocus::ShotOrigin, std::make_unique<BAPConFalse>(), std::make_unique<BAPConFalse>(), 0);
		BAPASense focused(SenseFocus::Target, std::make_unique<BAPConTrue>(), std::make_unique<BAPConFalse>(), 0);
		bool ok = false;
		bool finished = false;
		entry.execute(done, plan, ok, finished);
		CHECK((ok && finished));
		entry.execute(waiting, plan, ok, finished);
		CHECK((ok && !finished));
		entry.execute(focused, plan, ok, finished);
		CHECK((ok && finished));
		CHECK(body.calls == std::vector<std::string>{ "sense 4", "sense 3", "sense 0" });

		struct Bodiless final : PlanBot {
			const PlanInterpreter &plan_interpreter() const override { return infantry_interpreter(); }
			bool is_just_spawned() override { return false; }
			BotLodLevel lod_level() const override { return BotLodLevel::Full; }
			double game_time() const override { return 0.0; }
			InfantryBody *infantry() override { return nullptr; }
		} bodiless;
		BAPAMoveTo move(MoveGoal::Roam, {}, 0);
		CHECK(EntryInfanteryMoveTo().execute(move, bodiless, ok, finished));
		CHECK((!ok && finished));
	}
}
