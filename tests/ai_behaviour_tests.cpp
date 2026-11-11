#include <doctest/doctest.h>

#include "ai_infantry_fakes.h"
#include "bf3/ai/behaviour/behaviours.h"
#include "bf3/ai/behaviour/curves.h"
#include "bf3/ai/behaviour/urgency_merger.h"
#include "bf3/ai/bot/bot_main.h"
#include "bf3/ai/bot/bot_memory.h"
#include "bf3/ai/squad/squad_world.h"

#include <vector>

using namespace bf3;
using namespace bf3::ai;
using bf3::ai::test::FakeBody;

namespace {

struct Generator final : UrgencyGenerator {
	float value = 0.0f;
	float active = 0.0f;
	bool target = false;
	bool quotient = false;
	int quotient_calls = 0;
	int target_calls = 0;
	float urgency(Bot *) override { return value; }
	float active_urgency(Bot *) override { return active; }
	float update_active_urgency(Bot *) override {
		active = value;
		return active;
	}
	bool has_changed_target(Bot *) override {
		++target_calls;
		return target;
	}
	bool quotient_changed(Bot *, float, float) override {
		++quotient_calls;
		return quotient;
	}
	bool has_changed(Bot *, float, float) override { return target && quotient; }
	UrgencyGenerator *chosen_generator() override { return this; }
	void reset(Bot *) override { value = active = 0.0f; }
};

struct Level final : BotBehaviour {
	float level = 0.0f;
	Level(const UrgeCurve &p_curve, InfantryBody &r_body) : BotBehaviour(InfantryBehaviour::Idle, p_curve, r_body) {}
	bool plan(BotPlan &, bool) override { return true; }

protected:
	float situation() override { return level; }
};

}

