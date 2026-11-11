#include <doctest/doctest.h>

#include "ai_knowledge_fakes.h"
#include "bf3/ai/actionplan/conditions.h"
#include "bf3/ai/actionplan/flow_control.h"
#include "bf3/ai/behaviour/curves.h"
#include "bf3/ai/behaviour/urgency_merger.h"
#include "bf3/ai/debug/ai_console.h"
#include "bf3/ai/interpreter/bap_interpreter.h"
#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/information_grid.h"
#include "bf3/ai/knowledge/plug_ins.h"
#include "bf3/ai/knowledge/weapon_info.h"
#include "bf3/ai/main/ai_main.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace bf3;
using namespace bf3::ai;
using namespace bf3::ai::test;

TEST_SUITE("ai-port") {
	TEST_CASE("AIRandom: the same seed gives the same sequence across a refill; the seed mixes match seed and slot") {
		AIRandom a(AIRandom::seed_for(0x5EEDull, 3));
		AIRandom b(AIRandom::seed_for(0x5EEDull, 3));
		AIRandom c(AIRandom::seed_for(0x5EEDull, 4));
		a.init();
		b.init();
		c.init();
		CHECK(a.seed() != 0);
		CHECK(a.seed() != c.seed());
		size_t same_as_c = 0;
		for (int i = 0; i < 2 * AIRandom::kStateSize + 5; ++i) {
			const uint32_t x = a.next();
			CHECK(x == b.next());
			same_as_c += x == c.next() ? 1 : 0;
		}
		CHECK(same_as_c < 4);
		for (int i = 0; i < 1000; ++i) {
			const float f = a.next_float();
			CHECK(f >= 0.0f);
			CHECK(f < 1.0f);
		}
		CHECK(AIRandom::seed_for(1, 0) == AIRandom::seed_for(1, 0));
	}

	TEST_CASE("urge curves: UCConstant, UCLinear, UCXInverse and the S and J tables at sampled inputs") {
		CHECK(UCConstant(-2.0f).calculate(5.0f) == -2.0f);
		const UCLinear linear(2.0f, -1.0f);
		CHECK(linear.calculate(0.0f) == 0.0f);
		CHECK(linear.calculate(0.25f) == 0.0f);
		CHECK(linear.calculate(1.0f) == doctest::Approx(1.0f));
		CHECK(linear.calculate(0.75f) == doctest::Approx(0.5f));
		const UCXInverse inverse(1.0f, 1.0f, 1.0f, 0.0f);
		CHECK(inverse.calculate(1.0f) == doctest::Approx(0.5f));
		CHECK(inverse.calculate(3.0f) == doctest::Approx(0.25f));
		CHECK(UCXInverse(-1.0f, 1.0f, 1.0f, 0.0f).calculate(1.0f) == 0.0f);
		CHECK(UCXInverse(1.0f, 2.0f, 0.0f, 0.5f).calculate(7.0f) == doctest::Approx(1.0f));
		CHECK(SCurve::calculate(0.5f) == doctest::Approx(0.5f));
		CHECK(SCurve::calculate(0.37f) == doctest::Approx(0.231592f).epsilon(1e-5));
		CHECK(SCurve::calculate(0.005f) == doctest::Approx(0.5f * 0.00224938989f).epsilon(1e-4));
		CHECK(SCurve::calculate(0.99f) == doctest::Approx(0.997750998f).epsilon(1e-6));
		CHECK(SCurve::calculate(1.0f) == 1.0f);
		CHECK(SCurve::calculate(-1.0f) == 0.0f);
		CHECK(SCurve::calculate(std::nanf("")) == 0.0f);
		CHECK(SCurve::calculate(0.2f) < SCurve::calculate(0.3f));
		CHECK(JCurve::calculate(0.5f) == doctest::Approx(0.015625f));
		CHECK(JCurve::calculate(0.1f) == doctest::Approx(1e-6f).epsilon(1e-3));
		CHECK(JCurve::calculate(1.0f) == 1.0f);
		CHECK(JCurve::calculate(2.0f) == doctest::Approx(1.02f));
		CHECK(JCurve::calculate(-0.5f) == 0.0f);
		CHECK(JCurve::calculate(std::nanf("")) == 0.0f);
		CHECK(DecliningSlopeCurve::calculate(0.0f) == 0.0f);
		CHECK(DecliningSlopeCurve::calculate(0.19f) == doctest::Approx(0.551246345f).epsilon(1e-6));
		CHECK(DecliningSlopeCurve::calculate(0.185f) ==
				doctest::Approx(0.5f * 0.526357889f + 0.5f * 0.551246345f).epsilon(1e-4));
		CHECK(DecliningSlopeCurve::calculate(0.5f) == doctest::Approx(0.894736886f).epsilon(1e-6));
		CHECK(DecliningSlopeCurve::calculate(0.99f) == doctest::Approx(0.998936832f).epsilon(1e-6));
		CHECK(DecliningSlopeCurve::calculate(1.0f) == 1.0f);
		CHECK(DecliningSlopeCurve::calculate(3.0f) == 1.0f);
		CHECK(DecliningSlopeCurve::calculate(-0.5f) == 0.0f);
		CHECK(DecliningSlopeCurve::calculate(std::nanf("")) == 0.0f);
		CHECK(DecliningSlopeCurve::calculate(0.05f) > SCurve::calculate(0.05f));
	}

	TEST_CASE("information: real records per side, their plug-ins, weapon infos, and expiry") {
		Knowledge k;
		FakeObject soldier;
		soldier.where = { 3.0f, 1.0f, -4.0f };
		soldier.transform.forward = { 0.0f, 0.0f, -1.0f };
		const uint32_t side1 = k.soldier(soldier, 1);
		const uint32_t side2 = k.soldier(soldier, 2);
		REQUIRE(side1 != kNoHandle);
		REQUIRE(side2 != kNoHandle);
		CHECK(side1 != side2);
		Information *const info = k.information.information(side1);
		REQUIRE(info != nullptr);
		CHECK(info->information_sort() == 0);
		CHECK(info->time() == 600 - 60);
		CHECK(info->time_stamp() == 600);
		CHECK(info->has_contact());
		CHECK(info->security() == 1.0f);
		CHECK(info->strength() == 0.5f);
		CHECK(info->position().x == 3.0f);
		CHECK(info->facing().z == -1.0f);
		CHECK(info->side() == 1);
		CHECK(info->handle() == side1);
		CHECK(k.information.information(side2)->handle() == side2);
		CHECK(info->soldier_pose() == SoldierPose::Crouching);
		CHECK(info->object_id() == 7);

		REQUIRE(info->physical() != nullptr);
		REQUIRE(info->unit() != nullptr);
		REQUIRE(info->mobile() != nullptr);
		REQUIRE(info->armament() != nullptr);
		CHECK(info->physical()->type() == PlugInType::Physical);
		CHECK(info->unit()->type() == PlugInType::Unit);
		CHECK(info->physical()->health() == 1.0f);
		CHECK(info->mobile()->speed_magnitude() == doctest::Approx(6.0f));
		Vec3 ahead{};
		info->mobile()->position_estimate(600 + 30, ahead);
		CHECK(ahead.x == doctest::Approx(3.0f + 6.0f * 0.5f));

		const std::vector<uint32_t> &weapon_handles = info->armament()->weapons();
		REQUIRE(weapon_handles.size() == 2);
		CHECK(k.weapons.live_count() == 4);
		const WeaponInfo *const rifle = k.weapons.information(weapon_handles[0]);
		REQUIRE(rifle != nullptr);
		CHECK(rifle->ammo() == 30);
		CHECK(rifle->name() == "M16A4");
		CHECK(rifle->security() == 1.0f);
		CHECK(info->unit()->ammo() == doctest::Approx(0.5f));
		const float mod = SCurve::calculate(1.0f) * 0.25f + SCurve::calculate(0.5f) * 0.5f + 0.25f;
		CHECK(info->unit()->strength_mod() == doctest::Approx(mod));
		CHECK(info->unit()->battle_strengths()[0] == doctest::Approx(10.0f * mod));
		CHECK(info->unit()->strategic_strengths()[1] == doctest::Approx(8.0f * mod));
		CHECK(info->unit()->is_team_locked(2));
		WeaponInfo *const pistol = k.weapons.information(weapon_handles[1]);
		info->armament()->rearm(*pistol, 0.4f, 1);
		CHECK(pistol->rearm_amount() == doctest::Approx(0.4f));
		info->armament()->rearm(*pistol, 0.6f, 1);
		CHECK(pistol->rearm_amount() == 0.0f);

		soldier.body.hp = 0.0f;
		soldier.rifle.rounds = 0;
		soldier.pistol.rounds = 0;
		k.timer.tick = 660;
		CHECK(info->set_time(660));
		CHECK(info->unit()->ammo() == 0.0f);
		CHECK(info->unit()->strength_mod() == doctest::Approx(0.25f));

		AIInformationGrid grid(k.information, 1, 4, 4, { -50.0f, -50.0f }, { 50.0f, 50.0f });
		grid.add_info(side1);
		grid.add_info(side2);
		CHECK(grid.size() == 2);
		CHECK(k.information.free_information(side1));
		CHECK(k.information.information(side1) == nullptr);
		CHECK(k.information.information(side2) != nullptr);
		CHECK(k.weapons.live_count() == 2);
		std::vector<uint32_t> alive;
		const UnitPredicate units;
		grid.all_information(alive, &units, nullptr);
		CHECK(alive == std::vector<uint32_t>{ side2 });
		CHECK(grid.size() == 1);
		const uint32_t again = k.soldier(soldier, 1, false);
		CHECK(again != side1);
		CHECK(k.information.information(side1) == nullptr);
		CHECK(k.information.information(again) != nullptr);
	}

	TEST_CASE("AIInformationGrid: circle, rectangle and frustum queries over a grid with a negative corner") {
		Knowledge k;
		FakeObject a, b, c, crate;
		a.where = { -50.0f, 0.0f, -50.0f };
		b.where = { 10.0f, 0.0f, 10.0f };
		c.where = { 60.0f, 0.0f, 60.0f };
		crate.where = { 11.0f, 0.0f, 11.0f };
		crate.with_unit = false;
		const uint32_t ha = k.soldier(a, 1);
		const uint32_t hb = k.soldier(b, 1);
		const uint32_t hc = k.soldier(c, 1);
		const uint32_t hcrate = k.soldier(crate, 1);
		AIInformationGrid grid(k.information, 1, 8, 8, { -100.0f, -100.0f }, { 100.0f, 100.0f });
		for (const uint32_t h : { ha, hb, hc, hcrate }) {
			grid.add_info(h);
		}
		const UnitPredicate units;
		std::vector<uint32_t> out;
		grid.information_within_circle(out, { 12.0f, 12.0f }, 5.0f, &units);
		CHECK(out == std::vector<uint32_t>{ hb });
		out.clear();
		grid.information_within_circle(out, { 12.0f, 12.0f }, 5.0f, nullptr);
		CHECK(out.size() == 2);
		out.clear();
		grid.information_within_rectangle(out, { -60.0f, -60.0f }, { 20.0f, 20.0f }, &units);
		std::sort(out.begin(), out.end());
		std::vector<uint32_t> expected{ ha, hb };
		std::sort(expected.begin(), expected.end());
		CHECK(out == expected);
		b.where = { 70.0f, 0.0f, 70.0f };
		CHECK(grid.update_position(hb));
		CHECK_FALSE(grid.update_position(hb));
		out.clear();
		grid.information_within_circle(out, { 12.0f, 12.0f }, 5.0f, &units);
		CHECK(out.empty());
		out.clear();
		grid.information_within_circle(out, { 70.0f, 70.0f }, 3.0f, &units);
		CHECK(out == std::vector<uint32_t>{ hb });
		GridFrustum box;
		const float lo = 55.0f, hi = 65.0f;
		for (size_t i = 0; i < 8; ++i) {
			box.corners[i] = { (i & 1) ? hi : lo, (i & 2) ? 10.0f : -10.0f, (i & 4) ? hi : lo };
		}
		box.planes = { GridPlane{ { 1, 0, 0 }, -hi }, GridPlane{ { -1, 0, 0 }, lo }, GridPlane{ { 0, 0, 1 }, -hi },
			GridPlane{ { 0, 0, -1 }, lo }, GridPlane{ { 0, 1, 0 }, -10.0f }, GridPlane{ { 0, -1, 0 }, -10.0f } };
		out.clear();
		grid.information_within_frustum(out, box, &units);
		CHECK(out == std::vector<uint32_t>{ hc });
		grid.remove_info(hc);
		out.clear();
		grid.information_within_frustum(out, box, &units);
		CHECK(out.empty());
		CHECK(grid.safe_remove_info(hc, nullptr));
	}

	TEST_CASE("AIInformationGrid: a query corner far outside the grid and a zero-extent grid map to edge cells") {
		Knowledge k;
		FakeObject near_corner, origin;
		near_corner.where = { 95.0f, 0.0f, 95.0f };
		origin.where = { 0.0f, 0.0f, 0.0f };
		const uint32_t hn = k.soldier(near_corner, 1);
		const uint32_t ho = k.soldier(origin, 1);
		AIInformationGrid grid(k.information, 1, 8, 8, { -100.0f, -100.0f }, { 100.0f, 100.0f });
		grid.add_info(hn);
		grid.add_info(ho);
		const UnitPredicate units;
		std::vector<uint32_t> out;
		grid.information_within_rectangle(out, { 90.0f, 90.0f }, { 1.0e12f, 1.0e12f }, &units);
		CHECK(out == std::vector<uint32_t>{ hn });
		out.clear();
		grid.information_within_rectangle(out, { -1.0e12f, -1.0e12f }, { 1.0f, 1.0f }, &units);
		CHECK(out == std::vector<uint32_t>{ ho });
		AIInformationGrid flat(k.information, 1, 8, 8, { 5.0f, 5.0f }, { 5.0f, 5.0f });
		flat.add_info(hn);
		flat.add_info(ho);
		out.clear();
		flat.information_within_circle(out, { 0.0f, 0.0f }, 2.0f, &units);
		CHECK(out == std::vector<uint32_t>{ ho });
		out.clear();
		flat.information_within_rectangle(out, { 90.0f, 90.0f }, { 99.0f, 99.0f }, &units);
		CHECK(out == std::vector<uint32_t>{ hn });
	}

	TEST_CASE(
			"plan interpreter: a BAPContext owns its variables; the two recovered entries run; the rest have no body") {
		static int destroyed = 0;
		struct Counted final : BotScriptVariable {
			~Counted() override { ++destroyed; }
		};
		destroyed = 0;
		BAPContext context;
		context.add_variable(std::make_unique<Counted>());
		context.add_variable(std::make_unique<Counted>());
		CHECK(context.size() == 2);
		context.clear();
		CHECK(destroyed == 2);
		CHECK(context.size() == 0);

		AiSettings settings;
		bool first = false;
		bool second = false;
		struct Nobody final : PlanBot {
			const PlanInterpreter &plan_interpreter() const override { return infantry_interpreter(); }
			bool is_just_spawned() override { return false; }
			BotLodLevel lod_level() const override { return BotLodLevel::Full; }
			double game_time() const override { return 0.0; }
			InfantryBody *infantry() override { return nullptr; }
		} nobody;
		BAPAIdle idle(std::make_unique<BAPConTrue>(), 0, 0);
		const std::unique_ptr<BAPEntry> walk = create_plan_entry("EntryInfanteryMoveToDirection", settings);
		REQUIRE(walk != nullptr);
		CHECK(walk->target_id() == BAPStatementType::MoveToDirection);
		CHECK(walk->execute(idle, nobody, first, second));
		CHECK(create_plan_entry("EntryCarMoveToDirection", settings) == nullptr);
		CHECK(create_plan_entry("EntryInfanteryMoveTo", settings)->target_id() == BAPStatementType::MoveTo);
		CHECK(create_plan_entry("EntryHeliMoveTo", settings) == nullptr);
		CHECK(plan_entry_kinds().size() == 42);
		size_t bodies = 0;
		for (const PlanEntryKind &kind : plan_entry_kinds()) {
			bodies += kind.has_body ? 1 : 0;
		}
		CHECK(bodies == 7);
		CHECK(find_plan_entry_kind("EntryHeliMoveAwayFromObject")->target == BAPStatementType::MoveAwayFromObject);
		CHECK(find_plan_entry_kind("EntryPlaneAimAt")->domain == AiDomain::Air);
		settings.enable_vehicle_ai = true;
		const std::unique_ptr<BAPEntry> drive = create_plan_entry("EntryCarMoveToDirection", settings);
		REQUIRE(drive != nullptr);
		CHECK(drive->execute(idle, nobody, first, second));
	}

	TEST_CASE("behaviour kinds: vehicle and air kinds are registered, off, and never chosen by UrgencyMerger") {
		struct FixedGenerator final : UrgencyGenerator {
			float value = 0.0f;
			int resets = 0;
			float urgency(Bot *) override { return value; }
			float active_urgency(Bot *) override { return value; }
			float update_active_urgency(Bot *) override { return value; }
			bool has_changed_target(Bot *) override { return false; }
			bool quotient_changed(Bot *, float, float) override { return false; }
			bool has_changed(Bot *, float, float) override { return false; }
			UrgencyGenerator *chosen_generator() override { return this; }
			void reset(Bot *) override { ++resets; }
		};
		struct Maker final : BBCreateFunction {
			std::map<std::string, std::unique_ptr<FixedGenerator>> *made = nullptr;
			float value = 0.0f;
			int operator()(int, const std::string &p_name, int, const UrgeCurve *p_curve, const float *) override {
				auto generator = std::make_unique<FixedGenerator>();
				generator->value = p_curve != nullptr ? p_curve->calculate(1.0f) : value;
				(*made)[p_name] = std::move(generator);
				return 1;
			}
		};
		AiSettings settings;
		BotBehaviourManager manager(settings);
		std::map<std::string, std::unique_ptr<FixedGenerator>> made;
		std::vector<std::unique_ptr<Maker>> makers;
		size_t vehicle_or_air = 0;
		for (const BehaviourKind &kind : behaviour_kinds()) {
			CHECK(manager.is_registered(kind.name));
			auto maker = std::make_unique<Maker>();
			maker->made = &made;
			maker->value = kind.domain == AiDomain::Vehicle || kind.domain == AiDomain::Air ? 10.0f : 1.0f;
			CHECK(manager.set_create_function(kind.name, maker.get()));
			makers.push_back(std::move(maker));
			vehicle_or_air += kind.domain == AiDomain::Vehicle || kind.domain == AiDomain::Air ? 1 : 0;
		}
		CHECK(behaviour_kinds().size() == 26);
		CHECK(vehicle_or_air == 10);
		CHECK_FALSE(manager.set_create_function("BBTeleportEverywhere", makers.front().get()));
		CHECK(manager.is_enabled("bbfireinfantery"));
		CHECK_FALSE(manager.is_enabled("BBFireDriver"));
		CHECK_FALSE(manager.is_enabled("BBFire3d"));
		manager.set_behaviour_name(0, "Idle");
		manager.set_behaviour_name(1, "Fire");
		CHECK(manager.behaviour_id("idle") == 0);
		CHECK(manager.behaviour_name(1) == "Fire");
		CHECK(manager.behaviour_name(9) == "");
		CHECK(manager.create_uc_linear("ramp", 0.0f, 3.0f) == "UCLinear created");
		std::string message;
		CHECK(manager.create("Nope", "BBIdle", "x", 0, "", nullptr, message) == 0);
		CHECK(message == "'Nope' doesn't exist");
		for (const BehaviourKind &kind : behaviour_kinds()) {
			const std::string fn(kind.name);
			const int created =
					manager.create("Fire", fn, fn, 0, fn == "BBFireInfantery" ? "ramp" : "", nullptr, message);
			const bool off = kind.domain == AiDomain::Vehicle || kind.domain == AiDomain::Air;
			CHECK_MESSAGE(created == (off ? 0 : 1), fn);
			if (off) {
				CHECK(message.find("is off") != std::string::npos);
			}
		}
		CHECK(made.size() == 16);
		std::vector<UrgencyGenerator *> generators;
		for (const auto &[name, generator] : made) {
			generators.push_back(generator.get());
		}
		UrgencyMerger merger(generators);
		CHECK(merger.size() == 16);
		CHECK(merger.urgency(nullptr) == 0.0f);
		CHECK(merger.update_active_urgency(nullptr) == doctest::Approx(3.0f));
		CHECK(merger.chosen_generator() == made.at("BBFireInfantery").get());
		CHECK(merger.urgency(nullptr) == doctest::Approx(3.0f));
		CHECK(merger.active_urgency(nullptr) == doctest::Approx(3.0f));
		merger.reset(nullptr);
		CHECK(made.at("BBIdle")->resets == 1);

		settings.enable_vehicle_ai = true;
		CHECK(manager.is_enabled("BBFireDriver"));
		CHECK(manager.create("Fire", "BBFireDriver", "BBFireDriver", 0, "", nullptr, message) == 1);
		CHECK_FALSE(manager.is_enabled("BBTakeCover3d"));
		generators.push_back(made.at("BBFireDriver").get());
		UrgencyMerger with_vehicle(generators);
		CHECK(with_vehicle.update_active_urgency(nullptr) == doctest::Approx(10.0f));
		CHECK(with_vehicle.chosen_generator() == made.at("BBFireDriver").get());
	}

	TEST_CASE(
			"AISettings: the shipped bot_tuning.cfg gives the compiled-in defaults; host keys pass, bad ai keys fail") {
		std::ifstream f(OPENBF3_BOT_TUNING_CFG, std::ios::binary);
		REQUIRE(f.good());
		std::stringstream ss;
		ss << f.rdbuf();
		AiSettings parsed;
		parsed.max_bots = 5;
		const core::Status st = parse_ai_settings(ss.str(), parsed);
		REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
		CHECK(format_ai_settings(parsed) == format_ai_settings(AiSettings{}));
		CHECK_FALSE(parsed.enable_vehicle_ai);
		CHECK_FALSE(parsed.enable_air_ai);
		CHECK(ss.str().find("ai.enable_vehicle_ai") != std::string::npos);
		CHECK(ss.str().find("ai.enable_air_ai") != std::string::npos);

		AiSettings one;
		REQUIRE(parse_ai_settings("skill_min = 0.3\nai.enable_air_ai = true # on\nai.max_bots = 12\n", one).ok());
		CHECK(one.enable_air_ai);
		CHECK(one.max_bots == 12);
		CHECK(one.domain_enabled(AiDomain::Air));
		CHECK_FALSE(one.domain_enabled(AiDomain::Vehicle));
		for (const char *bad : { "ai.nope = 1\n", "ai.max_bots = 65\n", "ai.enable_vehicle_ai = 0\n",
					 "ai.bot_skill = nan\n", "ai.sides = 3\n" }) {
			AiSettings c;
			CHECK_MESSAGE(!parse_ai_settings(bad, c).ok(), bad);
		}
		CHECK(check_ai_setting("ai.bot_skill", "0.5").fault == core::KeyFault::None);
		CHECK(check_ai_setting("ai.bot_skill", "2").fault == core::KeyFault::OutOfRange);
		CHECK(check_ai_setting("ai.nope", "1").fault == core::KeyFault::UnknownKey);
		CHECK(AiSettings{}.armour_class_value(3) == 15.0f);
	}
}

