#include <doctest/doctest.h>

#include "bf3/ai/behaviour/behaviours.h"
#include "bf3/ai/behaviour/curves.h"
#include "bf3/ai/bot/bot_main.h"
#include "bf3/ai/main/ai_script.h"
#include "bf3/ai/vehicle/vehicle_change.h"

#include "ai_infantry_fakes.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

using namespace bf3;

namespace {

constexpr float kJeepSpeed = 25.0f;
constexpr float kSoldierSpeed = 7.0f;

ai::ChangeVehicle jeep(float p_x, int p_side = 1) {
	ai::ChangeVehicle v;
	v.kind = ai::ChangeUnitKind::Ground;
	v.side = p_side;
	v.position = { p_x, 1.0f, 0.0f };
	v.radius = 3.0f;
	v.max_speed = kJeepSpeed;
	v.seats = 4;
	return v;
}

ai::ChangeBot fresh_bot() {
	ai::ChangeBot b;
	b.player_id = 5;
	b.side = 1;
	b.radius = 0.4f;
	b.max_speed = kSoldierSpeed;
	b.now_s = 1.0;
	return b;
}

std::string change_script(std::string_view p_tenth, std::string_view p_tenth_kind) {
	std::string s = "aiSettings.setMaxNumberOfBehaviours 11\naiSettings.setVehicle Infantry\n"
					"aiSettings.createUCLinear Memory 0 1\n";
	const auto name = [&](size_t p_i) {
		return p_i < ai::kInfantryBehaviourCount
				? std::string(ai::infantry_behaviour_kind(ai::InfantryBehaviour(p_i)).substr(2))
				: std::string(p_tenth);
	};
	for (size_t i = 0; i < ai::kBotBehaviourCount; ++i) {
		s += "aiSettings.setBehaviour " + std::to_string(i) + " " + name(i) + "\n";
	}
	s += "aiSettings.createBehaviourModifiers Default\n";
	for (size_t i = 0; i < ai::kBotBehaviourCount; ++i) {
		s += "aiSettings.setBehaviourModifier " + name(i) + (i < ai::kInfantryBehaviourCount ? " 1\n" : " 1.5\n");
		const std::string kind = i < ai::kInfantryBehaviourCount
				? std::string(ai::infantry_behaviour_kind(ai::InfantryBehaviour(i)))
				: std::string(p_tenth_kind);
		s += "aiSettings.setVehicleBehaviour Infantry " + name(i) + " " + kind + " BBP" + name(i) +
				" 0 Memory Default\n";
	}
	return s + "aiSettings.setBasicBotWeights Default\n";
}

ai::BehaviourTuning unit_tuning() {
	ai::BehaviourTuning t;
	for (ai::BehaviourCurve &c : t.curves) {
		c.weight = 1.0f;
		c.offset = 1.0f;
	}
	return t;
}

void decide(ai::BotMain &r_bot) {
	r_bot.see(sim::kTickSeconds);
	r_bot.update_sensing(sim::kTickSeconds);
	r_bot.decision_making(sim::kTickSeconds);
	r_bot.plan_execution(sim::kTickSeconds);
}

}

