#include <doctest/doctest.h>

#include "ai_infantry_fakes.h"
#include "ai_knowledge_fakes.h"
#include "bf3/ai/behaviour/behaviours.h"
#include "bf3/ai/behaviour/curves.h"
#include "bf3/ai/behaviour/urgency_merger.h"
#include "bf3/ai/bot/bot_main.h"
#include "bf3/ai/bot/bot_memory.h"
#include "bf3/ai/bot/waypoints.h"
#include "bf3/ai/squad/squad_world.h"

#include <vector>

using namespace bf3;
using namespace bf3::ai;
using bf3::ai::test::FakeBody;
using bf3::ai::test::FakeObject;
using bf3::ai::test::Knowledge;

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

struct Areas final : WaypointAreas {
	bool held = false;
	int inside_area = -1;
	mutable int randomized = 0;
	bool exists(int p_area) const override { return p_area == 3; }
	bool owned(int, int) const override { return held; }
	bool inside(int p_area, const Vec3 &) const override { return p_area == inside_area; }
	float size(int, int) const override { return 6.0f; }
	void randomize_position(int, int, float, Vec3 &r_position) const override {
		++randomized;
		r_position.x += 40.0f;
	}
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

	TEST_CASE("WPMoveTo: intermediate goals pop within the step radius or inside their area, the last one's urgency") {
		Areas areas;
		const WaypointBot bot{ 1, 0.5f, false };
		WPMoveTo free(4.0f, 3.0f, std::vector<Vec3>{ { 0.0f, 0.0f, 0.0f }, { 20.0f, 0.0f, 0.0f } }, -1, areas);
		CHECK(free.kind() == WaypointKind::MoveTo);
		CHECK(free.radius() == 4.0f);
		Vec3 goal{};
		REQUIRE(free.goal_point(goal));
		CHECK(goal.x == 0.0f);
		free.clear_changed();
		CHECK(free.urgency({ 9.0f, 0.0f, 0.0f }, bot) == 1.0f);
		CHECK(free.goals().size() == 1u);
		CHECK(free.changed());
		REQUIRE(free.goal_point(goal));
		CHECK(goal.x == 20.0f);
		CHECK(free.urgency({ 0.0f, 0.0f, 0.0f }, bot) == doctest::Approx(1.0f));
		CHECK_FALSE(free.finished());
		CHECK(free.urgency({ 14.0f, 0.0f, 0.0f }, bot) == doctest::Approx(1.0f));
		CHECK_FALSE(free.finished());
		CHECK(free.urgency({ 15.5f, 0.0f, 0.0f }, bot) == doctest::Approx(20.25f / 24.0f));
		CHECK(free.finished());
		CHECK(free.urgency({ 17.0f, 0.0f, 0.0f }, bot) == 0.0f);
		CHECK(free.finished());
		std::vector<Vec3> left;
		free.coords(left);
		REQUIRE(left.size() == 1u);
		CHECK(left[0].x == 20.0f);

		WPMoveTo stepped(
				4.0f, 0.5f, WPMoveTo::Goals{ { { 0.0f, 0.0f, 0.0f }, 3 }, { { 30.0f, 0.0f, 0.0f }, 3 } }, areas);
		CHECK(stepped.urgency({ 1.5f, 0.0f, 0.0f }, bot) == 1.0f);
		CHECK(stepped.goals().size() == 2u);
		CHECK(stepped.urgency({ 0.5f, 0.0f, 0.0f }, bot) == 1.0f);
		CHECK(stepped.goals().size() == 1u);

		areas.inside_area = 3;
		WPMoveTo through(
				4.0f, 2.0f, WPMoveTo::Goals{ { { 0.0f, 0.0f, 0.0f }, 3 }, { { 50.0f, 0.0f, 0.0f }, 3 } }, areas);
		CHECK(through.urgency({ 40.0f, 0.0f, 0.0f }, bot) == 1.0f);
		CHECK(through.goals().size() == 1u);
		areas.inside_area = -1;
		WPMoveTo outside(
				4.0f, 2.0f, WPMoveTo::Goals{ { { 0.0f, 0.0f, 0.0f }, 3 }, { { 50.0f, 0.0f, 0.0f }, 3 } }, areas);
		CHECK(outside.urgency({ 40.0f, 0.0f, 0.0f }, bot) == 1.0f);
		CHECK(outside.goals().size() == 2u);
		outside.set_error(true);
		CHECK(outside.urgency({ 40.0f, 0.0f, 0.0f }, bot) == 1.0f);
		CHECK_FALSE(outside.error());
		CHECK(areas.randomized == 1);
		CHECK(outside.goals().front().first.x == 40.0f);
		WPMoveTo lost(4.0f, 2.0f, WPMoveTo::Goals{ { { 0.0f, 0.0f, 0.0f }, 7 }, { { 50.0f, 0.0f, 0.0f }, 7 } }, areas);
		CHECK(lost.urgency({ 40.0f, 0.0f, 0.0f }, bot) == 1.0f);
		CHECK(lost.goals().size() == 2u);
		lost.set_error(true);
		CHECK(lost.urgency({ 40.0f, 0.0f, 0.0f }, bot) == 1.0f);
		CHECK(lost.goals().size() == 1u);
	}

