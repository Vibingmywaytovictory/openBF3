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

	TEST_CASE("a bot whose squad member is close to a non-owned target looks for no vehicle, as BBChange skips the "
			  "hardware scan on SquadMember::isCloseToNonOwnedTarget; a seated one may still get out") {
		std::vector<ai::ChangeVehicle> vehicles{ jeep(20.0f) };
		ai::ChangeBot bot = fresh_bot();
		REQUIRE(ai::choose_change(bot, vehicles).change);
		bot.close_to_non_owned_target = true;
		const ai::ChangeChoice near = ai::choose_change(bot, vehicles);
		CHECK_FALSE(near.change);
		CHECK(near.best == doctest::Approx(2.0f * kSoldierSpeed * 5.0f));
		bot.now_s = 200.0;
		bot.vehicle = 0;
		bot.seat = 0;
		bot.position = vehicles[0].position;
		vehicles[0].seat[0].occupant = bot.player_id;
		bot.stuck = true;
		CHECK(ai::choose_change(bot, vehicles).get_out);
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

	TEST_CASE("a soldier's fire value joins his move value: 6 x his best strength with no enemy counts, 3 x count "
			  "x strength less the enemy's strength against infantry with them, and a jeep is measured against it") {
		ai::ChangeBot bot = fresh_bot();
		bot.strengths = { 78.0f, 7.0f, 2.0f, 0.0f, 0.0f, 0.0f };
		const std::vector<ai::ChangeVehicle> none;
		const float raw = 6.0f * 78.0f;
		CHECK(ai::change_unit_urgency(bot, none, ai::ChangeTarget{}, false) ==
				doctest::Approx(kSoldierSpeed * 5.0f + raw));
		const std::vector<ai::ChangeVehicle> vehicles{ jeep(20.0f) };
		const ai::ChangeChoice stay = ai::choose_change(bot, vehicles);
		CHECK(stay.current == doctest::Approx(2.0f * (kSoldierSpeed * 5.0f + raw)));
		CHECK(stay.best == doctest::Approx(stay.current));
		CHECK_FALSE(stay.change);
		CHECK(stay.urgency == 0.0f);
		ai::ChangeBot weak = fresh_bot();
		weak.strengths = { 2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		const float own = 2.0f * (kSoldierSpeed * 5.0f + 6.0f * 2.0f);
		const ai::ChangeChoice take = ai::choose_change(weak, vehicles);
		REQUIRE(take.change);
		CHECK(take.target == ai::ChangeTarget{ 0, 0 });
		CHECK(take.current == doctest::Approx(own));
		CHECK(take.best == doctest::Approx(kJeepSpeed * 5.0f));
		CHECK(take.urgency ==
				doctest::Approx(ai::DecliningSlopeCurve::calculate(kJeepSpeed * 5.0f * 0.5f / own) * 4.0f));
		bot.enemy_types = { 4.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		bot.enemy_strengths = { 50.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		CHECK(ai::change_unit_urgency(bot, none, ai::ChangeTarget{}, false) ==
				doctest::Approx(kSoldierSpeed * 5.0f + 78.0f * 3.0f * 4.0f - 50.0f));
	}

	TEST_CASE("a sided sensed enemy the soldier is no weaker against is worth 4 x (s^2 - its strength against him); an "
			  "unsided one is not, and an unarmed jeep stays at its move value") {
		ai::ChangeBot bot = fresh_bot();
		bot.strengths = { 78.0f, 7.0f, 2.0f, 0.0f, 0.0f, 0.0f };
		std::vector<ai::ChangeSensed> sensed(1);
		sensed[0].strengths = { 99.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		bot.sensed = sensed;
		const std::vector<ai::ChangeVehicle> vehicles{ jeep(20.0f) };
		CHECK(ai::change_unit_urgency(bot, vehicles, ai::ChangeTarget{}, false) ==
				doctest::Approx(kSoldierSpeed * 5.0f + 6.0f * 78.0f));
		sensed[0].sided = true;
		CHECK(ai::change_unit_urgency(bot, vehicles, ai::ChangeTarget{}, false) ==
				doctest::Approx(kSoldierSpeed * 5.0f + 4.0f * (78.0f * 78.0f - 99.0f)));
		CHECK(ai::change_unit_urgency(bot, vehicles, ai::ChangeTarget{ 0, 0 }, true) ==
				doctest::Approx(kJeepSpeed * 5.0f));
		sensed[0].strength_type = ai::kChangeLightArmourStrength;
		sensed[0].strengths = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		CHECK(ai::change_unit_urgency(bot, vehicles, ai::ChangeTarget{}, false) ==
				doctest::Approx(kSoldierSpeed * 5.0f + 4.0f * 7.0f * 7.0f));
	}

	TEST_CASE("an occupied gunner seat lends 0.9 of its strengths to an unarmed driver seat and 0.4 to an armed one; a "
			  "gunner seat with no driven hull, nothing sensed and no known enemy in reach takes its raw value") {
		const ai::ChangeBot bot = fresh_bot();
		std::vector<ai::ChangeVehicle> vehicles{ jeep(20.0f) };
		vehicles[0].seat[1].occupant = 9;
		vehicles[0].seat[1].armed = true;
		vehicles[0].seat[1].strengths = { 100.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		CHECK(ai::change_unit_urgency(bot, vehicles, ai::ChangeTarget{ 0, 0 }, true) ==
				doctest::Approx(kJeepSpeed * 5.0f + 0.9f * 100.0f * 6.0f));
		vehicles[0].seat[0].armed = true;
		CHECK(ai::change_unit_urgency(bot, vehicles, ai::ChangeTarget{ 0, 0 }, true) ==
				doctest::Approx(kJeepSpeed * 5.0f + 0.4f * 100.0f * 6.0f));
		vehicles[0].seat[1].occupant = -1;
		CHECK(ai::change_unit_urgency(bot, vehicles, ai::ChangeTarget{ 0, 1 }, true) ==
				doctest::Approx(0.4f * 0.0f + 6.0f * 100.0f));
		vehicles[0].seat[1].armed = false;
		CHECK(ai::change_unit_urgency(bot, vehicles, ai::ChangeTarget{ 0, 1 }, true) == 0.0f);
	}

	TEST_CASE("an undriven armed seat with nothing sensed scores the side's known enemies within its reach of the bot "
			  "and its range of the hull at s^2 less their strength against the hull, and a camera turned away from "
			  "every sensed enemy leaves it nothing") {
		ai::ChangeBot bot = fresh_bot();
		std::vector<ai::ChangeVehicle> vehicles{ jeep(20.0f) };
		ai::ChangeSeat &gunner = vehicles[0].seat[1];
		gunner.armed = true;
		gunner.max_range = 300.0f;
		gunner.min_range = 0.0f;
		gunner.strengths = { 100.0f, 10.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		std::vector<ai::ChangeSensed> known(2);
		known[0].position = { 250.0f, 1.0f, 0.0f };
		known[0].strengths = { 0.0f, 30.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		known[1].position = { 900.0f, 1.0f, 0.0f };
		known[1].strengths = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		bot.known = known;
		const float value = 100.0f * 100.0f - 30.0f;
		CHECK(ai::change_unit_urgency(bot, vehicles, ai::ChangeTarget{ 0, 1 }, true) == doctest::Approx(value));
		known[0].position = { 310.0f, 1.0f, 0.0f };
		CHECK(ai::change_unit_urgency(bot, vehicles, ai::ChangeTarget{ 0, 1 }, true) == doctest::Approx(6.0f * 100.0f));
		std::vector<ai::ChangeSensed> sensed(1);
		sensed[0].sided = true;
		sensed[0].position = { -100.0f, 1.0f, 0.0f };
		bot.sensed = sensed;
		gunner.yaw_min_deg = -30.0f;
		gunner.yaw_max_deg = 30.0f;
		CHECK(ai::change_unit_urgency(bot, vehicles, ai::ChangeTarget{ 0, 1 }, true) == 0.0f);
		sensed[0].position = { 20.0f, 1.0f, 100.0f };
		CHECK(ai::change_unit_urgency(bot, vehicles, ai::ChangeTarget{ 0, 1 }, true) ==
				doctest::Approx(4.0f * 100.0f * 100.0f));
	}

	TEST_CASE(
			"a seat's camera takes a direction within its yaw limits about its aim and its pitch limits off the hull") {
		ai::ChangeVehicle v = jeep(0.0f);
		ai::ChangeSeat &seat = v.seat[1];
		seat.aim_yaw_deg = 180.0f;
		seat.yaw_min_deg = -45.0f;
		seat.yaw_max_deg = 45.0f;
		seat.pitch_min_deg = -10.0f;
		seat.pitch_max_deg = 20.0f;
		CHECK(ai::change_camera_valid(v, 1, { 0.0f, 0.0f, -1.0f }));
		CHECK_FALSE(ai::change_camera_valid(v, 1, { 0.0f, 0.0f, 1.0f }));
		CHECK_FALSE(ai::change_camera_valid(v, 1, { 1.0f, 0.0f, 0.0f }));
		CHECK(ai::change_camera_valid(v, 1, { 0.0f, 0.3f, -0.95393920f }));
		CHECK_FALSE(ai::change_camera_valid(v, 1, { 0.0f, -0.5f, -0.8660254f }));
		CHECK(ai::change_camera_valid(v, 2, { 1.0f, 0.0f, 0.0f }));
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

	TEST_CASE("aiSettings.setVehicleMods gives a vehicle class its weight set, which multiplies Infantry's basic "
			  "weights; a class's own setVehicleBehaviour leaves Infantry's binding; unknown or incomplete fails") {
		const std::string base = change_script("Change", "BBChange") +
				"aiSettings.setVehicle Car\n"
				"aiSettings.createBehaviourModifiers Halves\n";
		std::string halves = base;
		for (size_t i = 0; i < ai::kBotBehaviourCount; ++i) {
			const std::string name = i < ai::kInfantryBehaviourCount
					? std::string(ai::infantry_behaviour_kind(ai::InfantryBehaviour(i)).substr(2))
					: std::string("Change");
			halves += "aiSettings.setBehaviourModifier " + name + " 0.5\n";
		}
		ai::AiConfig mods;
		REQUIRE(ai::run_ai_script(halves +
						"aiSettings.setVehicleMods Infantry Halves\n"
						"aiSettings.setVehicleBehaviour Car Change BBChange BBPChange 0 Memory Default\n"
						"aiSettings.setVehicleBehaviour Car Idle BBMoveTo BBPIdle 0 Memory Default\n",
				"mods.ai", mods)
						.ok());
		REQUIRE(mods.settings.mods_of(mods.settings.vehicle("Infantry")) != nullptr);
		CHECK(mods.settings.mods_of(mods.settings.vehicle("Car")) == nullptr);
		CHECK(mods.vehicle_behaviours.size() == ai::kBotBehaviourCount + 2u);
		ai::BehaviourTuning tuning;
		REQUIRE(ai::apply_behaviour_weights(mods, tuning).ok());
		CHECK(tuning.curves[size_t(ai::InfantryBehaviour::Revive)].weight == doctest::Approx(0.5f));
		CHECK(tuning.curves[size_t(ai::InfantryBehaviour::Change)].weight == doctest::Approx(0.75f));
		ai::AiConfig unknown;
		CHECK_FALSE(ai::run_ai_script(halves + "aiSettings.setVehicleMods Tank Halves\n", "tank.ai", unknown).ok());
		CHECK_FALSE(ai::run_ai_script(base + "aiSettings.setVehicleMods Car Halves\n", "partial.ai", unknown).ok());
		CHECK_FALSE(ai::run_ai_script(halves + "aiSettings.setVehicleMods Car None\n", "none.ai", unknown).ok());
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
			CHECK(ten.weighted_urgency(ai::InfantryBehaviour::Change) == 0.0f);
			CHECK(ten.behaviour_of(ai::InfantryBehaviour::Change) == nullptr);
		}
		tuning.change = true;
		tuning.curves[size_t(ai::InfantryBehaviour::Change)].weight = 0.5f;
		ai::BotMain bot{ 0, 3, 1, body, tuning };
		CHECK(bot.behaviour_count() == ai::kBotBehaviourCount);
		CHECK(bot.behaviour_of(ai::InfantryBehaviour::Change) != nullptr);
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

	TEST_CASE("change_unit_urgency adds four times the unit's basic strength to a value above 0.01, as "
			  "calculateVehicleUrgency adds 4 x the information's strength") {
		ai::ChangeBot bot;
		bot.player_id = 1;
		bot.side = 1;
		bot.max_speed = 6.5f;
		bot.now_s = 100.0;
		ai::ChangeVehicle jeep;
		jeep.side = 1;
		jeep.max_speed = 20.0f;
		jeep.seats = 1;
		std::vector<ai::ChangeVehicle> vehicles{ jeep };
		const float foot = ai::change_unit_urgency(bot, vehicles, ai::ChangeTarget{}, false);
		const float seat = ai::change_unit_urgency(bot, vehicles, ai::ChangeTarget{ 0, 0 }, true);
		CHECK(foot == doctest::Approx(6.5f * 5.0f));
		CHECK(seat == doctest::Approx(20.0f * 5.0f));
		bot.basic = ai::kSoldierBasicStrength;
		vehicles[0].seat[0].basic = 5.0f;
		CHECK(ai::change_unit_urgency(bot, vehicles, ai::ChangeTarget{}, false) == doctest::Approx(foot + 4.0f));
		CHECK(ai::change_unit_urgency(bot, vehicles, ai::ChangeTarget{ 0, 0 }, true) == doctest::Approx(seat + 20.0f));
		bot.max_speed = 0.0f;
		CHECK(ai::change_unit_urgency(bot, vehicles, ai::ChangeTarget{}, false) == 0.0f);
	}
}
