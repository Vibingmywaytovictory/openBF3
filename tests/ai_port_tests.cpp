#include <doctest/doctest.h>

#include "ai_bot_fake.h"
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
#include "bf3/ai/main/bot_placement_manager.h"
#include "bf3/ai/main/communication_hub.h"
#include "bf3/ai/strategic/strategic_object.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
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

	TEST_CASE("AISettings: the reconstruction's defaults, its vehicles and the side personalities") {
		const AiSettings d;
		CHECK(d.max_bots == -1);
		CHECK(d.max_bots_include_humans);
		CHECK(d.ignore_commander_distance_m == 30.0f);
		CHECK(d.behaviour_count == 0);
		CHECK(d.view_distance_m == -1.0f);
		CHECK(d.stats_view_distance_m == 30.0f);
		CHECK(d.draw_bot_stats);
		CHECK(d.bot_skill == 0.75f);
		CHECK(d.sai_update_frequency == 35.0f);
		CHECK(d.information_grid_dimension == 32u);
		CHECK(d.sides == 0u);
		CHECK(d.respawn_allowed);
		CHECK_FALSE(d.draw);
		CHECK_FALSE(d.auto_spawn_bots);
		CHECK_FALSE(d.override_menu_settings);
		CHECK_FALSE(d.domain_enabled(AiDomain::Vehicle));
		CHECK_FALSE(d.domain_enabled(AiDomain::Air));
		CHECK(d.domain_enabled(AiDomain::Infantry));
		CHECK(d.armour_class_value(3) == 15.0f);

		AiSettings s;
		s.set_vehicle("Infantry");
		s.set_vehicle("Jeep");
		s.set_vehicle("Infantry");
		CHECK(s.vehicle("Infantry") == 0);
		CHECK(s.vehicle("Jeep") == 1);
		CHECK(s.vehicle("Tank") == -1);
		CHECK(s.vehicles.size() == 3u);
		CHECK(s.personality(1) == nullptr);
		s.standard_personality[1] = { 2.0f, 3.0f };
		REQUIRE(s.personality(2) != nullptr);
		CHECK(s.personality(2)[1] == 3.0f);
		CHECK(s.personality(0) == nullptr);
		CHECK(s.personality(3) == nullptr);
		s.enable_air_ai = true;
		CHECK(s.domain_enabled(AiDomain::Air));
	}
}

