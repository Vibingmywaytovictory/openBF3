#include <doctest/doctest.h>

#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/path/cluster_graph.h"
#include "bf3/ai/path/map_properties.h"
#include "bf3/ai/squad/squad_manager.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace bf3;
using namespace bf3::ai;

namespace {

struct Soldier final : AiObject, AiObjectMobile, AiObjectPhysical, AiObjectUnit {
	std::string label = "soldier";
	Vec3 where{};
	Vec3 velocity{};
	Vec3 spin{};
	Transform transform;
	float top_speed = 7.0f;
	float reach = 0.4f;
	int team = 1;
	bool with_unit = false;
	bool occupied = false;
	bool primary = true;
	Soldier *owner = nullptr;
	std::vector<Soldier *> seats;
	int battle[kBattleStrengthCount] = {};
	int strategic[kStrategicStrengthCount] = { 4, 8 };
	int strength_class = 0;
	float hp = 100.0f;
	int kit = 0;
	std::map<int, uint32_t> handles;

	const std::string &name() const override { return label; }
	const std::string &ai_name() const override { return label; }
	const std::string &object_type_name() const override { return label; }
	AiObjectArmament *armament() const override { return nullptr; }
	AiObjectCover *cover() const override { return nullptr; }
	AiObjectMobile *mobile() const override { return const_cast<Soldier *>(this); }
	AiObjectPhysical *physical() const override { return const_cast<Soldier *>(this); }
	AiObjectUnit *unit() const override { return with_unit ? const_cast<Soldier *>(this) : nullptr; }
	uint32_t information(int p_side) const override {
		const auto found = handles.find(p_side);
		return found != handles.end() ? found->second : kNoHandle;
	}
	const Vec3 &position() const override { return where; }
	const Transform &transformation() const override { return transform; }
	uint32_t object_id() const override { return 1; }
	SoldierPose soldier_pose() const override { return SoldierPose::Standing; }
	int kit_type() const override { return kit; }
	bool is_primary() const override { return primary; }
	AiObject *primary_object() const override { return owner; }
	AiObject *secondary_object(int &r_iterator) const override {
		++r_iterator;
		return size_t(r_iterator) < seats.size() ? seats[size_t(r_iterator)] : nullptr;
	}
	AiObject *first_secondary_object(int &r_iterator) const override {
		r_iterator = 0;
		return seats.empty() ? nullptr : seats[0];
	}
	int first_secondary_object_iterator() const override { return -1; }
	int side() const override { return team; }
	bool object_spawner_holding() const override { return false; }
	AiObject *object_spawner() const override { return nullptr; }
	bool is_root_object() const override { return true; }
	int player_id() const override { return 0; }

	const Vec3 &speed() const override { return velocity; }
	const Vec3 &rotational_speed() const override { return spin; }
	int vehicle_number() const override { return 0; }
	float max_speed() const override { return top_speed; }
	float turn_radius() const override { return 0.0f; }
	float drag() const override { return 0.0f; }
	float cover_search_radius() const override { return 20.0f; }
	bool is_turnable() const override { return true; }
	float sound_sphere_radius() const override { return 10.0f; }
	float sound_sphere_radius_min() const override { return 5.0f; }
	float sound_sphere_radius_max() const override { return 30.0f; }
	float hearing_probability_max() const override { return 1.0f; }
	float hearing_probability_min() const override { return 0.0f; }
	float avoid_collision_look_ahead() const override { return 2.0f; }
	AiObject *sector() const override { return nullptr; }
	int last_passed_portal() const override { return -1; }
	float sprint_stamina() const override { return 1.0f; }