	TEST_CASE("WPMoveTo: the last goal's area doubles an unheld area, ends inside a held one, its radius widens it") {
		Areas areas;
		const WaypointBot bot{ 1, 0.5f, false };
		areas.held = true;
		WPMoveTo held(4.0f, 2.0f, WPMoveTo::Goals{ { { 100.0f, 0.0f, 0.0f }, 3 } }, areas);
		CHECK(held.urgency({ 90.0f, 0.0f, 0.0f }, bot) == doctest::Approx(1.0f));
		CHECK(held.urgency({ 95.0f, 0.0f, 0.0f }, bot) == doctest::Approx(0.1f));
		CHECK(held.finished());
		areas.inside_area = 3;
		CHECK(held.urgency({ 90.0f, 0.0f, 0.0f }, bot) == 0.0f);
		CHECK(held.finished());
		areas.inside_area = -1;
		areas.held = false;
		WPMoveTo taken(4.0f, 2.0f, WPMoveTo::Goals{ { { 100.0f, 0.0f, 0.0f }, 3 } }, areas);
		CHECK(taken.urgency({ 95.5f, 0.0f, 0.0f }, bot) == doctest::Approx(2.0f * 20.25f / 24.0f));
		CHECK(taken.urgency({ 0.0f, 0.0f, 0.0f }, bot) == doctest::Approx(2.0f));
		taken.set_error(true);
		CHECK(taken.urgency({ 0.0f, 0.0f, 0.0f }, bot) == doctest::Approx(2.0f));
		CHECK(areas.randomized == 1);
		CHECK_FALSE(taken.error());
		WPMoveTo none(4.0f, 2.0f, WPMoveTo::Goals{ { { 100.0f, 0.0f, 0.0f }, 9 } }, areas);
		none.set_error(true);
		CHECK(none.urgency({ 0.0f, 0.0f, 0.0f }, bot) == 0.0f);
		const WaypointBot wide{ 1, 6.0f, false };
		WPMoveTo big(4.0f, 2.0f, std::vector<Vec3>{ { 0.0f, 0.0f, 0.0f } }, -1, areas);
		CHECK(big.urgency({ 5.5f, 0.0f, 0.0f }, wide) == 0.0f);
		WPMoveTo empty(4.0f, 2.0f, WPMoveTo::Goals{}, areas);
		CHECK(empty.urgency({}, bot) == 0.0f);
		CHECK(empty.finished());
		Vec3 g{};
		CHECK_FALSE(empty.goal_point(g));
	}