TEST_SUITE("ai-vehicle") {
	TEST_CASE("a soldier 1 s into the game picks the nearer free jeep's driver seat with no possession ramp or x6") {
		const std::vector<ai::ChangeVehicle> vehicles{ jeep(60.0f), jeep(20.0f) };
		const ai::ChangeBot bot = fresh_bot();
		CHECK(ai::change_unit_urgency(bot, vehicles, ai::ChangeTarget{}, false) ==
				doctest::Approx(kSoldierSpeed * 5.0f));
		const ai::ChangeChoice c = ai::choose_change(bot, vehicles);
		REQUIRE(c.change);
		CHECK(c.target == ai::ChangeTarget{ 1, 0 });
		CHECK(c.best == doctest::Approx(kJeepSpeed * 5.0f));
		CHECK(c.current == doctest::Approx(2.0f * kSoldierSpeed * 5.0f));
		CHECK(c.urgency ==
				doctest::Approx(
						ai::DecliningSlopeCurve::calculate(kJeepSpeed * 5.0f * 0.5f / (2.0f * kSoldierSpeed * 5.0f)) *
						4.0f));
		CHECK_FALSE(c.get_out);
	}

	TEST_CASE("a seat's possession clock is never set unless a player left it, and only that seat's urgency ramps") {
		CHECK(ai::ChangeSeat{}.possessed_s == ai::kNeverPossessedSeconds);
		CHECK(ai::kNeverPossessedSeconds == -1000.0);
		ai::ChangeBot bot = fresh_bot();
		bot.now_s = 0.0;
		const std::vector<ai::ChangeVehicle> fresh{ jeep(20.0f) };
		CHECK(ai::change_unit_urgency(bot, fresh, ai::ChangeTarget{ 0, 0 }, true) ==
				doctest::Approx(kJeepSpeed * 5.0f));
		CHECK(ai::change_driver_modifier(fresh[0], false, bot.now_s) == doctest::Approx(0.77f));

		bot.now_s = 200.0;
		std::vector<ai::ChangeVehicle> gunner_left{ jeep(20.0f) };
		gunner_left[0].seat[1].possessed_s = bot.now_s - 6.0;
		CHECK(ai::change_unit_urgency(bot, gunner_left, ai::ChangeTarget{ 0, 0 }, true) ==
				doctest::Approx(kJeepSpeed * 5.0f));
		CHECK(ai::change_unit_urgency(bot, gunner_left, ai::ChangeTarget{ 0, 1 }, true) == 0.0f);
		CHECK(ai::change_driver_modifier(gunner_left[0], false, bot.now_s) == doctest::Approx(0.77f));

		bot.vehicle = 0;
		bot.seat = 0;
		std::vector<ai::ChangeVehicle> seated{ jeep(0.0f) };
		seated[0].seat[0].occupant = bot.player_id;
		seated[0].seat[0].possessed_s = bot.now_s - 6.0;
		seated[0].seat[2].possessed_s = bot.now_s - 1.0;
		const ai::ChangeChoice ramp = ai::choose_change(bot, seated);
		CHECK(ramp.current == doctest::Approx(2.0f * kJeepSpeed * 5.0f * 6.0f / 15.0f));
		CHECK_FALSE(ramp.change);
		seated[0].seat[0].possessed_s = bot.now_s;
		const ai::ChangeChoice stamped = ai::choose_change(bot, seated);
		CHECK(stamped.current == 0.0f);
		CHECK(stamped.get_out);
		CHECK(stamped.urgency ==
				doctest::Approx(ai::DecliningSlopeCurve::calculate(ai::kGetOutUrgency * 0.5f) * 4.0f * 6.0f * 5.0f));
		seated[0].seat[0].possessed_s = bot.now_s - 15.0;
		const ai::ChangeChoice settled = ai::choose_change(bot, seated);
		CHECK(settled.current == doctest::Approx(2.0f * kJeepSpeed * 5.0f));
		CHECK_FALSE(settled.change);
	}

	TEST_CASE("a passenger seat of an empty jeep scores nothing and its driver modifier is 0.77") {
		const std::vector<ai::ChangeVehicle> vehicles{ jeep(20.0f) };
		const ai::ChangeBot bot = fresh_bot();
		CHECK(ai::change_unit_urgency(bot, vehicles, ai::ChangeTarget{ 0, 1 }, true) == 0.0f);
		CHECK(ai::change_driver_modifier(vehicles[0], false, bot.now_s) == doctest::Approx(0.77f));
		ai::ChangeVehicle left = vehicles[0];
		left.seat[0].possessed_s = bot.now_s - 3.0;
		CHECK(ai::change_driver_modifier(left, false, bot.now_s) == 1.0f);
		const std::vector<ai::ChangeVehicle> just_left{ left };
		CHECK(ai::change_unit_urgency(bot, just_left, ai::ChangeTarget{ 0, 1 }, true) ==
				doctest::Approx((1.0f - 3.0f / 15.0f) * kJeepSpeed * 4.0f));
		CHECK(ai::change_unit_urgency(bot, just_left, ai::ChangeTarget{ 0, 0 }, true) ==
				doctest::Approx(kJeepSpeed * 5.0f * 3.0f / 15.0f));
	}

	TEST_CASE("a manned jeep offers its free seats by its driver's order, against staying on foot") {
		std::vector<ai::ChangeVehicle> vehicles{ jeep(20.0f) };
		vehicles[0].seat[0].occupant = 9;
		vehicles[0].seat[0].occupant_bot = true;
		const ai::ChangeBot bot = fresh_bot();
		CHECK(ai::change_unit_urgency(bot, vehicles, ai::ChangeTarget{ 0, 0 }, true) == 0.0f);
		CHECK(ai::change_unit_urgency(bot, vehicles, ai::ChangeTarget{ 0, 1 }, true) ==
				doctest::Approx(kJeepSpeed * 4.0f));
		const ai::ChangeChoice c = ai::choose_change(bot, vehicles);
		REQUIRE(c.change);
		CHECK(c.target == ai::ChangeTarget{ 0, 1 });
		vehicles[0].seat[0].same_order = false;
		CHECK(ai::change_unit_urgency(bot, vehicles, ai::ChangeTarget{ 0, 2 }, true) ==
				doctest::Approx(kJeepSpeed * 2.5f));
		CHECK(kJeepSpeed * 2.5f < 2.0f * kSoldierSpeed * 5.0f);
		CHECK_FALSE(ai::choose_change(bot, vehicles).change);
	}

	TEST_CASE("enemy-manned, flipped, off-mesh, far, wrecked and booked vehicles are refused") {
		const ai::ChangeBot bot = fresh_bot();
		std::vector<ai::ChangeVehicle> enemy{ jeep(20.0f, 2) };
		enemy[0].seat[3].occupant = 30;
		CHECK_FALSE(ai::choose_change(bot, enemy).change);
		enemy[0].seat[3].occupant = -1;
		CHECK(ai::choose_change(bot, enemy).change);

		std::vector<ai::ChangeVehicle> flipped{ jeep(20.0f) };
		flipped[0].up = 0.0f;
		CHECK_FALSE(ai::choose_change(bot, flipped).change);

		std::vector<ai::ChangeVehicle> off_mesh{ jeep(20.0f) };
		off_mesh[0].on_nav = false;
		CHECK_FALSE(ai::choose_change(bot, off_mesh).change);
		off_mesh[0].on_nav = true;
		off_mesh[0].nav_gap = 3.5f;
		CHECK_FALSE(ai::choose_change(bot, off_mesh).change);
		off_mesh[0].nav_gap = 3.3f;
		CHECK(ai::choose_change(bot, off_mesh).change);

		CHECK_FALSE(ai::choose_change(bot, std::vector<ai::ChangeVehicle>{ jeep(76.0f) }).change);
		CHECK(ai::choose_change(bot, std::vector<ai::ChangeVehicle>{ jeep(74.0f) }).change);

		std::vector<ai::ChangeVehicle> wrecked{ jeep(20.0f) };
		wrecked[0].health = 0.29f;
		CHECK_FALSE(ai::choose_change(bot, wrecked).change);
		wrecked[0].altitude = 9.0f;
		wrecked[0].health = 1.0f;
		CHECK_FALSE(ai::choose_change(bot, wrecked).change);

		std::vector<ai::ChangeVehicle> booked{ jeep(20.0f), jeep(40.0f) };
		booked[0].seat[0].booked_by = 7;
		const ai::ChangeChoice c = ai::choose_change(bot, booked);
		REQUIRE(c.change);
		CHECK(c.target == ai::ChangeTarget{ 1, 0 });
		booked[0].seat[0].booked_by = bot.player_id;
		CHECK(ai::choose_change(bot, booked).target == ai::ChangeTarget{ 0, 0 });
	}

	TEST_CASE("a seated bot gets out of a flipped jeep or when stuck, and keeps a good seat") {
		ai::ChangeBot bot = fresh_bot();
		bot.now_s = 200.0;
		bot.vehicle = 0;
		bot.seat = 0;
		std::vector<ai::ChangeVehicle> vehicles{ jeep(0.0f) };
		vehicles[0].seat[0].occupant = bot.player_id;
		CHECK_FALSE(ai::choose_change(bot, vehicles).change);
		bot.stuck = true;
		const ai::ChangeChoice stuck = ai::choose_change(bot, vehicles);
		CHECK(stuck.get_out);
		CHECK(stuck.target.soldier());
		CHECK(stuck.best == ai::kGetOutUrgency);
		CHECK(stuck.urgency == doctest::Approx(4.0f * 5.0f));
		bot.stuck = false;
		vehicles[0].up = -1.0f;
		const ai::ChangeChoice flipped = ai::choose_change(bot, vehicles);
		CHECK(flipped.get_out);
		const float out = kSoldierSpeed * 5.0f * 0.2f;
		CHECK(flipped.best == doctest::Approx(out));
		CHECK(flipped.urgency ==
				doctest::Approx(
						ai::DecliningSlopeCurve::calculate(out * 0.5f / (2.0f * kJeepSpeed * 5.0f)) * 4.0f * 5.0f));
	}

	TEST_CASE("a damaged vehicle's passenger bails below the bail value and its driver below 0.3 of it") {
		ai::ChangeBot bot = fresh_bot();
		bot.now_s = 200.0;
		bot.vehicle = 0;
		bot.seat = 1;
		std::vector<ai::ChangeVehicle> vehicles{ jeep(0.0f) };
		vehicles[0].seat[0].occupant = 9;
		vehicles[0].seat[0].occupant_bot = true;
		vehicles[0].seat[1].occupant = bot.player_id;
		vehicles[0].health = 0.5f;
		CHECK_FALSE(ai::choose_change(bot, vehicles).change);
		vehicles[0].health = 0.25f;
		const ai::ChangeChoice passenger = ai::choose_change(bot, vehicles);
		CHECK(passenger.get_out);
		CHECK(passenger.current == 0.0f);
		bot.seat = 0;
		vehicles[0].seat[0].occupant = bot.player_id;
		vehicles[0].seat[1].occupant = -1;
		CHECK_FALSE(ai::choose_change(bot, vehicles).change);
		vehicles[0].health = 0.08f;
		CHECK(ai::choose_change(bot, vehicles).get_out);
	}

	TEST_CASE("the change plan approaches, enters within reach, switches seats and gets out") {
		std::vector<ai::ChangeVehicle> vehicles{ jeep(20.0f) };
		ai::ChangeBot bot = fresh_bot();
		ai::ChangeOrder o = ai::change_plan_step(bot, vehicles, ai::ChangeTarget{ 0, 0 });
		CHECK(o.step == ai::ChangeStep::Approach);
		CHECK(o.goal.x == doctest::Approx(20.0f));
		bot.position = { 15.8f, 0.0f, 0.0f };
		CHECK(ai::change_plan_step(bot, vehicles, ai::ChangeTarget{ 0, 0 }).step == ai::ChangeStep::Enter);
		vehicles[0].seat[0].occupant = 11;
		CHECK(ai::change_plan_step(bot, vehicles, ai::ChangeTarget{ 0, 0 }).step == ai::ChangeStep::None);
		vehicles[0].seat[0].occupant = bot.player_id;
		bot.vehicle = 0;
		bot.seat = 0;
		o = ai::change_plan_step(bot, vehicles, ai::ChangeTarget{ 0, 1 });
		CHECK(o.step == ai::ChangeStep::SelectSeat);
		CHECK(o.seat == 1);
		CHECK(ai::change_plan_step(bot, vehicles, ai::ChangeTarget{}).step == ai::ChangeStep::GetOut);
		CHECK(ai::change_plan_step(bot, vehicles, ai::ChangeTarget{ 0, 0 }).step == ai::ChangeStep::None);
	}

	TEST_CASE("a config declares Change as behaviour 10 created by BBChange, and its modifier is the Change weight") {
		ai::AiConfig with;
		REQUIRE(ai::run_ai_script(change_script("Change", "BBChange"), "change.ai", with).ok());
		ai::BehaviourTuning tuning;
		REQUIRE(ai::apply_behaviour_weights(with, tuning).ok());
		CHECK(tuning.change);
		CHECK(tuning.curves[size_t(ai::InfantryBehaviour::Change)].weight == doctest::Approx(1.5f));
		ai::AiConfig misnamed;
		REQUIRE(ai::run_ai_script(change_script("Fire", "BBChange"), "fire.ai", misnamed).ok());
		ai::BehaviourTuning untouched;
		CHECK_FALSE(ai::apply_behaviour_weights(misnamed, untouched).ok());
		CHECK_FALSE(untouched.change);
		ai::AiConfig miscreated;
		REQUIRE(ai::run_ai_script(change_script("Change", "BBMoveTo"), "moveto.ai", miscreated).ok());
		CHECK_FALSE(ai::apply_behaviour_weights(miscreated, untouched).ok());
	}

	TEST_CASE("BotMain merges BBChange only when the tuning declares Change; it wins on its weighted urgency") {
		ai::test::FakeBody body;
		ai::BehaviourTuning tuning = unit_tuning();
		body.change = { 50.0f, 9 };
		{
			ai::BotMain ten{ 0, 3, 1, body, tuning };
			CHECK(ten.behaviour_count() == ai::kInfantryBehaviourCount);
			ten.update_bot(true, true);
			decide(ten);
			CHECK(body.change_senses == 0);
			CHECK(ten.behaviour() != ai::InfantryBehaviour::Change);
		}
		tuning.change = true;
		tuning.curves[size_t(ai::InfantryBehaviour::Change)].weight = 0.5f;
		ai::BotMain bot{ 0, 3, 1, body, tuning };
		CHECK(bot.behaviour_count() == ai::kBotBehaviourCount);
		bot.update_bot(true, true);
		decide(bot);
		CHECK(body.change_senses == 1);
		CHECK(bot.behaviour() == ai::InfantryBehaviour::Change);
		CHECK(bot.weighted_urgency(ai::InfantryBehaviour::Change) == doctest::Approx(25.0f));
		CHECK(bot.chosen_urgency() == doctest::Approx(25.0f));
		CHECK(bot.activity() == ai::InfantryActivity::Roam);
		CHECK(body.calls ==
				std::vector<std::string>{ "move_to_object " + std::to_string(int(ai::ObjectGoal::Equipment)) });
		body.change = {};
		decide(bot);
		CHECK(body.change_senses == 2);
		CHECK(bot.behaviour() != ai::InfantryBehaviour::Change);
		CHECK(bot.weighted_urgency(ai::InfantryBehaviour::Change) == 0.0f);
	}
}