	float health() const override { return hp; }
	float max_health() const override { return 100.0f; }
	float mass() const override { return 80.0f; }
	float volume() const override { return 0.1f; }
	float armour(const Vec3 &) const override { return 1.0f; }
	float best_armour() const override { return 1.0f; }
	float radius() const override { return reach; }
	Vec3 sphere_local_offset() const override { return {}; }
	float smallest_radius() const override { return reach; }
	void sub_spheres_2d(std::vector<Vec2> &) const override {}
	int strength_type() const override { return strength_class; }
	bool is_touching_terrain(Vec3 &) const override { return true; }
	bool is_in_air() const override { return false; }
	bool is_in_water() const override { return false; }
	int material() const override { return 1; }
	float man_down_time_left() const override { return 0.0f; }
	float max_man_down_time() const override { return 10.0f; }
	bool man_down() const override { return false; }

	int max_ammo() const override { return 0; }
	const int *battle_strengths() const override { return battle; }
	const int *strategic_strengths() const override { return strategic; }
	int equipment_type() const override { return 0; }
	bool use_no_pathfinding_to_get_to_object() const override { return false; }
	bool has_exposed_soldier() const override { return false; }
	bool always_target_soldier() const override { return false; }
	bool is_occupied() const override { return occupied; }
	float cull_distance() const override { return 100.0f; }
	float cull_distance_sqr() const override { return 10000.0f; }
	float entry_radius() const override { return 1.0f; }
	AiObject *occupier() const override { return nullptr; }
	AiObject *occupied_object() const override { return nullptr; }
	sim::Tick last_player_possession_tick() const override { return 0; }
	int team_lock() const override { return 0; }
	bool must_borrow_armament(const AiObject *) const override { return false; }
	int occupying_player_id() const override { return -1; }
	void set_missile_lock_status(int) override {}
	int missile_lock_status() const override { return 0; }
	const Vec3 &entry_position_of_non_mobile() const override { return where; }
};

struct Knowledge {
	InformationManager information;
	InformationFactory factory{ make() };

	KnowledgeContext make() {
		KnowledgeContext context;
		context.information = &information;
		return context;
	}

	uint32_t record(Soldier &r_object, int p_side, uint32_t p_types = 1u) {
		const uint32_t handle = factory.request_information_real(kNoHandle, &r_object, p_types, 0.5f, 0, 30);
		r_object.handles[p_side] = handle;
		factory.request_information_plug_in(handle, PlugInRequest::Physical, true);
		factory.request_information_plug_in(handle, PlugInRequest::Mobile, true);
		if (r_object.with_unit) {
			factory.request_information_plug_in(handle, PlugInRequest::Unit, true);
		}
		return handle;
	}
};

struct Player final : SquadPlayer {
	int player_id = 0;
	bool ai = true;
	bool alive = true;
	bool down = false;
	int team = 1;
	int kit = 0;
	Soldier body;
	int id() const override { return player_id; }
	bool is_ai() const override { return ai; }
	bool is_alive() const override { return alive; }
	bool is_man_down() const override { return down; }
	int side() const override { return team; }
	int spawn_group() const override { return 0; }
	int spawn_kit() const override { return kit; }
	AiObject *soldier() const override { return const_cast<Soldier *>(&body); }
	AiObject *current_vehicle() const override { return alive ? const_cast<Soldier *>(&body) : nullptr; }
};

struct FakeBot final : SquadBot {
	SquadMember *member = nullptr;
	int squad = 0;
	SquadGoal goal;
	int goals = 0;
	std::function<bool(int)> on_join;
	void set_squad_member(SquadMember *p_member) override { member = p_member; }
	bool join_squad(int p_squad) override {
		squad = p_squad;
		return on_join(p_squad);
	}
	void set_squad_goal(const SquadGoal &p_goal) override {
		goal = p_goal;
		++goals;
	}
};

struct World final : SquadWorld, SquadService, SquadStrategy {
	int squads = 5;
	int members = 4;
	sim::Tick tick = 100;
	std::map<int, std::unique_ptr<Player>> players;
	std::map<int, std::unique_ptr<FakeBot>> bots;
	std::map<int, std::vector<int>> rosters;
	mutable int roster_reads = 0;
	bool commander = false;
	float invalid_x = 1.0e9f;