	TEST_CASE("WPCloseTo, WPPatrol and WPEmpty: close in on an object, cycle a loop, stand still") {
		Knowledge k;
		FakeObject mate;
		mate.where = { 20.0f, 0.0f, 0.0f };
		const uint32_t handle = k.soldier(mate, 1);
		WPCloseTo close(handle, 2.0f, k.information);
		CHECK(close.kind() == WaypointKind::CloseTo);
		CHECK(close.radius() == 5.0f);
		Vec3 goal{};
		REQUIRE(close.goal_point(goal));
		CHECK(goal.x == 20.0f);
		const WaypointBot bot{ 1, 0.5f, false };
		close.clear_changed();
		CHECK(close.urgency({}, bot) == 1.0f);
		CHECK_FALSE(close.changed());
		CHECK(close.urgency({ 10.0f, 0.0f, 0.0f }, bot) == doctest::Approx(1.0f));
		CHECK(close.urgency({ 12.0f, 0.0f, 0.0f }, bot) == doctest::Approx(64.0f / 100.0f));
		mate.where = { 30.0f, 0.0f, 0.0f };
		CHECK(close.urgency({ 12.0f, 0.0f, 0.0f }, bot) == doctest::Approx(1.0f));
		CHECK(close.changed());
		REQUIRE(close.goal_point(goal));
		CHECK(goal.x == 30.0f);
		close.clear_changed();
		CHECK(close.urgency({ 27.0f, 0.0f, 0.0f }, bot) == 0.0f);
		CHECK(close.changed());
		CHECK(close.modify_max_speed(4.0f) == doctest::Approx(4.0f));
		close.set_error(true);
		CHECK(close.urgency({ 0.0f, 0.0f, 0.0f }, bot) == 0.0f);
		WPCloseTo gone(kNoHandle, 8.0f, k.information);
		CHECK(gone.radius() == 8.0f);
		CHECK(gone.urgency({}, bot) == 0.0f);
		CHECK(gone.finished());
		CHECK_FALSE(gone.goal_point(goal));
		CHECK(gone.modify_max_speed(3.0f) == 3.0f);

		WPPatrol patrol(2.0f, { { 0.0f, 0.0f, 0.0f }, { 10.0f, 0.0f, 0.0f } });
		CHECK(patrol.radius() == 5.0f);
		CHECK(patrol.kind() == WaypointKind::Patrol);
		CHECK(patrol.urgency({ 0.0f, 50.0f, 4.0f }, bot) == 1.0f);
		REQUIRE(patrol.goal_point(goal));
		CHECK(goal.x == 10.0f);
		CHECK(patrol.urgency({ 0.0f, 0.0f, 0.0f }, bot) == 1.0f);
		REQUIRE(patrol.goal_point(goal));
		CHECK(goal.x == 10.0f);
		CHECK(patrol.urgency({ 9.0f, 0.0f, 0.0f }, bot) == 1.0f);
		std::vector<Vec3> loop;
		patrol.coords(loop);
		REQUIRE(loop.size() == 2u);
		CHECK(loop[0].x == 0.0f);
		patrol.set_error(true);
		CHECK(patrol.urgency({ 0.0f, 0.0f, 0.0f }, bot) == 0.0f);

		WPEmpty empty({ 1.0f, 2.0f, 3.0f });
		CHECK(empty.kind() == WaypointKind::Empty);
		REQUIRE(empty.goal_point(goal));
		CHECK(goal.y == 2.0f);
		CHECK(empty.urgency({ 4.0f, 5.0f, 6.0f }, bot) == 0.0f);
		REQUIRE(empty.goal_point(goal));
		CHECK(goal.y == 5.0f);
		CHECK(empty.speed() == -1.0f);
	}

	TEST_CASE("altitude waypoints: flight goals at a height, switched off with the air AI") {
		Knowledge k;
		FakeObject pad;
		pad.where = { 0.0f, 0.0f, 0.0f };
		const uint32_t handle = k.soldier(pad, 1);
		const WaypointBot flying{ 1, 2.0f, true };
		const WaypointBot landed{ 1, 2.0f, false };
		WPAltitudeCloseTo over(handle, 10.0f, 50.0f, 30.0f, k.information);
		CHECK(over.kind() == WaypointKind::AltitudeCloseTo);
		CHECK(over.distance() == 30.0f);
		CHECK(over.urgency({ 5.0f, 40.0f, 0.0f }, flying) == 0.0f);
		CHECK(over.urgency({ 5.0f, 60.0f, 0.0f }, flying) == 1.0f);
		CHECK(over.urgency({ 11.0f, 40.0f, 0.0f }, flying) == 0.0f);
		CHECK(over.urgency({ 3.0f, 40.0f, 3.0f }, flying) == 0.0f);
		CHECK(over.urgency({ 4.0f, 40.0f, 3.0f }, flying) == 1.0f);

		WPAltitudeMoveTo route(10.0f, 5.0f, 40.0f, { { 0.0f, 100.0f, 0.0f }, { 200.0f, 100.0f, 0.0f } });
		CHECK(route.kind() == WaypointKind::AltitudeMoveTo);
		CHECK(route.urgency({ 50.0f, 100.0f, 0.0f }, flying) == 1.0f);
		CHECK(route.urgency({ 60.0f, 100.0f, 0.0f }, flying) == 1.0f);
		Vec3 goal{};
		REQUIRE(route.goal_point(goal));
		CHECK(goal.x == 200.0f);
		CHECK(route.urgency({ 180.0f, 100.0f, 0.0f }, flying) == 0.0f);
		CHECK(route.urgency({ 180.0f, 100.0f, 0.0f }, landed) == 1.0f);
		CHECK(route.urgency({ 197.0f, 102.0f, 0.0f }, landed) == doctest::Approx(13.0f / 125.0f));

		WPAltitudePatrol loop(10.0f, 5.0f, 40.0f, { { 0.0f, 100.0f, 0.0f }, { 100.0f, 100.0f, 0.0f } });
		CHECK(loop.kind() == WaypointKind::AltitudePatrol);
		CHECK(loop.urgency({ 0.0f, 120.0f, 0.0f }, flying) == 1.0f);
		REQUIRE(loop.goal_point(goal));
		CHECK(goal.x == 0.0f);
		CHECK(loop.urgency({ 0.0f, 103.0f, 0.0f }, flying) == 1.0f);
		REQUIRE(loop.goal_point(goal));
		CHECK(goal.x == 100.0f);
	}
}
