#include <doctest/doctest.h>

#include "bf3/ai/actionplan/conditions.h"
#include "bf3/ai/actionplan/flow_control.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace bf3;
using namespace bf3::ai;

namespace {

struct DirectInterpreter final : PlanInterpreter {
	bool execute(BAPStatement &r_statement, PlanBot &r_bot, bool &r_ok, bool &r_finished) const override {
		return r_statement.execute(r_bot, r_ok, r_finished);
	}
};

struct ClockBot final : PlanBot {
	DirectInterpreter interpreter;
	double now = 0.0;
	const PlanInterpreter &plan_interpreter() const override { return interpreter; }
	bool is_just_spawned() override { return false; }
	BotLodLevel lod_level() const override { return BotLodLevel::Full; }
	double game_time() const override { return now; }
	InfantryBody *infantry() override { return nullptr; }
};

struct Step final : BAPStatement {
	std::string name;
	std::vector<std::string> *log = nullptr;
	int finish_after = 1;
	int runs = 0;
	bool ok = true;
	Step(std::string p_name, std::vector<std::string> &r_log, int p_finish_after, uint64_t p_controls = 0) :
			BAPStatement(0, p_controls), name(std::move(p_name)), log(&r_log), finish_after(p_finish_after) {}
	bool execute(PlanBot &, bool &r_ok, bool &r_finished) override {
		log->push_back(name);
		++runs;
		r_ok = ok;
		r_finished = runs >= finish_after;
		if (r_finished) {
			runs = 0;
		}
		return true;
	}
	BAPStatementType id() const override { return BAPStatementType::Idle; }
};

struct Flag final : BAPCondition {
	const bool *value = nullptr;
	int *evaluations = nullptr;
	explicit Flag(const bool &p_value, int *p_evaluations = nullptr) : value(&p_value), evaluations(p_evaluations) {}
	bool evaluate(PlanBot &) override {
		if (evaluations != nullptr) {
			++*evaluations;
		}
		return *value;
	}
	BAPConditionType condition_type() const override { return BAPConditionType::Generic1Arg; }
};

std::unique_ptr<Step> step(
		const char *p_name, std::vector<std::string> &r_log, int p_finish_after, uint64_t p_controls = 0) {
	return std::make_unique<Step>(p_name, r_log, p_finish_after, p_controls);
}

struct Result {
	bool ok = false;
	bool finished = false;
	bool operator==(const Result &) const = default;
};

Result run(BAPStatement &r_statement, PlanBot &r_bot) {
	Result r;
	r_statement.execute(r_bot, r.ok, r.finished);
	return r;
}

}