	Player &add(int p_id, bool p_ai, Vec3 p_where) {
		auto player = std::make_unique<Player>();
		player->player_id = p_id;
		player->ai = p_ai;
		player->body.where = p_where;
		Player &added = *player;
		players[p_id] = std::move(player);
		if (p_ai) {
			auto bot = std::make_unique<FakeBot>();
			bot->on_join = [this, &added](int p_squad) { return join_squad(p_squad, added); };
			bots[p_id] = std::move(bot);
		}
		return added;
	}

	int max_number_of_squads() const override { return squads; }
	int max_number_of_squad_members() const override { return members; }
	sim::Tick game_tick() const override { return tick; }
	SquadPlayer *player(int p_id) const override {
		const auto found = players.find(p_id);
		return found != players.end() ? found->second.get() : nullptr;
	}
	std::vector<SquadPlayer *> all_players() const override {
		++roster_reads;
		std::vector<SquadPlayer *> out;
		for (const auto &[id, player] : players) {
			out.push_back(player.get());
		}
		return out;
	}
	SquadBot *bot_from_player_id(int p_id) const override {
		const auto found = bots.find(p_id);
		return found != bots.end() ? found->second.get() : nullptr;
	}
	bool is_valid_position(const Vec3 &p_position) const override { return p_position.x < invalid_x; }

	bool join_squad(int p_squad, const SquadPlayer &p_player) override {
		rosters[p_squad].push_back(p_player.id());
		return true;
	}
	bool exit_squad(int p_squad, const SquadPlayer &p_player) override {
		std::vector<int> &roster = rosters[p_squad];
		roster.erase(std::remove(roster.begin(), roster.end(), p_player.id()), roster.end());
		return true;
	}
	bool is_squad_leader(int p_squad, const SquadPlayer &p_player) const override {
		const auto found = rosters.find(p_squad);
		return found != rosters.end() && !found->second.empty() && found->second.front() == p_player.id();
	}
	void request_new_commander_order(int, const SquadPlayer &) override {}

	bool is_player_commander(int) const override { return commander; }
	bool own_all_takeable_control_points(int) const override { return false; }
	bool have_any_owned_and_takeable_control_points(int) const override { return false; }

	SquadServices services() {
		SquadServices out;
		out.world = this;
		out.squads = this;
		out.strategy = this;
		return out;
	}
};

struct Area final : SquadArea {
	int area_index = 3;
	int owner_status = 1;
	int index() const override { return area_index; }
	int status(int) const override { return owner_status; }
	bool is_takeable() const override { return true; }
	bool control_point(Vec3 &r_position, float &r_radius) const override {
		r_position = { 50.0f, 0.0f, 50.0f };
		r_radius = 10.0f;
		return true;
	}
};

void join_bots(World &r_world, SquadCoordinator &r_squad, std::initializer_list<int> p_ids, Vec3 p_where) {
	for (const int id : p_ids) {
		Player &player = r_world.add(id, true, p_where);
		r_world.join_squad(r_squad.squad_id() + 1, player);
		r_squad.add_squad_member(player);
		r_squad.update_squad_leader();
	}
}

}

