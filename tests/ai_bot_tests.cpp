#include <doctest/doctest.h>

#include "bf3/ai/behaviour/behaviours.h"
#include "bf3/ai/bot/bot_main.h"
#include "bf3/ai/plan/infantry_entries.h"
#include "bf3/ai/plan/plan_entries.h"
#include "bf3/host/ai_bot_source.h"
#include "bf3/host/game_server.h"
#include "bf3/host/server_view.h"
#include "bf3/host/tdm_brain.h"

#include "gunplay_fixture.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace bf3;
using namespace bf3::ai;

namespace {

struct FakeBody final : InfantryBody {
	sim::Tick now = 1;
	Vec3 at{};
	InfantrySenses s;
	bool throw_possible = false;
	bool cover_ok = true;
	bool recovered = false;
	bool cleared = false;
	bool running = true;
	int throws = 0, covers = 0, waits = 0, sees = 0, sensings = 0, plans = 0, ends = 0;
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
	void end_plan() override { ++ends; }
};

InfantryBehaviour argmax(const BotMain &p_bot) {
	InfantryBehaviour best = InfantryBehaviour::Idle;
	float u = 0.0f;
	for (size_t i = 0; i < kInfantryBehaviourCount; ++i) {
		const float w = p_bot.weighted_urgency(InfantryBehaviour(i));
		if (w > u) {
			u = w;
			best = InfantryBehaviour(i);
		}
	}
	return best;
}

struct Rig {
	FakeBody body;
	BehaviourTuning tuning;
	BotMain bot{ 0, 3, 1, body, tuning };

	InfantryBehaviour decide() {
		bot.update_bot(true, false);
		bot.see(sim::kTickSeconds);
		bot.update_sensing(sim::kTickSeconds);
		const uint64_t refusals = bot.stats().refusals;
		bot.decision_making(sim::kTickSeconds);
		bot.plan_execution(sim::kTickSeconds);
		CHECK(bot.behaviour() == argmax(bot));
		CHECK(bot.chosen_urgency() == doctest::Approx(bot.weighted_urgency(bot.behaviour())));
		if (bot.stats().refusals == refusals) {
			CHECK(bot.stats().chosen[size_t(bot.behaviour())] > 0u);
		}
		return bot.behaviour();
	}
};

}

