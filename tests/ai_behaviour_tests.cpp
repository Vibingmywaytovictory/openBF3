#include <doctest/doctest.h>

#include "ai_infantry_fakes.h"
#include "bf3/ai/behaviour/behaviours.h"
#include "bf3/ai/behaviour/curves.h"
#include "bf3/ai/behaviour/urgency_merger.h"
#include "bf3/ai/bot/bot_main.h"

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

	TEST_CASE("BotBehaviour: the urgency is the curve times the modifier; its quotient against the active one") {
		FakeBody body;
		const UCLinear curve(2.0f, 0.0f);
		Level b(curve, body);
		b.reset(nullptr);
		CHECK(b.urgency(nullptr) == 0.0f);
		CHECK(b.active_urgency(nullptr) == doctest::Approx(1e-4f));
		CHECK(b.has_changed_target(nullptr));
		CHECK(b.quotient_changed(nullptr, 0.87f, 1.15f));
		b.level = 0.5f;
		CHECK(b.calculate_urgency(nullptr, true, 3.0f) == doctest::Approx(3.0f));
		CHECK(b.urgency(nullptr) == doctest::Approx(3.0f));
		CHECK(b.update_active_urgency(nullptr) == doctest::Approx(3.0f));
		CHECK_FALSE(b.quotient_changed(nullptr, 0.87f, 1.15f));
		CHECK_FALSE(b.has_changed(nullptr, 0.87f, 1.15f));
		b.calculate_urgency(nullptr, true, 3.4f);
		CHECK_FALSE(b.quotient_changed(nullptr, 0.87f, 1.15f));
		b.calculate_urgency(nullptr, true, 3.5f);
		CHECK(b.quotient_changed(nullptr, 0.87f, 1.15f));
		CHECK(b.has_changed(nullptr, 0.87f, 1.15f));
		b.calculate_urgency(nullptr, true, 2.65f);
		CHECK_FALSE(b.quotient_changed(nullptr, 0.87f, 1.15f));
		b.calculate_urgency(nullptr, true, 2.55f);
		CHECK(b.quotient_changed(nullptr, 0.87f, 1.15f));
		b.level = 0.0f;
		CHECK(b.calculate_urgency(nullptr, true, 3.0f) == 0.0f);
		CHECK(b.quotient_changed(nullptr, 0.87f, 1.15f));
		b.update_active_urgency(nullptr);
		CHECK_FALSE(b.quotient_changed(nullptr, 0.87f, 1.15f));
		b.level = 1.0f;
		b.calculate_urgency(nullptr, true, 1.0f);
		CHECK(b.quotient_changed(nullptr, 0.87f, 1.15f));
		b.refuse(body.now);
		CHECK(b.calculate_urgency(nullptr, true, 1.0f) == 0.0f);
		body.now += 1;
		CHECK(b.calculate_urgency(nullptr, true, 1.0f) == doctest::Approx(2.0f));
		b.reset(nullptr);
		CHECK(b.urgency(nullptr) == 0.0f);
		CHECK(b.active_urgency(nullptr) == doctest::Approx(1e-4f));
	}
}