TEST_SUITE("ai-squad") {
	TEST_CASE("SquadFormation: a line of 4 is 2-0-1-3 at 1 m, spread by the member radius, rotated by the forward") {
		World world;
		SquadCoordinator squad(world.services(), 0, 0);
		const SquadFormation &formation = squad.formation();
		REQUIRE(formation.max_members() == 4);
		CHECK(formation.direction().x == 1.0f);
		CHECK(formation.slot(1).offset.x == 1.0f);
		CHECK(formation.slot(1).reference_slot == 0);
		CHECK(formation.slot(2).offset.x == -1.0f);
		CHECK(formation.slot(2).reference_slot == 0);
		CHECK(formation.slot(3).offset.x == 1.0f);
		CHECK(formation.slot(3).reference_slot == 1);

		join_bots(world, squad, { 10, 11, 12, 13 }, { 10.0f, 0.0f, 20.0f });
		REQUIRE(squad.number_of_squad_members() == 4);
		REQUIRE(squad.squad_leader() != nullptr);
		CHECK(squad.squad_leader()->player_id() == 10);
		for (int slot = 0; slot < 4; ++slot) {
			CHECK(formation.slot(slot).occupied);
			CHECK(formation.slot(slot).member_index == slot);
		}
		for (const auto &[id, bot] : world.bots) {
			CHECK(bot->member != nullptr);
		}

		world.players[11]->body.where = { 11.0f, 0.0f, 20.0f };
		world.players[12]->body.where = { 9.0f, 0.0f, 20.0f };
		world.players[13]->body.where = { 12.0f, 0.0f, 20.0f };
		for (int i = 0; i < 3; ++i) {
			squad.update(++world.tick);
		}
		CHECK(formation.slot_zero_position().x == 10.0f);
		CHECK(formation.slot_position(1).x == doctest::Approx(11.0f));
		CHECK(formation.slot_position(2).x == doctest::Approx(9.0f));
		CHECK(formation.slot_position(3).x == doctest::Approx(12.0f));
		CHECK(formation.slot_position(3).z == doctest::Approx(20.0f));
		const FakeBot &follower = *world.bots[13];
		CHECK(follower.goal.kind == SquadGoalKind::Slot);
		CHECK(follower.goal.position.x == doctest::Approx(12.0f));
		const FakeBot &anchor = *world.bots[10];
		CHECK(anchor.goal.kind == SquadGoalKind::None);
		CHECK(anchor.goal.max_speed == doctest::Approx(0.9f * 7.0f));
		CHECK_FALSE(anchor.goal.wait_for_members);

		world.players[10]->body.velocity = { 3.0f, 0.0f, 0.0f };
		world.players[10]->body.where = { 10.1f, 0.0f, 20.0f };
		squad.update(world.tick);
		CHECK(formation.forward().x == doctest::Approx(1.0f));
		CHECK(formation.forward().y == doctest::Approx(0.0f));
		world.players[10]->body.velocity = {};
		for (int i = 0; i < 6; ++i) {
			squad.update(++world.tick);
		}
		CHECK(formation.slot_position(1).z == doctest::Approx(20.0f - 1.0f));
		CHECK(formation.slot_position(2).z == doctest::Approx(20.0f + 1.0f));
		CHECK(formation.slot_position(3).z == doctest::Approx(20.0f - 2.0f));

		SquadCoordinator wide(world.services(), 0, 1);
		world.join_squad(2, *world.players[11]);
		world.players[11]->body.reach = 1.5f;
		wide.add_squad_member(*world.players[11]);
		Player &second = world.add(14, true, { 10.0f, 0.0f, 20.0f });
		wide.add_squad_member(second);
		Player &third = world.add(15, true, { 10.0f, 0.0f, 20.0f });
		wide.add_squad_member(third);
		wide.formation().set_line_formation();
		CHECK(wide.formation().slot(0).offset.x == 0.0f);
		CHECK(wide.formation().slot(1).offset.x == doctest::Approx(1.0f));
		CHECK(wide.formation().slot(2).offset.x == doctest::Approx(-1.0f));
		CHECK(wide.formation().slot(0).member_index == 0);
		CHECK(wide.formation().slot(3).offset.x == doctest::Approx(1.0f));
		wide.formation().leave_formation(0);
		wide.formation().join_formation(0);
		REQUIRE(wide.formation().slot_of_member(0) == 2);
		wide.formation().set_line_formation();
		CHECK(wide.formation().slot(2).offset.x == doctest::Approx(-3.0f));
	}

	TEST_CASE("SquadFormation: a moving anchor lays crumbs every 6 m and the followers trail them nearest first") {
		World world;
		SquadCoordinator squad(world.services(), 0, 0);
		join_bots(world, squad, { 1, 2, 3 }, { 0.0f, 0.0f, 0.0f });
		world.players[1]->body.where = { 1.0f, 0.0f, 0.0f };
		world.players[2]->body.where = { 1.0f, 0.0f, -3.0f };
		world.players[3]->body.where = { 1.0f, 0.0f, -1.0f };
		squad.update(++world.tick);
		const SquadFormation &formation = squad.formation();
		CHECK_FALSE(formation.trailing());

		world.players[1]->body.velocity = { 0.0f, 0.0f, 4.0f };
		for (int step = 1; step <= 3; ++step) {
			world.players[1]->body.where = { 1.0f, 0.0f, 7.0f * float(step) };
			squad.update(++world.tick);
		}
		CHECK(formation.trailing());
		CHECK(formation.slot_target(0).first);
		CHECK(formation.slot_target(0).second.z == doctest::Approx(14.0f));
		CHECK(formation.slot_target(1).first);
		CHECK(formation.slot_target(1).second.z == doctest::Approx(7.0f));
		CHECK(formation.trailing_position().z == doctest::Approx(21.0f));
		squad.update(++world.tick);
		CHECK(formation.trailing_position().z == doctest::Approx(21.0f));

		const int slot_of_3 = formation.slot_of_member(2);
		const int slot_of_2 = formation.slot_of_member(1);
		REQUIRE(slot_of_3 > 0);
		REQUIRE(slot_of_2 > 0);
		CHECK(formation.slot(slot_of_3).trail_index == 0);
		CHECK(formation.slot(slot_of_2).trail_index == 1);
		CHECK(world.bots[3]->goal.kind == SquadGoalKind::Slot);
		CHECK(world.bots[3]->goal.position.z == doctest::Approx(14.0f));
		CHECK(world.bots[2]->goal.position.z == doctest::Approx(7.0f));

		world.players[1]->body.velocity = {};
		squad.update(++world.tick);
		CHECK_FALSE(formation.trailing());
		CHECK_FALSE(formation.slot_target(0).first);
	}

	TEST_CASE("SquadManager: each coordinator is visited once per 2xN updates, sides alternating") {
		World world;
		world.squads = 4;
		SquadManager manager(world.services());
		REQUIRE(manager.coordinator_count(1) == 3);
		REQUIRE(manager.coordinator_count(2) == 3);
		std::vector<sim::Tick> visits;
		for (sim::Tick tick = 1; tick <= 6; ++tick) {
			manager.update(tick);
		}
		for (int side = 1; side <= 2; ++side) {
			for (int squad = 1; squad <= 3; ++squad) {
				visits.push_back(manager.coordinator(side, squad).last_update());
			}
		}
		CHECK(visits == std::vector<sim::Tick>{ 1, 3, 5, 2, 4, 6 });
		CHECK(world.roster_reads == 1);
		for (sim::Tick tick = 7; tick <= 12; ++tick) {
			manager.update(tick);
		}
		CHECK(manager.coordinator(1, 1).last_update() == 7);
		CHECK(manager.coordinator(2, 3).last_update() == 12);
		CHECK(world.roster_reads == 2);
	}

	TEST_CASE("SquadManager: joins fill the least-populated squad, a third member splits, leaders hand over") {
		World world;
		SquadManager manager(world.services());
		for (int id = 1; id <= 4; ++id) {
			world.add(id, true, { float(id), 0.0f, 0.0f });
			manager.add_squad_member(id, 1);
		}
		CHECK(manager.coordinator(1, 1).number_of_squad_members() == 3);
		CHECK(manager.coordinator(1, 2).number_of_squad_members() == 1);
		REQUIRE(manager.squad_from_player(3, 1) == &manager.coordinator(1, 2));
		CHECK(world.bots[3]->squad == 2);
		CHECK(world.bots[4]->squad == 1);
		CHECK(manager.squad_member(1, 1)->is_squad_leader());

		world.add(20, false, { 0.0f, 0.0f, 0.0f });
		manager.player_joins_squad(20, 2, 1);
		manager.player_joins_squad(20, 2, 1);
		CHECK(manager.coordinator(1, 2).number_of_squad_members() == 2);
		world.join_squad(2, *world.players[20]);
		manager.player_joins_squad(20, 3, 1);
		CHECK(manager.coordinator(1, 3).number_of_squad_members() == 0);

		SquadCoordinator &first = manager.coordinator(1, 1);
		REQUIRE(first.squad_leader()->player_id() == 1);
		world.exit_squad(1, *world.players[1]);
		CHECK(manager.remove_squad_member(1, 1));
		CHECK(first.number_of_squad_members() == 2);
		REQUIRE(first.squad_leader() != nullptr);
		CHECK(first.squad_leader()->player_id() == 2);
		CHECK(first.squad_member(0)->member_index() == 0);
		CHECK(first.squad_member(1)->member_index() == 1);
		CHECK(first.formation().slot_of_member(0) == 0);
		CHECK(first.formation().slot_of_member(1) != -1);
		CHECK(world.bots[1]->member == nullptr);
		CHECK_FALSE(manager.remove_squad_member(1, 1));
	}

	TEST_CASE("SquadManager: a human off the navigable floor is recorded once and cleared when valid again") {
		World world;
		world.squads = 2;
		SquadManager manager(world.services());
		Player &human = world.add(7, false, { 5.0f, 0.0f, 0.0f });
		world.invalid_x = 4.0f;
		manager.update(10);
		manager.update(11);
		REQUIRE(manager.player_valid_positions().count(7) == 1);
		CHECK(manager.player_valid_positions().at(7) == 11);
		manager.update(12);
		manager.update(13);
		CHECK(manager.player_valid_positions().at(7) == 11);
		human.body.where.x = 1.0f;
		manager.update(14);
		manager.update(15);
		CHECK(manager.player_valid_positions().empty());
	}

	TEST_CASE("SquadCoordinator: orders, fire records, repair requests and strategic strength") {
		World world;
		InformationManager information;
		SquadServices services = world.services();
		services.information = &information;
		SquadCoordinator squad(services, 0, 0);
		join_bots(world, squad, { 1, 2 }, { 0.0f, 0.0f, 0.0f });

		Area area;
		squad.assign_squad(&area, { 50.0f, 0.0f, 50.0f }, SquadOrder::Attack, SquadMissionStatus::Accepted);
		CHECK(squad.is_squad_assigned_to_area(3));
		squad.update(++world.tick);
		CHECK(world.bots[1]->goal.kind == SquadGoalKind::SquadTarget);
		CHECK(world.bots[1]->goal.position.x == 50.0f);
		CHECK_FALSE(squad.is_squad_taking_flag());
		world.players[2]->body.where = { 45.0f, 0.0f, 45.0f };
		squad.update(++world.tick);
		CHECK(squad.is_squad_taking_flag());
		area.owner_status = 0;
		squad.update(++world.tick);
		CHECK_FALSE(squad.is_squad_assigned_to_any_area());
		CHECK(squad.mission_status() == SquadMissionStatus::None);
		CHECK(squad.order() == SquadOrder::None);

		squad.shots_fired(2);
		squad.shots_fired(2);
		REQUIRE(squad.fire_records().size() == 1);
		squad.update(++world.tick);
		CHECK(squad.fire_records().size() == 1);
		world.players[2]->body.where = { 51.0f, 0.0f, 45.0f };
		squad.update(++world.tick);
		CHECK(squad.fire_records().empty());
		CHECK(squad.squad_member(1)->needs_path());
		squad.shots_fired(2);
		world.tick += kSquadFireRecordTicks + 1;
		squad.update(world.tick);
		CHECK(squad.fire_records().empty());

		squad.request_repair({ 1.0f, 2.0f, 3.0f }, world.tick);
		squad.confirm_repair_request();
		squad.update(world.tick + kSquadRepairRequestTicks);
		CHECK(squad.is_repair_requested());
		CHECK(squad.have_confirmed_repair_request());
		squad.update(world.tick + kSquadRepairRequestTicks + 1);
		CHECK_FALSE(squad.is_repair_requested());
		CHECK_FALSE(squad.have_confirmed_repair_request());

		CHECK(squad.strategic_strength(0) == 0);
		Knowledge knowledge;
		services.information = &knowledge.information;
		SquadCoordinator strong(services, 0, 1);
		for (const int id : { 1, 2 }) {
			Soldier &body = world.players[id]->body;
			body.with_unit = true;
			knowledge.record(body, 1);
			strong.add_squad_member(*world.players[id]);
		}
		CHECK(strong.strategic_strength(0) == 8);
		CHECK(strong.strategic_strength(1) == 16);
		world.players[2]->down = true;
		CHECK(strong.strategic_strength(1) == 8);
		CHECK(strong.strategic_strength(kStrategicStrengthCount) == 0);
	}

	TEST_CASE("MaterialManager and MapProperties: OpenBF2's default costs, material names and slope limits") {
		MaterialManager materials;
		CHECK_FALSE(materials.is_valid_material(kMaterialInvalid));
		CHECK(materials.material_cost(kMaterialInvalid) == -1.0f);
		CHECK(materials.is_valid_material(kMaterialGround));
		CHECK(materials.material_cost(kMaterialRoad) == 1.0f);
		CHECK(materials.material_cost(kMaterialDangerObject) == 1000.0f);
		CHECK(materials.material_cost(kMaterialOtherDangerObject) == 1.0f);
		CHECK(materials.material_cost(kMaterialBothDangerObject) == 1000.0f);
		CHECK_FALSE(materials.is_valid_material(kMaterialCount));
		materials.set_material_cost(kMaterialShallows, -1.0f);
		CHECK_FALSE(materials.is_valid_material(kMaterialShallows));
		CHECK(parse_material("deepwater") == kMaterialDeepWater);
		CHECK(parse_material("BothDangerObject") == kMaterialBothDangerObject);
		CHECK(parse_material("NumberOfMaterials") == kMaterialCount);
		CHECK_FALSE(parse_material("Lava").has_value());

		MapProperties map;
		CHECK(map.head_clearance() == 2.0f);
		CHECK(map.allowed_height_diff() == 2.5f);
		CHECK(map.is_valid_ground_mesh({ 0.0f, 0.01f, 1.0f }, kMaterialGround));
		map.set_max_slope(0.5f);
		CHECK(map.max_slope() == doctest::Approx(0.5f));
		CHECK(map.is_valid_ground_mesh({ 0.0f, std::cos(0.4f), std::sin(0.4f) }, kMaterialGround));
		CHECK_FALSE(map.is_valid_ground_mesh({ 0.0f, std::cos(0.6f), std::sin(0.6f) }, kMaterialGround));
	}

	TEST_CASE("ClusterGraph: the cheapest cluster path weighs portal costs by the entered cluster's material") {
		std::vector<Cluster> clusters(5);
		clusters[0] = { 0, 1, 0, 2, kMaterialGround };
		clusters[1] = { 1, 1, 2, 2, kMaterialGround };
		clusters[2] = { 2, 1, 4, 2, kMaterialDangerObject };
		clusters[3] = { 3, 1, 6, 2, kMaterialRoad };
		clusters[4] = { 4, 1, 8, 0, kMaterialGround };
		std::vector<ClusterPortal> portals = {
			{ { 1, 0, 0 }, 1 },
			{ { 0, 0, 1 }, 2 },
			{ { 1, 0, 0 }, 0 },
			{ { 2, 0, 0 }, 3 },
			{ { 0, 0, 1 }, 0 },
			{ { 0, 0, 2 }, 3 },
			{ { 2, 0, 0 }, 1 },
			{ { 0, 0, 2 }, 2 },
		};
		std::vector<float> costs = { 10.0f, 4.0f, 10.0f, 10.0f, 4.0f, 4.0f, 10.0f, 4.0f };
		ClusterGraph graph;
		CHECK_FALSE(graph.assign(clusters, { 0, 1, 2, 3, 4 }, portals, { 1.0f }).ok());
		std::vector<ClusterPortal> stray = portals;
		stray[3].to_cluster = 9;
		CHECK_FALSE(graph.assign(clusters, { 0, 1, 2, 3, 4 }, stray, costs).ok());
		CHECK_FALSE(graph.assign(clusters, { 0, 1, 2 }, portals, costs).ok());
		REQUIRE(graph.assign(clusters, { 0, 1, 2, 3, 4 }, portals, costs).ok());
		CHECK(graph.number_of_clusters() == 5);
		CHECK(graph.cluster_portal(1, 1).to_cluster == 3);
		CHECK(graph.portal_cost(2, 1) == 4.0f);

		MaterialManager materials;
		ClusterNodeManager nodes;
		std::vector<uint32_t> path;
		CHECK(graph.shortest_path(0, 3, materials, nodes, path) == doctest::Approx(20.0f));
		CHECK(path == std::vector<uint32_t>{ 0, 1, 3 });
		materials.set_material_cost(kMaterialDangerObject, 1.0f);
		CHECK(graph.shortest_path(0, 3, materials, nodes, path) == doctest::Approx(8.0f));
		CHECK(path == std::vector<uint32_t>{ 0, 2, 3 });
		materials.set_material_cost(kMaterialRoad, 3.0f);
		CHECK(graph.shortest_path(0, 3, materials, nodes, path) == doctest::Approx(16.0f));
		CHECK(path == std::vector<uint32_t>{ 0, 2, 3 });
		CHECK(graph.shortest_path(3, 0, materials, nodes, path) == doctest::Approx(8.0f));
		CHECK(path == std::vector<uint32_t>{ 3, 2, 0 });
		CHECK(graph.shortest_path(0, 4, materials, nodes, path) == -1.0f);
		CHECK(path.empty());
		CHECK(graph.shortest_path(2, 2, materials, nodes, path) == 0.0f);
		CHECK(path == std::vector<uint32_t>{ 2 });
		materials.set_material_cost(kMaterialDangerObject, -1.0f);
		CHECK(graph.shortest_path(0, 3, materials, nodes, path) == doctest::Approx(40.0f));
		CHECK(path == std::vector<uint32_t>{ 0, 1, 3 });
		CHECK(graph.shortest_path(0, 2, materials, nodes, path) == -1.0f);
		graph.clean();
		CHECK(graph.number_of_clusters() == 0);
	}

	TEST_CASE("ClusterNodeManager: open and closed records hash by cluster and recycle through the free list") {
		ClusterNodeManager nodes;
		const int32_t a = nodes.new_oc_data(7);
		const int32_t b = nodes.new_oc_data(7 + int(ClusterNodeManager::kOCDataBuckets));
		const int32_t c = nodes.new_oc_data(8);
		CHECK(nodes.live_count() == 3);
		CHECK(nodes.find(7) == a);
		CHECK(nodes.find(7 + int(ClusterNodeManager::kOCDataBuckets)) == b);
		CHECK(nodes.find(8) == c);
		CHECK(nodes.find(9) == -1);
		nodes.recycle_oc_data(a);
		CHECK(nodes.find(7) == -1);
		CHECK(nodes.find(7 + int(ClusterNodeManager::kOCDataBuckets)) == b);
		CHECK(nodes.live_count() == 2);
		CHECK(nodes.new_oc_data(11) == a);
		CHECK(nodes.capacity() == 3);
		nodes.reset();
		CHECK(nodes.live_count() == 0);
		CHECK(nodes.find(8) == -1);
		nodes.new_oc_data(1);
		nodes.new_oc_data(2);
		nodes.new_oc_data(3);
		CHECK(nodes.capacity() == 3);
	}
}