namespace {

struct Calls {
	std::vector<std::string> log;
};

struct FakeBot final : Bot {
	Calls *calls = nullptr;
	uint32_t id = 0;
	int team = 1;
	int player = -1;
	uint32_t equipment = kNoHandle;
	int idle = -2;
	int decisions = 0;
	int seen = 0;
	int talks = 0;
	double last_slice = 0.0;
	BotLodLevel lod = BotLodLevel::Full;
	uint32_t bot_id() const override { return id; }
	int side() const override { return team; }
	void set_side(int p_side) override { team = p_side; }
	int player_id() const override { return player; }
	void set_player(int p_player_id) override { player = p_player_id; }
	void set_name(const std::string &) override {}
	void set_personality(const float *) override {}
	void receive_idle_order(int p_idle) override { idle = p_idle; }
	bool is_dead() const override { return false; }
	bool man_down() const override { return false; }
	bool is_deactivated() const override { return false; }
	void set_deactivated(bool) override {}
	BotLodLevel lod_level() const override { return lod; }
	void set_lod_level(BotLodLevel p_level) override { lod = p_level; }
	uint32_t current_equipment() const override { return equipment; }
	bool has_squad_member() const override { return true; }
	int decision_making(double p_slice) override {
		++decisions;
		last_slice = p_slice;
		calls->log.push_back("decide " + std::to_string(id));
		return 1;
	}
	void see(double) override {
		++seen;
		calls->log.push_back("see " + std::to_string(id));
	}
	void update_sensing(double) override {}
	void update_communication() override { ++talks; }
	bool path_search_pending() const override { return false; }
	void update_obstacles() override {}
	bool navigation_active() const override { return false; }
	void update_navigation_occasionally() override {}
	bool search_active() const override { return false; }
	bool search_new() const override { return false; }
	bool search_running() const override { return false; }
	void update_pathfinding(bool &, bool *) override {}
};

struct FakeFactory final : BotFactory {
	Calls *calls = nullptr;
	uint32_t equipment = kNoHandle;
	std::vector<FakeBot *> bots;
	std::unique_ptr<Bot> create_bot(int p_player_id, uint32_t p_bot_id, const float *, int p_side) override {
		auto bot = std::make_unique<FakeBot>();
		bot->calls = calls;
		bot->id = p_bot_id;
		bot->player = p_player_id;
		bot->team = p_side;
		bot->equipment = equipment;
		bots.push_back(bot.get());
		return bot;
	}
};

struct FakeGame final : AiGame {
	Calls *calls = nullptr;
	int next_player = 10;
	bool is_playing() const override { return true; }
	int max_number_of_players() const override { return 4; }
	bool is_valid_player_id(int p_player_id) const override { return p_player_id >= 0 && p_player_id < kMaxPlayerIds; }
	int player_side(int p_player_id) const override { return p_player_id >= 10 && p_player_id < next_player ? 1 : 0; }
	bool player_is_ai(int p_player_id) const override { return p_player_id >= 10; }
	bool add_bot(int &r_player_id, int &r_side, std::string &r_name) override {
		r_player_id = next_player++;
		r_side = 1 + r_player_id % 2;
		r_name = "bot" + std::to_string(r_player_id);
		calls->log.push_back("add " + std::to_string(r_player_id));
		return true;
	}
	void terminate_player(int p_player_id) override {
		calls->log.push_back("terminate " + std::to_string(p_player_id));
	}
	void give_damage(AiObject *, int, int, float) override {}
	float recommended_view_distance() const override { return 120.0f; }
};

struct FakeSai final : StrategicAi {
	Calls *calls = nullptr;
	int side = 0;
	void add_bot(Bot *p_bot) override {
		calls->log.push_back("sai" + std::to_string(side) + " add " + std::to_string(p_bot->bot_id()));
	}
	void remove_bot(Bot *p_bot) override {
		calls->log.push_back("sai" + std::to_string(side) + " remove " + std::to_string(p_bot->bot_id()));
	}
	void update(double p_budget) override {
		calls->log.push_back("sai" + std::to_string(side) + " update");
		CHECK(p_budget > 0.0);
	}
};

struct FakeSquads final : SquadUpdater, LevelListener {
	Calls *calls = nullptr;
	void update(sim::Tick p_tick) override { calls->log.push_back("squads " + std::to_string(p_tick)); }
	void new_squad_leader(int p_side, int p_squad) override {
		calls->log.push_back("leader " + std::to_string(p_side) + " " + std::to_string(p_squad));
	}
	void player_joins_squad(int p_player_id, int p_squad, int p_side) override {
		calls->log.push_back(
				"joins " + std::to_string(p_player_id) + " " + std::to_string(p_squad) + " " + std::to_string(p_side));
	}
	void post_load_init_areas() override { calls->log.push_back("areas"); }
};

struct World {
	Calls calls;
	FakeTimer timer;
	FakeFactory factory;
	FakeGame game;
	FakeSai sai1, sai2;
	FakeSquads squads;
	BF2AI ai;