namespace {

struct Calls {
	std::vector<std::string> log;
};

struct FakeBot final : NullBot {
	Calls *calls = nullptr;
	const float *personality = nullptr;
	int idle = -2;
	int decisions = 0;
	int executions = 0;
	int seen = 0;
	int talks = 0;
	int heard = 0;
	int resets = 0;
	int refreshes = 0;
	int stalls = 0;
	bool pending_search = false;
	bool stuck = false;
	double last_slice = 0.0;
	double change_time = 0.0;
	Vec3 spawn{};
	FakeBot() { squad_member = true; }
	void receive_idle_order(int p_idle) override { idle = p_idle; }
	int decision_making(double p_slice) override {
		++decisions;
		last_slice = p_slice;
		planned = true;
		calls->log.push_back("decide " + std::to_string(id));
		return 1;
	}
	void plan_execution(double) override {
		++executions;
		calls->log.push_back("execute " + std::to_string(id));
	}
	void update_player_action(bool p_stalled) override { stalls += p_stalled ? 1 : 0; }
	void see(double) override {
		++seen;
		calls->log.push_back("see " + std::to_string(id));
	}
	void heard_object(uint32_t p_information) override {
		++heard;
		calls->log.push_back("heard " + std::to_string(id) + " " + std::to_string(p_information));
	}
	void update_communication() override { ++talks; }
	bool path_search_pending() const override { return pending_search; }
	void set_bot_stuck(bool p_stuck) override { stuck = p_stuck; }
	bool update_bot() override {
		++refreshes;
		return true;
	}
	void reset() override { ++resets; }
	void set_last_change_time(double p_time) override { change_time = p_time; }
	void set_spawn_coord(const Vec3 &p_position) override { spawn = p_position; }
	void set_is_repaired(bool p_repaired) override { repaired = p_repaired; }
	bool repaired = false;
	void set_personality(const float *p_weights) override { personality = p_weights; }
};

struct FakeFactory final : BotFactory {
	Calls *calls = nullptr;
	uint32_t equipment = kNoHandle;
	std::vector<FakeBot *> bots;
	std::unique_ptr<Bot> create_bot(
			int p_player_id, uint32_t p_bot_id, const float *p_personality, int p_side) override {
		auto bot = std::make_unique<FakeBot>();
		bot->calls = calls;
		bot->personality = p_personality;
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
	int player_side(int p_player_id) const override {
		const auto found = sides.find(p_player_id);
		if (found != sides.end()) {
			return found->second;
		}
		return p_player_id >= 10 && p_player_id < next_player ? 1 : 0;
	}
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
	void respawn_bot(int p_player_id) override { calls->log.push_back("respawn " + std::to_string(p_player_id)); }
	void set_bot_name_unused(const std::string &p_name) override { calls->log.push_back("unused " + p_name); }
	AiObject *player_vehicle(int) const override { return vehicle; }
	AiObject *player_default_vehicle(int) const override { return vehicle; }
	void occupied_objects(std::vector<std::pair<int, AiObject *>> &r_out) const override { r_out = occupied; }
	uint32_t force_information(AiObject &r_object, int p_side, bool) override { return r_object.information(p_side); }
	bool player_camera(int, Transform &r_camera) const override {
		r_camera = camera;
		return true;
	}
	void enemy_spotted_by_ai(int p_spotter, const AiObject &) override {
		calls->log.push_back("spotted " + std::to_string(p_spotter));
	}
	int player_score(int p_player_id) const override {
		const auto found = scores.find(p_player_id);
		return found != scores.end() ? found->second : 0;
	}
	void set_player_side(int p_player_id, int p_side) override { sides[p_player_id] = p_side; }
	AiObject *vehicle = nullptr;
	std::vector<std::pair<int, AiObject *>> occupied;
	Transform camera;
	std::map<int, int> sides;
	std::map<int, int> scores;
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
	void set_bot_has_changed_equipment(int p_bot_id) override {
		calls->log.push_back("sai" + std::to_string(side) + " changed " + std::to_string(p_bot_id));
	}
	bool player_requests_artillery(int p_player_id, const Vec3 &) override {
		calls->log.push_back("sai" + std::to_string(side) + " artillery " + std::to_string(p_player_id));
		return true;
	}
	bool player_requests_uav(int p_player_id, const Vec3 &) override {
		calls->log.push_back("sai" + std::to_string(side) + " uav " + std::to_string(p_player_id));
		return true;
	}
	bool player_requests_supply(int p_player_id, const Vec3 &) override {
		calls->log.push_back("sai" + std::to_string(side) + " supply " + std::to_string(p_player_id));
		return true;
	}
	bool player_requests_vehicle_drop(int p_player_id, const Vec3 &) override {
		calls->log.push_back("sai" + std::to_string(side) + " drop " + std::to_string(p_player_id));
		return true;
	}
	void set_player_is_commander(bool p_commander) override {
		calls->log.push_back("sai" + std::to_string(side) + " commander " + std::to_string(int(p_commander)));
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
	bool remove_squad_member(int p_player_id, int p_side) override {
		calls->log.push_back("remove " + std::to_string(p_player_id) + " " + std::to_string(p_side));
		return true;
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

	TEST_CASE("AIEventCourier: game events reach AIMain, the squads and the level as they happen") {
		AiSettings settings;
		settings.auto_spawn_bots = true;
		settings.max_bots = 4;
		settings.sides = 2;
		World w(settings);
		AIEventCourier &events = w.ai.events();
		events.event_level_loaded();
		events.event_joined_squad(5, 2, 1);
		events.event_new_squad_leader(1, 2);
		events.event_player_created(false);
		events.event_game_start();
		events.event_player_switch_sides(2, 10);
		CHECK(w.calls.log ==
				std::vector<std::string>{ "areas", "joins 5 2 1", "leader 1 2", "add 10", "sai1 add 0", "add 11",
						"sai2 add 1", "add 12", "sai1 add 2", "sai1 remove 0", "sai2 add 0" });
		CHECK(w.ai.main().init(1) == "AIMain initiated.");
		CHECK(w.ai.settings().view_distance_m == 120.0f);
		w.calls.log.clear();
		CHECK(w.ai.update());
		const std::vector<std::string> expected{ "sai1 update", "squads 600" };
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

	TEST_CASE("AIEventCourier: a bot's life, the commander requests and the resets go where OpenBF2's courier sends "
			  "them") {
		AiSettings settings;
		settings.max_bots = 2;
		World w(settings);
		AIMain &main = w.ai.main();
		CHECK(main.init(1) == "AIMain initiated.");
		REQUIRE(main.add_unspawned_bot(10, 1, nullptr));
		REQUIRE(main.add_unspawned_bot(11, 2, nullptr));
		FakeBot &first = *w.factory.bots[0];
		FakeBot &second = *w.factory.bots[1];
		AIEventCourier &events = w.ai.events();
		events.event_player_killed(10, 1);
		CHECK(first.transition);
		CHECK(first.resets == 1);
		events.event_player_spawned(10);
		CHECK_FALSE(first.transition);
		CHECK(first.change_time == -1000000.0);
		FakeObject enemy;
		enemy.team = 2;
		const uint32_t known = w.ai.factory().request_information_known(kNoHandle, &enemy, 1u, 0, 5.0f, 0.5f, 0, 1);
		REQUIRE(w.ai.factory().request_information_plug_in(known, PlugInRequest::Physical, false));
		enemy.handles[1] = known;
		events.event_man_down(11, enemy);
		CHECK(second.down);
		CHECK(w.ai.information().information(known)->physical()->health() == 0.0f);
		events.event_revived(11);
		CHECK_FALSE(second.down);
		events.event_missile_lock(&enemy);
		CHECK(enemy.squad_unit.lock == 1);
		first.mine = 77;
		events.event_mine_removed(77);
		CHECK(first.mine == kNoHandle);
		events.event_is_repaired(10);
		CHECK(first.repaired);
		w.calls.log.clear();
		CHECK(events.event_received_artillery_request(1, 10, Vec3{}));
		CHECK(events.event_received_supply_request(2, 11, Vec3{}));
		CHECK_FALSE(events.event_received_uav_request(3, 11, Vec3{}));
		events.event_accepted_commander(4, 2);
		events.event_resign_as_commander(4, 2);
		CHECK(w.calls.log ==
				std::vector<std::string>{
						"sai1 artillery 10", "sai2 supply 11", "sai2 commander 1", "sai2 commander 0" });
		w.game.sides = { { 10, 1 } };
		w.calls.log.clear();
		events.event_bot_switch_sides(1);
		CHECK(w.calls.log == std::vector<std::string>{ "sai1 remove 0", "sai2 add 0" });
		w.calls.log.clear();
		events.event_player_disconnect(11);
		events.event_player_disconnect(3);
		CHECK(w.calls.log == std::vector<std::string>{ "remove 11 0", "unused ", "remove 3 0" });

		AIStrategicObjectManager objects{ StrategicServices{} };
		REQUIRE(objects.create("north", { -10.0f, -10.0f }, { 0.0f, 0.0f }, 0.0f, 10.0f, std::string()));
		CommunicationHubServices hub_services;
		hub_services.objects = &objects;
		CommunicationHub hub(hub_services);
		AIEventCourier reset_events;
		AiEventServices reset_services;
		reset_services.hub = &hub;
		reset_events.attach(reset_services);
		CHECK(hub.combat_strengths(1).empty());
		reset_events.event_world_reset();
		CHECK(hub.combat_strengths(1).size() == 1);
	}

	TEST_CASE("BotManager: plan execution runs half the roster a tick and queues decisions and sensing by its "
			  "thresholds") {
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
		w.calls.log.clear();
		bots.action(0.01);
		CHECK(w.calls.log == std::vector<std::string>{ "execute 0", "see 0" });
		w.calls.log.clear();
		bots.action(0.01);
		CHECK(w.calls.log == std::vector<std::string>{ "execute 1", "execute 2", "see 1", "see 2" });
		CHECK(bots.info(1).since_decision == doctest::Approx(1.0 / 60.0));
		CHECK(bots.plan_execution_update_count(1) == 1);
		CHECK(bots.sensing_update_count(2) == 1);
		CHECK(w.factory.bots[0]->talks + w.factory.bots[1]->talks + w.factory.bots[2]->talks == 2);
		for (int i = 0; i < 40; ++i) {
			bots.action(0.01);
		}
		for (uint32_t i = 0; i < 3; ++i) {
			CHECK(w.factory.bots[i]->decisions >= 1);
			CHECK(bots.decision_making_update_count(i) == uint32_t(w.factory.bots[i]->decisions));
			CHECK(bots.info(i).since_decision < 0.5 + 1e-9);
		}
		bots.set_lod_level_ticks(6, 3, 1);
		w.factory.bots[1]->lod = BotLodLevel::Medium;
		for (int i = 0; i < 59; ++i) {
			bots.action_execute_plan(0.01);
		}
		w.calls.log.clear();
		bots.action_execute_plan(0.01);
		CHECK(w.calls.log == std::vector<std::string>{ "execute 1", "execute 2" });
		w.calls.log.clear();
		bots.action_decision_making(0.009);
		CHECK(w.calls.log == std::vector<std::string>{ "decide 1", "decide 2" });
		CHECK(w.factory.bots[1]->last_slice == doctest::Approx(0.003));
		CHECK(w.factory.bots[2]->last_slice == doctest::Approx(0.006));
		w.factory.bots[1]->lod = BotLodLevel::Full;
		bots.set_lod_level_ticks(6, 6, 6);
		CHECK(bots.updates().size() == 5);
		bots.reset_statistics(0);
		CHECK(bots.plan_execution_update_count(0) == 0);
		CHECK(bots.decision_making_update_count(0) == 0);
		w.factory.bots[1]->pending_search = true;
		body.legs.velocity = {};
		w.game.vehicle = &body;
		w.calls.log.clear();
		bots.action_execute_plan(0.01);
		bots.action_execute_plan(0.01);
		CHECK(std::find(w.calls.log.begin(), w.calls.log.end(), "respawn 21") != w.calls.log.end());
		CHECK(std::find(w.calls.log.begin(), w.calls.log.end(), "execute 1") == w.calls.log.end());
		w.timer.step = 1.0;
		w.calls.log.clear();
		w.factory.bots[1]->pending_search = false;
		const int stalls = w.factory.bots[0]->stalls + w.factory.bots[1]->stalls + w.factory.bots[2]->stalls;
		bots.action_execute_plan(0.5);
		CHECK(w.calls.log.empty());
		CHECK(w.factory.bots[0]->stalls + w.factory.bots[1]->stalls + w.factory.bots[2]->stalls == stalls + 1);
	}

	TEST_CASE("BotManager: death, spawn, man-down and revive drive the bot as OpenBF2's IBotManager does") {
		struct CallBack final : BotCallBack {
			int reports = 0;
			void report() override { ++reports; }
		};
		struct ReportingBot final : NullBot {
			CallBack died;
			CallBack spawned;
			int resets = 0;
			int nav_resets_all = 0;
			int nav_resets = 0;
			double change_time = 0.0;
			Vec3 spawn{};
			void reset() override { ++resets; }
			void reset_navigation(bool p_all) override { ++(p_all ? nav_resets_all : nav_resets); }
			void set_last_change_time(double p_time) override { change_time = p_time; }
			void set_spawn_coord(const Vec3 &p_position) override { spawn = p_position; }
			BotCallBack *death_call_back() const override { return const_cast<CallBack *>(&died); }
			BotCallBack *spawn_call_back() const override { return const_cast<CallBack *>(&spawned); }
		};
		struct Factory final : BotFactory {
			std::vector<ReportingBot *> made;
			std::unique_ptr<Bot> create_bot(int p_player_id, uint32_t p_bot_id, const float *, int p_side) override {
				auto bot = std::make_unique<ReportingBot>();
				bot->id = p_bot_id;
				bot->player = p_player_id;
				bot->team = p_side;
				made.push_back(bot.get());
				return bot;
			}
		};
		Calls calls;
		FakeTimer timer;
		FakeGame game;
		game.calls = &calls;
		FakeSai sai;
		sai.calls = &calls;
		sai.side = 1;
		Factory factory;
		Knowledge knowledge;
		FakeObject body;
		body.where = { 4.0f, 0.0f, 9.0f };
		const uint32_t equipment = knowledge.soldier(body, 1);
		AiSettings settings;
		settings.max_bots = 2;
		AiMainServices services;
		services.settings = &settings;
		services.timer = &timer;
		services.information = &knowledge.information;
		services.weapons = &knowledge.weapons;
		services.game = &game;
		services.factory = &factory;
		AIMain main(services);
		main.set_sai(1, &sai);
		CHECK(main.init(1) == "AIMain initiated.");
		BotManager &bots = main.bot_manager();
		REQUIRE(bots.add_unspawned_bot(30, 1, nullptr));
		ReportingBot &bot = *factory.made[0];
		bot.equipment = equipment;
		bot.mine = 77;
		bot.bot_name = "Alpha";
		CHECK(bots.bot_from_name("Alpha") == &bot);
		bots.set_bot_is_alive(30);
		CHECK(bot.spawned.reports == 0);
		bots.set_bot_is_dead(30);
		CHECK(bot.transition);
		CHECK(bot.resets == 1);
		CHECK(bot.nav_resets_all == 1);
		CHECK(bot.equipment == kNoHandle);
		CHECK(bot.died.reports == 1);
		CHECK(bots.info(0).dead);
		bot.equipment = equipment;
		bots.set_bot_is_alive(30);
		CHECK_FALSE(bot.transition);
		CHECK(bot.change_time == -1000000.0);
		CHECK(bot.spawn.x == 4.0f);
		CHECK(bot.spawn.z == 9.0f);
		CHECK(bot.spawned.reports == 1);
		CHECK_FALSE(bots.info(0).dead);
		bots.set_bot_is_man_down(30);
		CHECK(bot.down);
		CHECK(bot.nav_resets_all == 2);
		calls.log.clear();
		bots.set_bot_is_revived(30);
		CHECK_FALSE(bot.down);
		CHECK(bot.nav_resets == 1);
		CHECK(calls.log == std::vector<std::string>{ "sai1 changed 0" });
		bots.remove_mine_target(77);
		CHECK(bot.mine == kNoHandle);
		bots.register_damage_on_bot(5.0f, 0, 0);
		CHECK(bots.info(0).damage == 5.0f);
		bots.update_damages();
		CHECK(bots.info(0).damage == 0.0f);
		CHECK(bots.vehicle_bail_value() == 0.3f);
		bot.dead = true;
		calls.log.clear();
		CHECK(bots.deactivate_bot_from_player_id(30) == 0);
		CHECK(calls.log == std::vector<std::string>{ "unused Alpha" });
		CHECK(bot.deactivated);
		CHECK(bots.bot_from_player_id(30) == nullptr);
		CHECK(bots.number_of_deactivated_bots() == 1);
		CHECK(bots.reactivate_bot(31, 1, "Bravo"));
		CHECK(bots.bot_from_player_id(31) == &bot);
		CHECK(bot.bot_name == "Bravo");
		CHECK(bots.reset());
		CHECK(bots.bot_count() == 0);
		CHECK(bots.lod_level_ticks(BotLodLevel::Light) == 1);
		CHECK(bots.unplanned_decision_making_threshold(BotLodLevel::Medium) == 0.8);
	}

	TEST_CASE("AIMain: a bot activated for a side takes that side's standard personality, a side without one none, "
			  "as AIMain::activateBot passes getStandardPersonality") {
		AiSettings settings;
		settings.max_bots = 3;
		settings.standard_personality[1] = std::vector<float>(10, 2.5f);
		World w(settings);
		AIMain &main = w.ai.main();
		REQUIRE(main.activate_bot(30, 2, "Alpha"));
		REQUIRE(main.activate_bot(31, 1, "Bravo"));
		const auto bot_of = [&](int p_player) -> const FakeBot * {
			for (const FakeBot *b : w.factory.bots) {
				if (b->player == p_player) {
					return b;
				}
			}
			return nullptr;
		};
		REQUIRE(bot_of(30) != nullptr);
		REQUIRE(bot_of(31) != nullptr);
		REQUIRE(bot_of(30)->personality != nullptr);
		CHECK(bot_of(30)->personality[1] == 2.5f);
		CHECK(bot_of(31)->personality == nullptr);
	}

	TEST_CASE("BotManager: a sounding enemy within its radius is heard by every live bot of the other side") {
		AiSettings settings;
		settings.max_bots = 2;
		World w(settings);
		AIMain &main = w.ai.main();
		CHECK(main.add_unspawned_bot(20, 1, nullptr));
		CHECK(main.add_unspawned_bot(21, 2, nullptr));
		BotManager &bots = main.bot_manager();
		FakeObject listener_a;
		FakeObject listener_b;
		FakeObject enemy;
		listener_a.where = { 5.0f, 0.0f, 0.0f };
		listener_b.where = { 50.0f, 0.0f, 0.0f };
		listener_b.team = 2;
		enemy.team = 2;
		enemy.handles[1] = 42;
		bots.update_bot_vehicle(*w.factory.bots[0], &listener_a);
		bots.update_bot_vehicle(*w.factory.bots[1], &listener_b);
		CHECK(bots.bot_from_object(&listener_a) == w.factory.bots[0]);
		w.game.occupied = { { 20, &listener_a }, { 21, &listener_b }, { 3, &enemy } };
		w.calls.log.clear();
		bots.action_hearing(1.0);
		CHECK(w.calls.log == std::vector<std::string>{ "heard 0 42" });
		w.factory.bots[0]->down = true;
		w.calls.log.clear();
		bots.action_hearing(1.0);
		CHECK(w.calls.log.empty());
		w.factory.bots[0]->down = false;
		listener_a.arms.list.clear();
		listener_b.arms.list.clear();
		listener_a.where = { 30.0f, 0.0f, 0.0f };
		w.timer.tick = 60;
		w.calls.log.clear();
		bots.action_hearing(1.0);
		CHECK(w.calls.log == std::vector<std::string>{ "heard 0 42" });
		w.timer.tick = 600;
		w.calls.log.clear();
		bots.action_hearing(1.0);
		CHECK(w.calls.log.empty());
		AiMainServices untimed;
		untimed.settings = &settings;
		untimed.game = &w.game;
		untimed.factory = &w.factory;
		AIMain timeless(untimed);
		CHECK(timeless.init(1) == "AIMain initiated.");
		REQUIRE(timeless.bot_manager().add_unspawned_bot(30, 1, nullptr));
		timeless.bot_manager().update_bot_vehicle(*timeless.bot_manager().bot(0), &listener_a);
		w.game.occupied = { { 30, &listener_a }, { 3, &enemy } };
		w.calls.log.clear();
		timeless.bot_manager().action_hearing(1.0);
		CHECK(w.calls.log == std::vector<std::string>{ "heard 0 42" });
	}

	TEST_CASE("CommunicationHub: a pickup request within 20 s goes to the bot it names, the rest to the radio") {
		struct Radio final : RadioComm {
			std::vector<std::pair<int, int>> sent;
			void send_radio_message(int, int p_side, const RadioMessage &p_message) override {
				sent.emplace_back(p_side, p_message.type);
			}
		};
		struct Voices final : RadioVoices {
			bool settings(int p_type, RadioMessageSettings &r_settings) const override {
				r_settings = RadioMessageSettings{ p_type, 1.0f, 30.0f, 5.0f };
				return true;
			}
		};
		struct TalkingBot final : NullBot {
			std::unique_ptr<CommunicationAgent> agent;
			CommunicationAgent *communication() const override { return agent.get(); }
		};
		struct Factory final : BotFactory {
			Voices *voices = nullptr;
			Radio *radio = nullptr;
			std::vector<TalkingBot *> made;
			std::unique_ptr<Bot> create_bot(int p_player_id, uint32_t p_bot_id, const float *, int p_side) override {
				auto bot = std::make_unique<TalkingBot>();
				bot->id = p_bot_id;
				bot->player = p_player_id;
				bot->team = p_side;
				bot->agent = std::make_unique<CommunicationAgent>(*voices, *radio);
				made.push_back(bot.get());
				return bot;
			}
		};
		Calls calls;
		Knowledge knowledge;
		FakeGame game;
		game.calls = &calls;
		Radio radio;
		Voices voices;
		Factory factory;
		factory.voices = &voices;
		factory.radio = &radio;
		AiSettings settings;
		settings.max_bots = 1;
		AiMainServices services;
		services.settings = &settings;
		services.timer = &knowledge.timer;
		services.information = &knowledge.information;
		services.game = &game;
		services.factory = &factory;
		AIMain main(services);
		REQUIRE(main.bot_manager().add_unspawned_bot(12, 1, nullptr));
		FakeObject body;
		body.where = { 0.0f, 0.0f, 10.0f };
		factory.made[0]->equipment = knowledge.soldier(body, 1);
		CommunicationHubServices hub_services;
		hub_services.radio = &radio;
		hub_services.voices = &voices;
		hub_services.timer = &knowledge.timer;
		hub_services.game = &game;
		hub_services.bots = &main.bot_manager();
		hub_services.information = &knowledge.information;
		CommunicationHub hub(hub_services);
		hub.reset();
		RadioMessage pickup;
		pickup.type = kRadioRequestPickup;
		pickup.receiver = 12;
		hub.send_radio_message(5, 1, pickup);
		CHECK(radio.sent == std::vector<std::pair<int, int>>{ { 1, kRadioRequestPickup } });
		CHECK(hub.last_pickup_send_time(1) == doctest::Approx(10.0));
		knowledge.timer.tick += 60;
		hub.send_radio_message(5, 1, pickup);
		CHECK(radio.sent.size() == 1);
		const CommunicationAgent &agent = *factory.made[0]->agent;
		CHECK(agent.received_message_exists(kRadioRequestPickup, 5));
		pickup.receiver = 3;
		hub.send_radio_message(6, 1, pickup);
		CHECK(radio.sent.size() == 1);
		CHECK_FALSE(agent.received_message_exists(kRadioRequestPickup, 6));
		hub.send_radio_message(6, 2, pickup);
		RadioMessage call;
		call.type = kRadioCallForMedic;
		hub.send_radio_message(6, 1, call);
		CHECK(radio.sent ==
				std::vector<std::pair<int, int>>{
						{ 1, kRadioRequestPickup }, { 2, kRadioRequestPickup }, { 1, kRadioCallForMedic } });

		RadioMessageSettings request{ kRadioRequestPickup, 1.0f, 30.0f, 5.0f };
		hub.filter_and_receive_ai_radio_message(request, 12, 12, false);
		CHECK_FALSE(agent.received_message_exists(kRadioRequestPickup, 12));
		game.camera.forward = { 0.0f, 0.0f, 1.0f };
		hub.filter_and_receive_ai_radio_message(request, 3, 12, false);
		CHECK(agent.received_message_exists(kRadioRequestPickup, 3));
		game.camera.forward = { 0.0f, 0.0f, -1.0f };
		hub.filter_and_receive_ai_radio_message(request, 4, 12, false);
		CHECK_FALSE(agent.received_message_exists(kRadioRequestPickup, 4));
	}

	TEST_CASE(
			"CommunicationHub: a bot's sighting is reported once per area a minute; a human's stamps the information") {
		Calls calls;
		Knowledge knowledge;
		FakeGame game;
		game.calls = &calls;
		AIStrategicObjectManager objects{ StrategicServices{} };
		REQUIRE(objects.create("north", { -10.0f, -10.0f }, { 0.0f, 0.0f }, 0.0f, 10.0f, std::string()));
		REQUIRE(objects.create("south", { 90.0f, -10.0f }, { 100.0f, 0.0f }, 0.0f, 10.0f, std::string()));
		CommunicationHubServices hub_services;
		hub_services.timer = &knowledge.timer;
		hub_services.game = &game;
		hub_services.information = &knowledge.information;
		hub_services.objects = &objects;
		CommunicationHub hub(hub_services);
		hub.reset();
		REQUIRE(hub.combat_strengths(1).size() == 2);
		CHECK(hub.combat_strengths(2)[1] == std::pair<int, double>{ kCombatStrengthNone, -1000.0 });
		FakeObject enemy;
		enemy.primary = true;
		enemy.where = { 5.0f, 0.0f, 5.0f };
		knowledge.soldier(enemy, 1);
		hub.enemy_spotted_by_ai(7, enemy, 1);
		CHECK(calls.log == std::vector<std::string>{ "spotted 7" });
		CHECK(hub.combat_strengths(1)[0].second == doctest::Approx(10.0));
		hub.enemy_spotted_by_ai(7, enemy, 1);
		CHECK(calls.log.size() == 1);
		enemy.where = { 200.0f, 0.0f, 0.0f };
		knowledge.timer.tick += 3600;
		hub.enemy_spotted_by_ai(7, enemy, 1);
		CHECK(calls.log.size() == 1);
		const uint32_t known = knowledge.factory.request_information_known(kNoHandle, &enemy, 1u, 0, 5.0f, 0.5f, 0, 2);
		enemy.handles[2] = known;
		hub.enemy_spotted_by_human(enemy, 2);
		CHECK(knowledge.information.information(known)->time() == knowledge.timer.tick);
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
		CHECK(w.calls.log == std::vector<std::string>{ "terminate 13", "remove 13 1" });
		CHECK(main.bot_player_count() == 3);
		CHECK(main.human_player_count() == 1);
		CHECK_FALSE(main.set_sai_enable(true, 0));
		CHECK_FALSE(main.sai_enabled());
	}

	TEST_CASE("AIMain: deactivating, reactivating and moving bots between sides as OpenBF2's IAIMain does") {
		AiSettings settings;
		settings.max_bots = 3;
		World w(settings);
		AIMain &main = w.ai.main();
		CHECK(main.init(1) == "AIMain initiated.");
		REQUIRE(main.add_unspawned_bot(10, 1, nullptr));
		REQUIRE(main.add_unspawned_bot(11, 2, nullptr));
		REQUIRE(main.add_unspawned_bot(12, 1, nullptr));
		w.game.sides = { { 10, 1 }, { 11, 2 }, { 12, 1 } };
		w.game.scores = { { 10, 5 }, { 12, 2 } };
		w.calls.log.clear();
		main.switch_sides(1);
		CHECK(w.calls.log == std::vector<std::string>{ "sai1 remove 2", "sai2 add 2" });
		CHECK(w.game.sides[12] == 2);
		CHECK(w.factory.bots[2]->team == 2);
		w.calls.log.clear();
		int player = 99;
		CHECK(main.deactivate_bot(player, 2));
		CHECK(player == 11);
		CHECK(w.calls.log == std::vector<std::string>{ "remove 99 2", "unused ", "sai2 remove 1" });
		CHECK(w.factory.bots[1]->deactivated);
		w.calls.log.clear();
		CHECK_FALSE(main.deactivate_bot(10));
		w.factory.bots[0]->dead = true;
		CHECK(main.deactivate_bot(10));
		CHECK(w.calls.log ==
				std::vector<std::string>{ "remove 10 1", "unused ", "remove 10 1", "unused ", "sai1 remove 0" });
		CHECK(main.bot_manager().number_of_deactivated_bots() == 2);
		main.new_player_created(false);
		w.calls.log.clear();
		main.reactivate_bot(3);
		CHECK(main.human_player_count() == 0);
		CHECK(w.calls.log == std::vector<std::string>{ "remove 3 0" });
		main.set_ai_system_components_quotient(1, 3);
		CHECK(main.bot_manager_quotient() == 0.25);
		CHECK(main.component_quotient() == 0.75);
		main.set_ai_system_quotient(0.4);
		CHECK(main.ai_system_quotient() == 0.4);
		CHECK(main.reset());
		CHECK_FALSE(main.is_initialised());
		CHECK(main.sai(1) == nullptr);
		CHECK_FALSE(main.sai_enabled());
		CHECK(main.ai_system_quotient() == 0.2);
		CHECK(main.component_quotient() == 0.25);
		CHECK(main.bot_manager().bot_count() == 0);
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

	TEST_CASE("BotPlacementManager: the console registers a placement per team and spawnBot spends one as shipped") {
		AiSettings settings;
		settings.max_bots = 1;
		World w(settings);
		BotManager &bots = w.ai.main().bot_manager();
		BotPlacementManager placements(bots);
		AiConsole console;
		register_placement_commands(console, placements);
		CHECK(console.execute("addPlacement alpha 1/2/3 0/90/0 rifle 2 crate").text == "1");
		CHECK(console.execute("addPlacement beta 1/2 0/0/0 rifle 2 crate").text == "bad argument: 1/2");
		CHECK_FALSE(console.execute("addPlacement gamma 0/0/0 0/0/0 rifle 3 crate").text == "1");
		REQUIRE(placements.placements(2).size() == 1);
		const BotPlacement &placement = placements.placements(2).front();
		CHECK(placement.name == "alpha");
		CHECK(placement.position.y == 2.0f);
		CHECK(placement.rotation.y == 90.0f);
		CHECK(placement.fourth_argument == "rifle");
		CHECK(placement.sixth_argument == "crate");
		CHECK(console.execute("spawnBot 1").text == "0");
		CHECK(console.execute("spawnBot 2").text == "0");
		CHECK(placements.placements(2).empty());
		CHECK(placements.add_placement("delta", {}, {}, "", 2, ""));
		REQUIRE(bots.add_unspawned_bot(10, 2, nullptr));
		CHECK(console.execute("spawnBot 2").text == "1");
		CHECK(bots.bot_from_player_id(10)->name() == "FIXME");
	}
}