TEST_SUITE("ai-actionplan") {
	TEST_CASE("serial combiner: a child runs until it finishes, then the next; the last one finishes the combiner") {
		ClockBot bot;
		std::vector<std::string> log;
		BAPCombinerSerial serial;
		serial.add_statement(step("a", log, 2, 1));
		serial.add_statement(step("b", log, 1, 2));
		serial.add_statement(nullptr);
		CHECK(serial.size() == 2u);
		CHECK(serial.controls() == 1u);
		CHECK(run(serial, bot) == Result{ true, false });
		CHECK(run(serial, bot) == Result{ true, false });
		CHECK(serial.current() == 1u);
		CHECK(serial.controls() == 2u);
		CHECK(run(serial, bot) == Result{ true, true });
		CHECK(serial.current() == 0u);
		CHECK(log == std::vector<std::string>{ "a", "a", "b" });

		BAPCombinerSerial empty;
		CHECK(run(empty, bot) == Result{ true, true });

		log.clear();
		BAPCombinerSerial eager(7, false);
		eager.add_statement(step("a", log, 3));
		eager.add_statement(step("b", log, 3));
		CHECK(eager.pattern_id() == 7);
		CHECK(run(eager, bot) == Result{ true, false });
		CHECK(run(eager, bot) == Result{ true, true });
		CHECK(log == std::vector<std::string>{ "a", "b" });
	}

	TEST_CASE("parallel combiner: every child runs each call; ok is all of theirs, finished all or any of theirs") {
		ClockBot bot;
		std::vector<std::string> log;
		BAPCombinerParallel all(0, true, false);
		all.add_statement(step("a", log, 1, 1));
		auto b = step("b", log, 2, 4);
		Step &held = *b;
		all.add_statement(std::move(b));
		CHECK(all.controls() == 5u);
		CHECK(run(all, bot) == Result{ true, false });
		CHECK(run(all, bot) == Result{ true, true });
		held.ok = false;
		CHECK(run(all, bot).ok == false);
		CHECK(log == std::vector<std::string>{ "a", "b", "a", "b", "a", "b" });

		BAPCombinerParallel any(0, false, false);
		any.add_statement(step("a", log, 1));
		any.add_statement(step("b", log, 3));
		CHECK(run(any, bot) == Result{ true, true });

		BAPCombinerParallel never(0, true, true);
		never.add_statement(step("a", log, 1));
		CHECK(run(never, bot) == Result{ true, false });
	}

	TEST_CASE("if: the condition picks a branch that runs to its end; reevaluate and keep-else re-test it") {
		ClockBot bot;
		std::vector<std::string> log;
		bool cond = true;
		int evaluations = 0;
		BAPFlowCIf held(std::make_unique<Flag>(cond, &evaluations), step("then", log, 2), step("else", log, 1), 0,
				false, false);
		CHECK(run(held, bot) == Result{ true, false });
		cond = false;
		CHECK(run(held, bot) == Result{ true, true });
		CHECK(evaluations == 1);
		CHECK(held.active_statement() == nullptr);
		CHECK(run(held, bot) == Result{ true, true });
		CHECK(log == std::vector<std::string>{ "then", "then", "else" });

		log.clear();
		cond = true;
		BAPFlowCIf always(std::make_unique<Flag>(cond), step("then", log, 2), step("else", log, 2), 0, true, false);
		CHECK(run(always, bot) == Result{ true, false });
		cond = false;
		CHECK(run(always, bot) == Result{ true, false });
		CHECK(log == std::vector<std::string>{ "then", "else" });

		log.clear();
		cond = false;
		BAPFlowCIf keep(std::make_unique<Flag>(cond), step("then", log, 2), step("else", log, 2), 0, true, true);
		CHECK(run(keep, bot) == Result{ true, false });
		cond = true;
		CHECK(run(keep, bot) == Result{ true, true });
		CHECK(run(keep, bot) == Result{ true, false });
		CHECK(log == std::vector<std::string>{ "else", "else", "then" });

		BAPFlowCIf no_else(std::make_unique<BAPConFalse>(), step("then", log, 1), nullptr, 0, false, false);
		CHECK(run(no_else, bot) == Result{ true, true });
		CHECK(no_else.controls() == 0u);
	}

	TEST_CASE("while: the loop runs while its condition holds, tested when a pass finishes; break ends it") {
		ClockBot bot;
		std::vector<std::string> log;
		bool cond = true;
		int evaluations = 0;
		BAPFlowCWhile loop(std::make_unique<Flag>(cond, &evaluations), step("body", log, 2, 8), 0);
		CHECK(loop.controls() == 8u);
		CHECK(run(loop, bot) == Result{ true, false });
		CHECK(run(loop, bot) == Result{ true, false });
		CHECK(evaluations == 1);
		CHECK(run(loop, bot) == Result{ true, false });
		CHECK(evaluations == 2);
		cond = false;
		CHECK(run(loop, bot) == Result{ true, false });
		CHECK(run(loop, bot) == Result{ true, true });
		CHECK(log.size() == 4u);

		cond = true;
		BAPFlowCWhile every(std::make_unique<Flag>(cond, &evaluations), step("body", log, 2), 0, false);
		evaluations = 0;
		run(every, bot);
		run(every, bot);
		CHECK(evaluations == 2);
		every.break_loop();
		CHECK(run(every, bot) == Result{ true, true });

		BAPFlowCWhile bare(std::make_unique<Flag>(cond), nullptr, 0);
		CHECK(run(bare, bot) == Result{ true, false });
		cond = false;
		CHECK(run(bare, bot) == Result{ true, true });
	}

	TEST_CASE("end, wait, idle, evaluate and the wrappers report as OpenBF2's bodies do") {
		ClockBot bot;
		bool cond = false;
		using E = BAPFlowCEnd::EndType;
		BAPFlowCEnd abort_when(0, E::AbortWhen, std::make_unique<Flag>(cond));
		BAPFlowCEnd end_when(0, E::EndWhen, std::make_unique<Flag>(cond));
		BAPFlowCEnd end_unless(0, E::EndUnless, std::make_unique<Flag>(cond));
		CHECK(run(abort_when, bot) == Result{ true, false });
		CHECK(run(end_when, bot) == Result{ true, false });
		CHECK(run(end_unless, bot) == Result{ true, true });
		cond = true;
		CHECK(run(abort_when, bot) == Result{ false, true });
		CHECK(run(end_when, bot) == Result{ true, true });
		CHECK(run(end_unless, bot) == Result{ true, false });
		BAPFlowCEnd abort_now(0, E::AbortWhen, nullptr);
		BAPFlowCEnd end_none(0, E::EndWhen, nullptr);
		BAPFlowCEnd unless_none(0, E::EndUnless, nullptr);
		CHECK(run(abort_now, bot) == Result{ false, true });
		CHECK(run(end_none, bot) == Result{ false, true });
		CHECK(run(unless_none, bot) == Result{ false, false });

		BAPFlowCWait wait(std::make_unique<Flag>(cond), 0);
		CHECK(run(wait, bot) == Result{ true, true });
		BAPAIdle idle(std::make_unique<BAPConFalse>(), 3, 0);
		CHECK(run(idle, bot) == Result{ true, false });
		CHECK(idle.controls() == 3u);
		BAPAEvaluateCondition evaluate(std::make_unique<Flag>(cond), 0);
		CHECK(run(evaluate, bot) == Result{ true, true });
		CHECK(evaluate.result());

		std::vector<std::string> log;
		BAPWrapperFinished finished(step("inner", log, 5), true);
		CHECK(run(finished, bot) == Result{ true, true });
		BAPInfoWrapper info(step("info", log, 1, 16), 4);
		CHECK(info.controls() == 16u);
		CHECK(run(info, bot) == Result{ true, true });
		CHECK(log == std::vector<std::string>{ "inner", "info" });

		struct NoDefault final : BAPAction {
			BAPStatementType id() const override { return BAPStatementType::Trigger; }
		} action;
		bool ok = true;
		bool done = false;
		CHECK_FALSE(action.execute(bot, ok, done));
		CHECK(Result{ ok, done } == Result{ false, true });
	}

	TEST_CASE("conditions: and, or, not, time, iteration, shared and less-than; a context owns its variables") {
		ClockBot bot;
		bool yes = true;
		bool no = false;
		int evaluations = 0;
		BAPConAnd both;
		both.add_condition(std::make_unique<Flag>(no, &evaluations));
		both.add_condition(std::make_unique<Flag>(yes, &evaluations));
		CHECK_FALSE(both.evaluate(bot));
		CHECK(evaluations == 1);
		BAPConOr either;
		either.add_condition(std::make_unique<Flag>(yes, &evaluations));
		either.add_condition(std::make_unique<Flag>(no, &evaluations));
		CHECK(either.evaluate(bot));
		CHECK(evaluations == 2);
		BAPConAnd none;
		BAPConOr nothing;
		CHECK(none.evaluate(bot));
		CHECK_FALSE(nothing.evaluate(bot));
		BAPConNot negated(std::make_unique<Flag>(no));
		negated.add_condition(std::make_unique<Flag>(yes));
		CHECK(negated.size() == 1u);
		CHECK(negated.evaluate(bot));
		CHECK(negated.condition_type() == BAPConditionType::Not);

		BAPConTime armed(10.0, 2.0, true, false);
		bot.now = 11.5;
		CHECK_FALSE(armed.evaluate(bot));
		bot.now = 12.0;
		CHECK(armed.evaluate(bot));
		CHECK_FALSE(armed.evaluate(bot));
		bot.now = 13.9;
		CHECK_FALSE(armed.evaluate(bot));
		bot.now = 14.0;
		CHECK(armed.evaluate(bot));
		BAPConTime first(10.0, 5.0, false, true);
		CHECK(first.evaluate(bot));
		BAPConTime lazy(0.0, 1.0, false, false);
		bot.now = 100.0;
		CHECK_FALSE(lazy.evaluate(bot));
		bot.now = 101.0;
		CHECK(lazy.evaluate(bot));

		BAPConIteration thrice(3);
		CHECK_FALSE(thrice.evaluate(bot));
		CHECK_FALSE(thrice.evaluate(bot));
		CHECK(thrice.evaluate(bot));
		CHECK(thrice.evaluate(bot));

		bool source = true;
		BAPConSharedCondition shared(std::make_unique<Flag>(source));
		std::unique_ptr<BAPConSharedCondition> copy = shared.new_shared_condition();
		CHECK_FALSE(copy->evaluate(bot));
		CHECK(shared.evaluate(bot));
		source = false;
		CHECK(copy->evaluate(bot));
		shared.evaluate(bot);
		CHECK_FALSE(copy->evaluate(bot));

		BAPContext context;
		BAPVariable<float> &left = context.add(1.0f);
		BAPVariable<float> &right = context.add(2.0f);
		BAPConOpLessThan<float> less(left, right);
		CHECK(less.evaluate(bot));
		left.value = 3.0f;
		CHECK_FALSE(less.evaluate(bot));
		static int destroyed = 0;
		struct Counted final : BotScriptVariable {
			~Counted() override { ++destroyed; }
		};
		context.add_variable(std::make_unique<Counted>());
		CHECK(context.size() == 3u);
		context.clear();
		CHECK(destroyed == 1);
		CHECK(context.size() == 0u);
	}
}