	explicit World(AiSettings p_settings = AiSettings{}) {
		factory.calls = &calls;
		game.calls = &calls;
		sai1.calls = &calls;
		sai1.side = 1;
		sai2.calls = &calls;
		sai2.side = 2;
		squads.calls = &calls;
		AiServices services;
		services.settings = p_settings;
		services.timer = &timer;
		services.game = &game;
		services.factory = &factory;
		services.squads = &squads;
		services.level = &squads;
		services.match_seed = 99;
		REQUIRE(ai.init(services));
		ai.main().set_sai(1, &sai1);
		ai.main().set_sai(2, &sai2);
	}
};

}

TEST_SUITE("ai-port") {
	TEST_CASE("BF2AI: init and clear run in OpenBF2's order; a second init is refused") {
		World w;
		using P = BF2AI::Part;
		CHECK(w.ai.init_order() ==
				std::vector<P>{ P::InformationManager, P::WeaponInfoManager, P::InformationFactory, P::Settings,
						P::EventCourier, P::BehaviourManager, P::Random, P::Main });
		CHECK(w.ai.is_initialized());
		CHECK_FALSE(w.ai.is_shutting_down());
		AiServices again;
		CHECK_FALSE(w.ai.init(again));
		CHECK(w.ai.random().seed() == AIRandom::seed_for(99, 0));
		CHECK(w.ai.knowledge().factory == &w.ai.factory());
		CHECK(w.ai.clear());
		CHECK_FALSE(w.ai.is_initialized());
		CHECK(w.ai.clear_order() ==
				std::vector<P>{ P::InformationFactory, P::EventCourier, P::BehaviourManager, P::Main,
						P::InformationManager, P::WeaponInfoManager, P::Settings, P::Random });
		CHECK_FALSE(w.ai.update());
		CHECK(w.ai.clear());
	}

	TEST_CASE("AIEventCourier: posted events are delivered in order on the next update") {
		AiSettings settings;
		settings.auto_spawn_bots = true;
		settings.max_bots = 4;
		World w(settings);
		AIEventCourier &events = w.ai.events();
		events.event_level_loaded();
		events.event_joined_squad(5, 2, 1);
		events.event_new_squad_leader(1, 2);
		events.event_player_created(false);
		events.event_game_start();
		events.event_player_switch_sides(2, 10);
		CHECK(events.pending() == 6);
		CHECK(w.calls.log.empty());
		CHECK(w.ai.main().init(1) == "AIMain initiated.");
		CHECK(w.ai.settings().view_distance_m == 120.0f);
		CHECK(w.ai.update());
		CHECK(events.pending() == 0);
		const std::vector<std::string> expected{ "areas", "joins 5 2 1", "leader 1 2", "add 10", "sai1 add 0", "add 11",
			"sai2 add 1", "add 12", "sai1 add 2", "sai1 remove 0", "sai2 add 0", "sai1 update", "squads 600" };
		REQUIRE(w.calls.log.size() >= expected.size());
		CHECK(std::vector<std::string>(w.calls.log.begin(), w.calls.log.begin() + std::ptrdiff_t(expected.size())) ==
				expected);
		CHECK(w.ai.main().human_player_count() == 1);
		CHECK(w.ai.main().bot_manager().bot_count() == 3);
		CHECK(w.ai.main().bot_manager().bot_from_player_id(11)->bot_id() == 1);
		CHECK(w.ai.main().bot_manager().bot_from_player_id(10)->side() == 2);
		CHECK(w.ai.main().component_cursor() == 1);
		w.calls.log.clear();
		CHECK(w.ai.update());
		CHECK(w.calls.log.front() == "sai2 update");
	}

	TEST_CASE("BotManager: queued bots get decision and sensing slices from the tick budget, in priority order") {
		AiSettings settings;
		settings.max_bots = 3;
		World w(settings);
		AIMain &main = w.ai.main();
		FakeObject body;
		const uint32_t equipment = w.ai.factory().request_information_real(kNoHandle, &body, 1u, 0.5f, 600, 0);
		for (FakeBot *const bot : w.factory.bots) {
			bot->equipment = equipment;
		}
		w.ai.behaviours().set_behaviour_name(4, "Idle");
		CHECK(main.add_unspawned_bot(20, 1, nullptr));
		CHECK(main.add_unspawned_bot(21, 2, nullptr));
		CHECK(main.add_unspawned_bot(22, 1, nullptr));
		CHECK_FALSE(main.add_unspawned_bot(23, 1, nullptr));
		BotManager &bots = main.bot_manager();
		REQUIRE(bots.bot_count() == 3);
		CHECK(bots.number_of_active_bots() == 3);
		CHECK(w.factory.bots[0]->idle == 4);
		CHECK(bots.last_created_bot() == w.factory.bots[2]);
		CHECK(bots.bot_from_player_id(21) == w.factory.bots[1]);
		CHECK(bots.bot_from_player_id(99) == nullptr);
		CHECK(bots.request_decision(0, false, 1));
		CHECK(bots.request_decision(2, false, 5));
		CHECK(bots.request_decision(1, true, 3));
		CHECK_FALSE(bots.request_decision(7, true, 3));
		CHECK(bots.request_sensing(1, 2));
		w.calls.log.clear();
		bots.action(0.01);
		CHECK(w.calls.log == std::vector<std::string>{ "decide 2", "decide 0", "decide 1", "see 1" });
		CHECK(bots.info(2).decision_updates == 1);
		CHECK(bots.info(2).priority == 0);
		CHECK(bots.info(1).sensing_updates == 1);
		CHECK(w.factory.bots[2]->last_slice == doctest::Approx(w.factory.bots[0]->last_slice));
		CHECK(w.factory.bots[0]->last_slice > 0.0);
		CHECK(w.factory.bots[0]->last_slice <= 0.01 * 0.2 / 3.0 + 1e-9);
		CHECK(w.factory.bots[0]->talks + w.factory.bots[1]->talks + w.factory.bots[2]->talks == 1);
		w.calls.log.clear();
		bots.action(0.01);
		CHECK(w.calls.log.empty());
		w.timer.step = 1.0;
		CHECK(bots.request_decision(0, false, 1));
		CHECK(bots.request_decision(1, false, 1));
		w.calls.log.clear();
		bots.action(0.5);
		CHECK(w.calls.log.size() <= 1);
		CHECK(bots.updates().size() == 5);
	}

	TEST_CASE("AIMain: a human joining a full server terminates a bot on the larger side") {
		AiSettings settings;
		settings.auto_spawn_bots = true;
		settings.max_bots = 4;
		World w(settings);
		AIMain &main = w.ai.main();
		for (int i = 0; i < 4; ++i) {
			main.new_player_created(true);
		}
		w.game.next_player = 14;
		w.calls.log.clear();
		main.new_player_created(false);
		CHECK(w.calls.log == std::vector<std::string>{ "terminate 13" });
		CHECK(main.bot_player_count() == 3);
		CHECK(main.human_player_count() == 1);
		CHECK_FALSE(main.set_sai_enable(true, 0));
		CHECK_FALSE(main.sai_enabled());
	}

	TEST_CASE("AILODManager: bots near a viewer run at Full, farther at Medium, else Light, with hysteresis") {
		struct Viewers final : LodEnvironment {
			std::map<int, LodViewer> list;
			std::vector<int> active_player_ids() const override {
				std::vector<int> ids;
				for (const auto &entry : list) {
					ids.push_back(entry.first);
				}
				return ids;
			}
			bool is_valid_player_id(int p_player_id) const override { return list.count(p_player_id) != 0; }
			int next_player_id(int p_player_id) const override {
				const auto next = list.upper_bound(p_player_id);
				return next != list.end() ? next->first : -1;
			}
			bool viewer(int p_player_id, LodViewer &r_viewer) const override {
				const auto found = list.find(p_player_id);
				if (found == list.end()) {
					return false;
				}
				r_viewer = found->second;
				return true;
			}
		};
		AiSettings settings;
		settings.max_bots = 3;
		World w(settings);
		AIMain &main = w.ai.main();
		std::array<FakeObject, 3> bodies;
		for (size_t i = 0; i < 3; ++i) {
			bodies[i].legs.velocity = {};
			bodies[i].where = { 100.0f * float(i) + 50.0f, 0.0f, 0.0f };
			w.factory.bots[i]->equipment =
					w.ai.factory().request_information_real(kNoHandle, &bodies[i], 1u, 0.5f, 600, 0);
			w.ai.factory().request_information_plug_in(w.factory.bots[i]->equipment, PlugInRequest::Mobile, true);
			CHECK(main.add_unspawned_bot(20 + int(i), 1, nullptr));
		}
		Viewers viewers;
		FakeObject human;
		human.legs.velocity = { 3.0f, 0.0f, 4.0f };
		viewers.list[5] = LodViewer{ true, &human, { 999.0f, 0.0f, 0.0f }, 2.0f };
		viewers.list[6] = LodViewer{ false, &human, { 1000.0f, 0.0f, 0.0f }, 1.0f };
		AILODManager *const lod = main.create_lod_manager(viewers, 50.0f, 2.0f);
		REQUIRE(lod != nullptr);
		REQUIRE(lod->player_stats().size() == 2);
		CHECK(lod->player_stats().at(5).lod_distance == doctest::Approx((50.0f + 2.0f * 5.0f) * 2.0f));
		CHECK(lod->player_stats().at(5).position.x == 0.0f);
		CHECK(lod->player_stats().at(6).position.x == 1000.0f);
		CHECK(lod->player_stats().at(6).lod_distance == doctest::Approx(50.0f));
		lod->update_bots();
		CHECK(w.factory.bots[0]->lod == BotLodLevel::Full);
		CHECK(w.factory.bots[1]->lod == BotLodLevel::Medium);
		CHECK(w.factory.bots[2]->lod == BotLodLevel::Light);
		bodies[0].where.x = 125.0f;
		bodies[1].where.x = 175.0f;
		lod->update_bots();
		CHECK(w.factory.bots[0]->lod == BotLodLevel::Full);
		CHECK(w.factory.bots[1]->lod == BotLodLevel::Medium);
		viewers.list.erase(6);
		lod->update_players();
		lod->update_players();
		CHECK(lod->player_stats().size() == 1);
		lod->remove_player(5);
		CHECK(lod->player_stats().empty());
		lod->update_bots();
		CHECK(w.factory.bots[0]->lod == BotLodLevel::Light);
		lod->lod_enable(false);
		lod->update_bots();
		CHECK(w.factory.bots[0]->lod == BotLodLevel::Full);
	}

	TEST_CASE("AI console: the bot manager and LOD commands read and set what OpenBF2's console binds") {
		World w;
		AIMain &main = w.ai.main();
		AiConsole console;
		register_main_commands(console, main, nullptr);
		CHECK(console.execute("getLodLevelFullTicks").text == "6");
		CHECK(console.execute("setLodLevelTicks 3 2 1").ok);
		CHECK(main.bot_manager().lod_level_ticks(BotLodLevel::Medium) == 2);
		CHECK(console.execute("getLodLevelLightTicks").text == "1");
		CHECK(console.execute("setSystemQuotient 50 25 25").ok);
		CHECK(main.bot_manager().system_decision_making_quotient() == doctest::Approx(0.5));
		CHECK(main.bot_manager().system_sensing_quotient() == doctest::Approx(0.25));
		CHECK(console.execute("setPlannedDecisionMakingThreshold 0.5 -1 3").ok);
		CHECK(main.bot_manager().planned_decision_making_threshold(BotLodLevel::Medium) == 1.0);
		CHECK(console.execute("getBotCount").text == "0");
		const ConsoleResult wrong = console.execute("setLodLevelTicks 3 2");
		CHECK_FALSE(wrong.ok);
		CHECK(wrong.text == "setLodLevelTicks: takes 3..3 arguments");
		CHECK(console.execute("setLodLevelTicks 3 x 1").text == "bad argument: x");
		CHECK(console.execute("noSuchCommand").text == "unknown command: noSuchCommand");
		CHECK_FALSE(console.execute("").ok);
		CHECK(console.execute("lodEnable").text == "no LOD manager");
		const std::vector<std::string> names = console.names();
		CHECK(std::find(names.begin(), names.end(), "createLODManager") == names.end());
		CHECK(names.size() == 32);
		struct NoViewers final : LodEnvironment {
			std::vector<int> active_player_ids() const override { return {}; }
			bool is_valid_player_id(int) const override { return false; }
			int next_player_id(int) const override { return -1; }
			bool viewer(int, LodViewer &) const override { return false; }
		} viewers;
		AiConsole with_lod;
		register_main_commands(with_lod, main, &viewers);
		CHECK(with_lod.execute("createLODManager 80 1.5").ok);
		REQUIRE(main.lod_manager() != nullptr);
		CHECK(main.lod_manager()->basic_lod_distance() == 80.0f);
		CHECK(with_lod.execute("lodEnable").text == "1");
		CHECK(with_lod.execute("lodEnable 0").ok);
		CHECK_FALSE(main.lod_manager()->lod_enabled());
		CHECK(with_lod.execute("lodEnable true").ok);
		CHECK(main.lod_manager()->lod_enabled());
		CHECK(with_lod.execute("lodEnable 2").text == "bad argument: 2");
		CHECK(with_lod.execute("setBasicLODDistance 120").ok);
		CHECK(main.lod_manager()->basic_lod_distance() == 120.0f);
	}
}