TEST_SUITE("ai-behaviour") {
	TEST_CASE("UrgencyMerger: the strictly most urgent generator wins on update; none above zero chooses none") {
		Generator a, b, c;
		UrgencyMerger merger({ &a, nullptr, &b, &c });
		CHECK(merger.size() == 4);
		CHECK(merger.chosen_generator() == nullptr);
		CHECK(merger.urgency(nullptr) == 0.0f);
		CHECK(merger.active_urgency(nullptr) == 0.0f);
		a.value = 2.0f;
		b.value = 3.0f;
		c.value = 3.0f;
		CHECK(merger.urgency(nullptr) == 0.0f);
		CHECK(merger.update_active_urgency(nullptr) == 3.0f);
		CHECK(merger.chosen_generator() == &b);
		CHECK(a.active == 2.0f);
		CHECK(c.active == 3.0f);
		b.value = 1.0f;
		CHECK(merger.urgency(nullptr) == 1.0f);
		CHECK(merger.active_urgency(nullptr) == 3.0f);
		a.value = b.value = c.value = 0.0f;
		CHECK(merger.update_active_urgency(nullptr) == 0.0f);
		CHECK(merger.chosen_generator() == nullptr);
		a.value = -1.0f;
		CHECK(merger.update_active_urgency(nullptr) == 0.0f);
		CHECK(merger.chosen_generator() == nullptr);
	}

	TEST_CASE("UrgencyMerger: hasChanged is any quotient or target change, the other two every generator's") {
		Generator a, b;
		UrgencyMerger merger({ &a, nullptr, &b });
		CHECK_FALSE(merger.has_changed(nullptr, 0.87f, 1.15f));
		CHECK(a.quotient_calls == 1);
		CHECK(b.quotient_calls == 1);
		b.target = true;
		CHECK(merger.has_changed(nullptr, 0.87f, 1.15f));
		a.quotient = true;
		b.target = false;
		const int b_calls = b.quotient_calls;
		CHECK(merger.has_changed(nullptr, 0.87f, 1.15f));
		CHECK(b.quotient_calls == b_calls);
		CHECK_FALSE(merger.quotient_changed(nullptr, 0.87f, 1.15f));
		b.quotient = true;
		CHECK(merger.quotient_changed(nullptr, 0.87f, 1.15f));
		CHECK_FALSE(merger.has_changed_target(nullptr));
		const int b_targets = b.target_calls;
		a.target = false;
		CHECK_FALSE(merger.has_changed_target(nullptr));
		CHECK(b.target_calls == b_targets);
		a.target = b.target = true;
		CHECK(merger.has_changed_target(nullptr));
		UrgencyMerger empty({});
		CHECK(empty.has_changed_target(nullptr));
		CHECK(empty.quotient_changed(nullptr, 0.87f, 1.15f));
		CHECK_FALSE(empty.has_changed(nullptr, 0.87f, 1.15f));
		a.value = 5.0f;
		merger.reset(nullptr);
		CHECK(a.value == 0.0f);
	}

	TEST_CASE("BotBehaviour: the urgency is the situation times the modifier; its quotient against the active one") {
		FakeBody body;
		const UCLinear curve(2.0f, 0.0f);
		Level b(curve, body);
		b.reset(nullptr);
		CHECK(b.urgency(nullptr) == 0.0f);
		CHECK(b.active_urgency(nullptr) == doctest::Approx(1e-4f));
		CHECK(b.has_changed_target(nullptr));
		CHECK(b.quotient_changed(nullptr, 0.87f, 1.15f));
		b.level = 0.5f;
		CHECK(b.calculate_urgency(nullptr, 6.0f) == doctest::Approx(3.0f));
		CHECK_FALSE(b.has_changed_target(nullptr));
		CHECK(b.urgency(nullptr) == doctest::Approx(3.0f));
		CHECK(b.update_active_urgency(nullptr) == doctest::Approx(3.0f));
		CHECK_FALSE(b.quotient_changed(nullptr, 0.87f, 1.15f));
		CHECK_FALSE(b.has_changed(nullptr, 0.87f, 1.15f));
		b.calculate_urgency(nullptr, 6.8f);
		CHECK_FALSE(b.quotient_changed(nullptr, 0.87f, 1.15f));
		b.calculate_urgency(nullptr, 7.0f);
		CHECK(b.quotient_changed(nullptr, 0.87f, 1.15f));
		CHECK_FALSE(b.has_changed(nullptr, 0.87f, 1.15f));
		b.calculate_urgency(nullptr, 5.3f);
		CHECK_FALSE(b.quotient_changed(nullptr, 0.87f, 1.15f));
		b.calculate_urgency(nullptr, 5.1f);
		CHECK(b.quotient_changed(nullptr, 0.87f, 1.15f));
		b.level = 0.0f;
		CHECK(b.calculate_urgency(nullptr, 6.0f) == 0.0f);
		CHECK(b.quotient_changed(nullptr, 0.87f, 1.15f));
		b.update_active_urgency(nullptr);
		CHECK_FALSE(b.quotient_changed(nullptr, 0.87f, 1.15f));
		b.level = 1.0f;
		b.calculate_urgency(nullptr, 1.0f);
		CHECK(b.quotient_changed(nullptr, 0.87f, 1.15f));
		b.refuse(body.now);
		CHECK(b.calculate_urgency(nullptr, 1.0f) == 0.0f);
		body.now += 1;
		CHECK(b.calculate_urgency(nullptr, 1.0f) == doctest::Approx(1.0f));
		CHECK(&b.urge_curve() == &curve);
		b.reset(nullptr);
		CHECK(b.urgency(nullptr) == 0.0f);
		CHECK(b.active_urgency(nullptr) == doctest::Approx(1e-4f));
	}

	TEST_CASE("BotMemory: the remembered behaviour's urge curve at the seconds since it was chosen, 1 for the others") {
		FakeBody body;
		const UCLinear curve(-0.5f, 2.0f);
		Level b(curve, body);
		BotMemory memory;
		CHECK(memory.last_behaviour == -1);
		CHECK(memory.modify_behaviour_urgency(b, 10.0) == 1.0f);
		memory.last_behaviour = int(InfantryBehaviour::Idle);
		memory.chosen_time = 9.0;
		CHECK(memory.modify_behaviour_urgency(b, 10.0) == doctest::Approx(1.5f));
		CHECK(memory.modify_behaviour_urgency(b, 13.0) == 0.0f);
		memory.last_behaviour = int(InfantryBehaviour::Random);
		CHECK(memory.modify_behaviour_urgency(b, 10.0) == 1.0f);
	}

	TEST_CASE("BotMain: a choice holds until an urgency leaves 0.87..1.15 of the active one or a target changes") {
		FakeBody body;
		body.s.navigable = true;
		BehaviourTuning tuning;
		BotMain bot(0, 3, 1, body, tuning);
		bot.update_bot(true, true);
		const float far = tuning.squad_far_m;
		SquadGoal g;
		g.kind = SquadGoalKind::Slot;
		const auto decide = [&](float p_distance) {
			g.position = { p_distance * far, 0.0f, 0.0f };
			bot.set_squad_goal(g);
			body.now += 1;
			bot.decision_making(sim::kTickSeconds);
			return bot.behaviour();
		};
		CHECK(decide(0.7f) == InfantryBehaviour::MoveTo);
		CHECK(bot.chosen_urgency() == doctest::Approx(1.05f));
		CHECK(decide(0.64f) == InfantryBehaviour::MoveTo);
		CHECK(bot.weighted_urgency(InfantryBehaviour::MoveTo) == doctest::Approx(0.96f));
		CHECK(bot.weighted_urgency(InfantryBehaviour::Random) == doctest::Approx(1.0f));
		CHECK(bot.chosen_urgency() == doctest::Approx(1.05f));
		CHECK(decide(0.6f) == InfantryBehaviour::Random);
		CHECK(bot.chosen_urgency() == doctest::Approx(1.0f));
		CHECK(decide(0.7f) == InfantryBehaviour::MoveTo);
		body.s.blind_fire = true;
		CHECK(decide(0.7f) == InfantryBehaviour::FireInfantery);
		CHECK(bot.memory().last_behaviour == int(InfantryBehaviour::FireInfantery));
		CHECK(bot.memory().chosen_time == doctest::Approx(double(body.now) * sim::kTickSeconds));
		body.s.blind_fire = false;
		bot.invalidate_plan();
		CHECK(decide(0.64f) == InfantryBehaviour::Random);
	}

	TEST_CASE("BotMain: the last behaviour's urge curve scales its weight from one decision to the next") {
		FakeBody body;
		body.s.navigable = true;
		BehaviourTuning tuning;
		tuning.curves[size_t(InfantryBehaviour::Random)] = { 1.0f, -6.0f, 1.0f };
		BotMain bot(0, 3, 1, body, tuning);
		bot.update_bot(true, true);
		bot.decision_making(sim::kTickSeconds);
		CHECK(bot.behaviour() == InfantryBehaviour::Random);
		body.now += 6;
		bot.decision_making(sim::kTickSeconds);
		CHECK(bot.weighted_urgency(InfantryBehaviour::Random) == doctest::Approx(0.4f));
		CHECK(bot.behaviour() == InfantryBehaviour::Idle);
		body.now += 6;
		bot.decision_making(sim::kTickSeconds);
		CHECK(bot.weighted_urgency(InfantryBehaviour::Random) == doctest::Approx(1.0f));
		CHECK(bot.behaviour() == InfantryBehaviour::Random);
	}
}