TEST_SUITE("ai-bot") {
	TEST_CASE("BotMain: every plan is UrgencyMerger's winner, ranked as TdmBrain's chain ranked its activities") {
		Rig r;
		r.bot.update_bot(true, true);
		CHECK(r.decide() == InfantryBehaviour::Idle);
		CHECK(r.bot.activity() == InfantryActivity::Idle);
		CHECK(r.body.calls == std::vector<std::string>{ "sense 4" });
		CHECK(r.body.sees == 1);
		CHECK(r.body.sensings == 1);
		CHECK(r.body.ends == 1);

		r.body.s.navigable = true;
		CHECK(r.decide() == InfantryBehaviour::Random);
		CHECK(r.bot.activity() == InfantryActivity::Roam);
		CHECK(r.body.calls == std::vector<std::string>{ "move_to 0" });

		r.body.s.hunt_target = true;
		CHECK(r.decide() == InfantryBehaviour::MoveTo);
		CHECK(r.bot.activity() == InfantryActivity::Hunt);
		CHECK(r.body.calls == std::vector<std::string>{ "move_to 1" });

		r.body.s.shot_at = true;
		CHECK(r.decide() == InfantryBehaviour::Avoid);
		CHECK(r.bot.activity() == InfantryActivity::Hunt);
		CHECK(r.body.calls == std::vector<std::string>{ "move_to 1", "sense 3" });
		r.body.s.hunt_target = false;
		CHECK(r.decide() == InfantryBehaviour::Avoid);
		CHECK(r.bot.activity() == InfantryActivity::Roam);

		r.body.s.blind_fire = true;
		CHECK(r.decide() == InfantryBehaviour::FireInfantery);
		CHECK(r.bot.activity() == InfantryActivity::Fight);
		CHECK(r.body.calls == std::vector<std::string>{ "sense 1", "trigger 1" });

		r.body.throw_possible = true;
		CHECK(r.decide() == InfantryBehaviour::FireInfantery);
		CHECK(r.bot.activity() == InfantryActivity::Grenade);
		CHECK(r.body.throws == 1);
		CHECK(r.body.calls == std::vector<std::string>{ "trigger 2" });
		r.body.s.grenade_running = false;

		r.body.s.enemy_visible = true;
		CHECK(r.decide() == InfantryBehaviour::FireInfantery);
		CHECK(r.bot.activity() == InfantryActivity::Fight);
		CHECK(r.body.calls == std::vector<std::string>{ "sense 0", "trigger 0", "move_to_object 0" });

		r.body.s.low_health = true;
		r.body.cover_ok = false;
		CHECK(r.decide() == InfantryBehaviour::FireInfantery);
		CHECK(r.body.covers == 1);
		CHECK(r.bot.stats().refusals == 1u);
		r.body.now += 1;
		r.body.cover_ok = true;
		CHECK(r.decide() == InfantryBehaviour::TakeCoverInfantry);
		CHECK(r.bot.activity() == InfantryActivity::Retreat);
		CHECK(r.body.covers == 2);
		CHECK(r.body.calls == std::vector<std::string>{ "move_to 2" });

		r.body.s.grenade_running = true;
		CHECK(r.decide() == InfantryBehaviour::FireInfantery);
		CHECK(r.bot.activity() == InfantryActivity::Grenade);
		r.body.s.grenade_running = false;
		CHECK(r.decide() == InfantryBehaviour::TakeCoverInfantry);
		CHECK(r.body.covers == 3);

		r.body.s.revive_target = true;
		CHECK(r.decide() == InfantryBehaviour::Revive);
		CHECK(r.bot.activity() == InfantryActivity::Revive);
		CHECK(r.body.calls == std::vector<std::string>{ "move_to_object 1", "pose 1", "trigger 3" });
		r.body.s.revive_target = false;

		for (size_t i = 0; i < kInfantryBehaviourCount; ++i) {
			CHECK(r.bot.stats().chosen[i] > 0u);
		}
		CHECK(r.bot.stats().decisions == r.bot.stats().planned + r.bot.stats().unplanned);
	}

	TEST_CASE("BBTakeCoverInfantry: retreat, wait in cover once cleared, leave when recovered") {
		Rig r;
		r.bot.update_bot(true, true);
		r.body.s = { .enemy_visible = true, .low_health = true, .navigable = true };
		CHECK(r.decide() == InfantryBehaviour::TakeCoverInfantry);
		CHECK(r.bot.activity() == InfantryActivity::Retreat);
		CHECK(r.body.covers == 1);
		r.body.s.enemy_visible = false;
		r.body.s.shot_at = true;
		CHECK(r.decide() == InfantryBehaviour::TakeCoverInfantry);
		CHECK(r.bot.activity() == InfantryActivity::Retreat);
		CHECK(r.body.covers == 1);
		r.body.cleared = true;
		CHECK(r.decide() == InfantryBehaviour::TakeCoverInfantry);
		CHECK(r.bot.activity() == InfantryActivity::RegenWait);
		CHECK(r.body.waits == 1);
		CHECK(r.body.calls == std::vector<std::string>{ "pose 0", "sense 2", "sense 3" });
		r.body.s.shot_at = false;
		CHECK(r.decide() == InfantryBehaviour::TakeCoverInfantry);
		CHECK(r.bot.activity() == InfantryActivity::RegenWait);
		CHECK(r.body.waits == 1);
		r.body.recovered = true;
		r.body.s.low_health = false;
		CHECK(r.decide() == InfantryBehaviour::Random);
		CHECK(static_cast<const BBTakeCoverInfantry &>(r.bot.behaviour_of(InfantryBehaviour::TakeCoverInfantry))
						.phase() == BBTakeCoverInfantry::Phase::None);
		r.body.recovered = false;
		r.body.s.shot_at = true;
		CHECK(r.decide() == InfantryBehaviour::Avoid);
		r.body.s.low_health = true;
		CHECK(r.decide() == InfantryBehaviour::TakeCoverInfantry);
		CHECK(r.body.covers == 2);
	}

	TEST_CASE("decisions: every decision_ticks staggered by player id, and at once when a sense changes") {
		FakeBody body;
		body.s.navigable = true;
		BehaviourTuning tuning;
		REQUIRE(tuning.decision_ticks == 6u);
		std::vector<std::unique_ptr<BotMain>> bots;
		for (int p = 0; p < 6; ++p) {
			bots.push_back(std::make_unique<BotMain>(uint32_t(p), p, 1, body, tuning));
			bots.back()->update_bot(true, true);
		}
		std::array<std::vector<sim::Tick>, 6> decided;
		for (sim::Tick t = 1; t <= 60; ++t) {
			body.now = t;
			for (size_t p = 0; p < bots.size(); ++p) {
				if (bots[p]->wants_decision()) {
					bots[p]->decision_making(sim::kTickSeconds);
					decided[p].push_back(t);
				}
				bots[p]->plan_execution(sim::kTickSeconds);
			}
		}
		for (size_t p = 0; p < bots.size(); ++p) {
			CHECK(decided[p].size() == (p == 5 ? 10u : 11u));
			CHECK(bots[p]->stats().unplanned == 1u);
			for (size_t i = 1; i < decided[p].size(); ++i) {
				CHECK((decided[p][i] + p) % 6 == 0);
			}
		}
		for (size_t p = 1; p < bots.size(); ++p) {
			CHECK(decided[p][1] != decided[0][1]);
		}
		body.now = 61;
		CHECK_FALSE(bots[0]->wants_decision());
		body.s.enemy_visible = true;
		CHECK(bots[0]->wants_decision());
		bots[0]->decision_making(sim::kTickSeconds);
		CHECK(bots[0]->behaviour() == InfantryBehaviour::FireInfantery);
		CHECK_FALSE(bots[0]->wants_decision());
		CHECK(bots[0]->stats().unplanned == 2u);
	}

	TEST_CASE("BBMoveTo: the squad goal's urgency grows with distance through its UrgeCurve") {
		Rig r;
		r.bot.update_bot(true, true);
		r.body.s.navigable = true;
		SquadGoal g;
		g.kind = SquadGoalKind::Slot;
		g.position = { 25.0f, 0.0f, 0.0f };
		r.bot.set_squad_goal(g);
		CHECK(r.decide() == InfantryBehaviour::MoveTo);
		CHECK(r.bot.activity() == InfantryActivity::Roam);
		CHECK(r.body.calls == std::vector<std::string>{ "move_to 3" });
		CHECK(r.bot.weighted_urgency(InfantryBehaviour::MoveTo) == doctest::Approx(1.5f * 25.0f / 30.0f));
		g.position = { 10.0f, 0.0f, 0.0f };
		r.bot.set_squad_goal(g);
		CHECK(r.decide() == InfantryBehaviour::Random);
		CHECK(r.bot.weighted_urgency(InfantryBehaviour::MoveTo) == doctest::Approx(0.5f));
		g.position = { 2.0f, 0.0f, 0.0f };
		r.bot.set_squad_goal(g);
		CHECK(r.decide() == InfantryBehaviour::Random);
		CHECK(r.bot.weighted_urgency(InfantryBehaviour::MoveTo) == 0.0f);
		r.body.s.hunt_target = true;
		CHECK(r.decide() == InfantryBehaviour::MoveTo);
		CHECK(r.bot.activity() == InfantryActivity::Hunt);
	}

	TEST_CASE("orders and personality: BF2's calculateMods scales each behaviour's urgency") {
		Rig r;
		r.bot.update_bot(true, true);
		r.body.s.navigable = true;
		r.body.s.blind_fire = true;
		CHECK(r.decide() == InfantryBehaviour::FireInfantery);
		std::array<float, kInfantryBehaviourCount> order{};
		order.fill(1.0f);
		order[size_t(InfantryBehaviour::Random)] = 10.0f;
		CHECK_FALSE(r.bot.receive_order(BotOrder(order.data(), uint8_t(InfantryBehaviour::Random))));
		r.bot.set_taking_orders(true);
		CHECK(r.bot.receive_order(BotOrder(order.data(), uint8_t(InfantryBehaviour::Random))));
		CHECK(r.bot.ordered_behaviour() == int(InfantryBehaviour::Random));
		CHECK(r.decide() == InfantryBehaviour::Random);
		CHECK(r.bot.weighted_urgency(InfantryBehaviour::Random) == doctest::Approx(10.0f));
		r.bot.set_taking_orders(false);
		CHECK(r.decide() == InfantryBehaviour::FireInfantery);
		std::array<float, kInfantryBehaviourCount> calm{};
		calm.fill(1.0f);
		calm[size_t(InfantryBehaviour::FireInfantery)] = 0.0f;
		r.bot.set_personality(calm.data());
		CHECK(r.decide() == InfantryBehaviour::Random);
	}

	TEST_CASE(
			"bb.* tuning: the defaults round-trip, out-of-range and unknown keys refused, weights reorder the choice") {
		const BehaviourTuning def;
		BehaviourTuning t;
		const std::string text = format_behaviour_tuning(def);
		CHECK(text.find("bb.fire_infantery.weight = 7") != std::string::npos);
		size_t start = 0;
		while (start < text.size()) {
			const size_t end = text.find('\n', start);
			const std::string line = text.substr(start, end - start);
			const size_t eq = line.find(" = ");
			REQUIRE(eq != std::string::npos);
			CHECK(parse_behaviour_setting(line.substr(0, eq), line.substr(eq + 3), t).fault == core::KeyFault::None);
			start = end == std::string::npos ? text.size() : end + 1;
		}
		CHECK(t == def);
		CHECK(is_behaviour_key("bb.idle.weight"));
		CHECK_FALSE(is_behaviour_key("ai.max_bots"));
		CHECK(parse_behaviour_setting("bb.nope", "1", t).fault == core::KeyFault::UnknownKey);
		CHECK(parse_behaviour_setting("bb.decision_ticks", "0", t).fault == core::KeyFault::OutOfRange);
		CHECK(parse_behaviour_setting("bb.decision_ticks", "61", t).fault == core::KeyFault::OutOfRange);
		CHECK(parse_behaviour_setting("bb.idle.weight", "nan", t).fault != core::KeyFault::None);
		CHECK(parse_behaviour_setting("bb.idle.weight", "-1", t).fault == core::KeyFault::OutOfRange);
		CHECK(t == def);

		Rig r;
		REQUIRE(parse_behaviour_setting("bb.random.weight", "20", r.tuning).fault == core::KeyFault::None);
		BotMain tuned(1, 1, 1, r.body, r.tuning);
		tuned.update_bot(true, true);
		r.body.s.navigable = true;
		r.body.s.enemy_visible = true;
		tuned.decision_making(sim::kTickSeconds);
		CHECK(tuned.behaviour() == InfantryBehaviour::Random);
		REQUIRE(parse_behaviour_setting("bb.random.slope", "0.1", r.tuning).fault == core::KeyFault::None);
		BotMain sloped(2, 2, 1, r.body, r.tuning);
		sloped.update_bot(true, true);
		sloped.decision_making(sim::kTickSeconds);
		CHECK(sloped.behaviour() == InfantryBehaviour::FireInfantery);
	}

	TEST_CASE("infantry plan entries: the five bodies exist and drive the body through a BotScript") {
		AiSettings settings;
		FakeBody body;
		BotScript script(body);
		for (const char *name : { "EntryInfanteryMoveTo", "EntryInfanteryMoveToObject", "EntrySoldierPose",
					 "EntryTrigger", "EntrySense" }) {
			const PlanEntryKind *kind = find_plan_entry_kind(name);
			REQUIRE(kind != nullptr);
			CHECK(kind->has_body);
			const std::unique_ptr<BAPEntry> entry = create_plan_entry(name, settings);
			REQUIRE(entry != nullptr);
			CHECK(entry->target_id() == kind->target);
			CHECK(infantry_entry(kind->target) != nullptr);
		}
		CHECK(infantry_entry(PlanTarget::AimAt) == nullptr);
		script.clear(InfantryActivity::Fight);
		PlanArgs a;
		a.focus = SenseFocus::Target;
		CHECK(script.add(PlanTarget::Sense, a));
		a.trigger = TriggerKind::Aimed;
		CHECK(script.add(PlanTarget::Trigger, a));
		a.object = ObjectGoal::Target;
		CHECK(script.add(PlanTarget::MoveToObject, a));
		a.move = MoveGoal::Cover;
		CHECK(script.add(PlanTarget::MoveTo, a));
		CHECK_FALSE(script.add(PlanTarget::SoldierPose, a));
		CHECK_FALSE(script.add(PlanTarget::AimAt, a));
		CHECK(script.steps().size() == 4u);
		CHECK(script.execute(nullptr));
		CHECK(body.calls == std::vector<std::string>{ "sense 0", "trigger 0", "move_to_object 0", "move_to 2" });
		CHECK(script.done_steps() == 3u);
	}

	TEST_CASE("add_tdm_bots gives every bot an AiBotSource, and every activity is its last decision's winner") {
		struct DecisionCheck final : host::TdmBotObserver {
			std::array<InfantryActivity, 64> last{};
			std::array<bool, 64> seen{};
			uint64_t decisions = 0, activities = 0, mismatches = 0, undecided = 0, fights = 0;
			void decided(sim::Tick, uint8_t p_bot, InfantryBehaviour p_winner, InfantryActivity p_activity) override {
				last[p_bot] = p_activity;
				seen[p_bot] = true;
				++decisions;
				fights += p_winner == InfantryBehaviour::FireInfantery ? 1u : 0u;
			}
			void activity(sim::Tick, uint8_t p_bot, host::TdmBotActivity p_activity) override {
				if (p_activity == host::TdmBotActivity::Dead || p_activity == host::TdmBotActivity::Restricted) {
					return;
				}
				++activities;
				if (!seen[p_bot]) {
					++undecided;
				} else if (host::tdm_activity(last[p_bot]) != p_activity) {
					++mismatches;
				}
			}
		};
		const std::shared_ptr<const host::SharedLevel> level = bf3::test::gunplay::synthetic_floor();
		REQUIRE(level != nullptr);
		host::GameServer server;
		host::GameServerConfig cfg;
		cfg.level = level;
		cfg.max_slots = 16;
		auto bots = std::make_shared<host::TdmBotConfig>();
		bots->count = 8;
		bots->stats = std::make_shared<host::TdmBotStats>();
		auto check = std::make_shared<DecisionCheck>();
		bots->observer = check;
		cfg.tdm_bots = bots;
		REQUIRE(server.init(cfg).ok());
		CHECK(bots->stats->bots.load() == 8u);
		CHECK(bots->stats->bf2_bots.load() == 8u);
		std::vector<host::OutDatagram> out;
		for (int i = 0; i < 600; ++i) {
			out.clear();
			server.tick(out);
		}
		CHECK(check->activities > 8u * 500u);
		CHECK(check->undecided == 0u);
		CHECK(check->mismatches == 0u);
		CHECK(check->fights > 0u);
		CHECK(check->decisions == bots->stats->decisions.load());
		CHECK(check->decisions < check->activities / 3u);
		CHECK(bots->stats->unplanned_decisions.load() < check->decisions);
		MESSAGE("8 bots, 600 ticks: " << check->decisions << " decisions (" << bots->stats->unplanned_decisions.load()
									  << " on a sense change) over " << check->activities << " bot-ticks");
	}
}
