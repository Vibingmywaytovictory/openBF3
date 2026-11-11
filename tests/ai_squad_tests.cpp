#include <doctest/doctest.h>

#include "bf3/ai/behaviour/curves.h"
#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/information_grid.h"
#include "bf3/ai/main/ai_random.h"
#include "bf3/ai/path/cluster_graph.h"
#include "bf3/ai/path/map_properties.h"
#include "bf3/ai/squad/squad_manager.h"
#include "bf3/ai/strategic/bot_spawner.h"
#include "bf3/ai/strategic/strategy.h"
#include "bf3/ai/vehicle/aerial_height_map.h"
#include "bf3/ai/vehicle/landing_zone_manager.h"
#include "bf3/ai/vehicle/vehicle_occupancy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <list>
#include <map>
#include <memory>
#include <set>
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

struct Objective final : SpawnObjective {
	int area = 0;
	Vec2 where{};
	Vec2 middle{};
	float value = 1.0f;
	int index() const override { return area; }
	Vec2 position(int) const override { return where; }
	float strategic_value(int) const override { return value; }
	Vec2 centre() const override { return middle; }
};

struct HeldGroup final : SquadPlayer {
	const Player *inner = nullptr;
	int id() const override { return inner->id(); }
	bool is_ai() const override { return true; }
	bool is_alive() const override { return true; }
	bool is_man_down() const override { return false; }
	int side() const override { return 1; }
	int spawn_group() const override { return 4; }
	int spawn_kit() const override { return 0; }
	AiObject *soldier() const override { return inner->soldier(); }
	AiObject *current_vehicle() const override { return inner->current_vehicle(); }
};

struct Spawnee final : SpawnBot {
	uint32_t id = 0;
	int team = 1;
	const SquadPlayer *body = nullptr;
	const SquadMember *member = nullptr;
	int point = -1;
	int heading_to = -1;
	int target_area = -2;
	int target_group = -2;
	int target_kit = -2;
	uint32_t bot_id() const override { return id; }
	int side() const override { return team; }
	const SquadPlayer &player() const override { return *body; }
	int spawn_point() const override { return point; }
	const SquadMember *squad_member() const override { return member; }
	int spawn_to_strategic_area_id() const override { return heading_to; }
	void set_spawn_target(int p_group, int p_kit) override {
		target_group = p_group;
		target_kit = p_kit;
	}
	void set_spawn_target(int p_area, int p_group, int p_kit) override {
		target_area = p_area;
		target_group = p_group;
		target_kit = p_kit;
	}
};

struct SpawnField final : SpawnGame {
	std::vector<int> groups;
	std::map<int, Vec3> positions;
	std::map<int, int> control_points;
	std::vector<int> invalid;
	std::vector<int> kits = { 0, 1, 2, 3, 4, 5, 6 };
	std::map<int, const Objective *> objectives;
	std::vector<int> spawns_added;
	std::map<int, std::pair<int, int>> dead;
	std::map<uint32_t, const Spawnee *> roster;
	const AIInformationGrid *grid = nullptr;

	const std::vector<int> &spawn_point_list(int) const override { return groups; }
	Vec3 spawn_point_position(int p_group) const override { return positions.at(p_group); }
	bool is_eligible_spawnpoint(int, int p_group) const override { return positions.count(p_group) != 0; }
	int control_point_id(int p_group) const override {
		const auto found = control_points.find(p_group);
		return found != control_points.end() ? found->second : -1;
	}
	const std::vector<int> &kit_templates() const override { return kits; }
	bool is_valid_position(const Vec3 &p_position) const override {
		for (const int group : invalid) {
			if (positions.at(group) == p_position) {
				return false;
			}
		}
		return true;
	}
	const SpawnObjective *strategic_object(int p_id) const override {
		const auto found = objectives.find(p_id);
		return found != objectives.end() ? found->second : nullptr;
	}
	const SpawnObjective *closest_object(const Vec3 &p_position, int) const override {
		const SpawnObjective *best = nullptr;
		float nearest = 1.0e30f;
		for (const auto &[id, objective] : objectives) {
			const float dx = objective->where.x - p_position.x;
			const float dz = objective->where.y - p_position.z;
			if (dx * dx + dz * dz < nearest) {
				nearest = dx * dx + dz * dz;
				best = objective;
			}
		}
		return best;
	}
	void add_spawn_to_point(int, int p_group) override { spawns_added.push_back(p_group); }
	const std::map<int, std::pair<int, int>> &dead_bots(int) const override { return dead; }
	const SpawnBot *bot_from_id(uint32_t p_id) const override {
		const auto found = roster.find(p_id);
		return found != roster.end() ? found->second : nullptr;
	}
	const AIInformationGrid *information_grid(int) const override { return grid; }
};

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

	TEST_CASE("VehicleOccupancy: a vehicle is occupied when its primary or any seat is, resolved from a seat") {
		Knowledge knowledge;
		Soldier vehicle;
		Soldier driver;
		Soldier gunner;
		for (Soldier *part : { &vehicle, &driver, &gunner }) {
			part->with_unit = true;
		}
		vehicle.seats = { &driver, &gunner };
		driver.primary = false;
		driver.owner = &vehicle;
		const uint32_t hull = knowledge.record(vehicle, 1);
		const uint32_t seat = knowledge.record(driver, 1);
		knowledge.record(gunner, 1);
		const InformationManager &information = knowledge.information;
		CHECK_FALSE(vehicle_occupied(information, hull, 1));
		gunner.occupied = true;
		CHECK(vehicle_occupied(information, hull, 1));
		CHECK(vehicle_occupied(information, seat, 1));
		gunner.occupied = false;
		vehicle.occupied = true;
		CHECK(vehicle_occupied(information, seat, 1));
		CHECK_FALSE(vehicle_occupied(information, kNoHandle, 1));
		CHECK_FALSE(vehicle_occupied(information, 0, 1));
		Soldier wreck;
		const uint32_t bare = knowledge.record(wreck, 1);
		CHECK_FALSE(vehicle_occupied(information, bare, 1));
	}

	TEST_CASE("AerialHeightMap: every 16 m cell holds the ceiling of the field's maximum and survives a round trip") {
		struct Field final : AerialHeightSource {
			float height(float p_x, float p_z) const override {
				const float spike = (p_x == 5.0f && p_z == 21.0f) ? 50.2f : 0.0f;
				return 0.25f * (p_x + 32.0f) + 0.1f * p_z + spike;
			}
		};
		AerialHeightMap map;
		CHECK(map.empty());
		CHECK(map.ground_height(0.0f, 0.0f) == kMissingGroundHeight);
		map.create_height_map({ -32.0f, 0.0f, 64.0f, 48.0f }, Field{}, 17);
		REQUIRE(map.width() == 4);
		REQUIRE(map.height() == 3);
		CHECK(map.cell_size() == 16.0f);
		for (int cz = 0; cz < 3; ++cz) {
			for (int cx = 0; cx < 4; ++cx) {
				float highest = 0.25f * (16.0f * float(cx) + 16.0f) + 0.1f * (16.0f * float(cz) + 16.0f);
				if (cx == 2 && cz == 1) {
					highest = std::max(highest, 0.25f * 37.0f + 2.1f + 50.2f);
				}
				const float x = -32.0f + 16.0f * float(cx) + 8.0f;
				const float z = 16.0f * float(cz) + 8.0f;
				CHECK(map.ground_height(x, z) == std::ceil(highest));
			}
		}
		CHECK(map.ground_height(5.0f, 21.0f) == 62.0f);
		CHECK(map.altitude({ 5.0f, 100.0f, 21.0f }) == 38.0f);
		CHECK(map.ground_height(-40.0f, 8.0f) == kMissingGroundHeight);
		CHECK(map.ground_height(8.0f, 48.5f) == kMissingGroundHeight);

		const std::vector<uint8_t> bytes = map.serialize();
		AerialHeightMap loaded;
		REQUIRE(loaded.deserialize(bytes).ok());
		CHECK(loaded.serialize() == bytes);
		CHECK(loaded.ground_height(5.0f, 21.0f) == 62.0f);
		CHECK(loaded.origin().x == -32.0f);
		const std::vector<uint8_t> cut(bytes.begin(), bytes.end() - 1);
		CHECK_FALSE(loaded.deserialize(cut).ok());
		CHECK_FALSE(loaded.deserialize(std::vector<uint8_t>(4, 0)).ok());
		CHECK(loaded.ground_height(5.0f, 21.0f) == 62.0f);
	}

	TEST_CASE("AILandingZoneManager: zones by name, one active zone, and a reset back to none") {
		AILandingZoneManager zones;
		CHECK(zones.number_of_landing_zones() == 0);
		CHECK(zones.active_zone_name() == std::string(kNoActiveLandingZone));
		REQUIRE(zones.create_landing_zone("north", { 1.0f, 2.0f, 3.0f }, 1) != nullptr);
		CHECK(zones.create_landing_zone("north", {}, 0) == nullptr);
		REQUIRE(zones.create_landing_zone("south", { 4.0f, 5.0f, 6.0f }, 2) != nullptr);
		CHECK(zones.number_of_landing_zones() == 2);
		CHECK(zones.landing_zone("south")->position.y == 5.0f);
		CHECK(zones.landing_zone("east") == nullptr);
		CHECK_FALSE(zones.set_active_zone("east"));
		CHECK(zones.set_active_zone("south"));
		CHECK(zones.active_zone_name() == "south");
		zones.reset();
		CHECK(zones.number_of_landing_zones() == 0);
		CHECK(zones.landing_zone("north") == nullptr);
		CHECK(zones.active_zone_name() == std::string(kNoActiveLandingZone));
	}

	TEST_CASE("BotSpawner: a held spawn group stays, an empty side refuses, a squad mate spawns at the nearest valid "
			  "point") {
		World world;
		SquadManager squads(world.services());
		AIRandom random(AIRandom::seed_for(0xB07ull, 1));
		random.init();
		SpawnField field;
		BotSpawner spawner({ &field, &squads, &random, nullptr });

		Player &held = world.add(1, true, {});
		HeldGroup holder;
		holder.inner = &held;
		Spawnee keeper;
		keeper.body = &holder;
		CHECK(spawner.find_suitable_spawn_group(0, keeper));
		CHECK(keeper.target_group == -2);
		Spawnee bot;
		bot.body = &held;
		CHECK_FALSE(spawner.find_suitable_spawn_group(0, bot));
		CHECK(bot.target_group == -1);
		CHECK(bot.target_kit == kKitNone);

		field.groups = { 1, 2, 3 };
		field.positions = { { 1, { 100.0f, 0.0f, 0.0f } }, { 2, { 10.0f, 0.0f, 0.0f } }, { 3, { 20.0f, 0.0f, 0.0f } } };
		field.invalid = { 2 };
		Player &leader = world.add(2, false, { 0.5f, 0.0f, 0.0f });
		SquadCoordinator &squad = squads.coordinator(1, 1);
		world.join_squad(1, leader);
		squad.add_squad_member(leader);
		world.join_squad(1, held);
		squad.add_squad_member(held);
		squad.update_squad_leader();
		squad.update(++world.tick);
		bot.member = squads.squad_member(1, 1);
		REQUIRE(bot.member != nullptr);
		const SpawnObjective *object = nullptr;
		CHECK(spawner.find_suitable_spawn_group(1, object, bot) == 3);
		CHECK(object == nullptr);
		CHECK(field.spawns_added == std::vector<int>{ 3 });
		leader.alive = false;
		CHECK(spawner.find_suitable_spawn_group(1, object, bot) == -1);
	}

	TEST_CASE("BotSpawner: control-point groups are weighted by strategic value, free strength and squad distance") {
		World world;
		SquadManager squads(world.services());
		AIRandom random(AIRandom::seed_for(0xB07ull, 2));
		random.init();
		SpawnField field;
		BotSpawner spawner({ &field, &squads, &random, nullptr });
		Objective rich;
		rich.area = 7;
		rich.where = { 0.0f, 100.0f };
		rich.value = 60.0f;
		Objective poor;
		poor.area = 8;
		poor.where = { 300.0f, 0.0f };
		poor.value = 2.0f;
		field.objectives = { { 7, &rich }, { 8, &poor } };
		field.groups = { 1, 2, 3 };
		field.positions = { { 1, { 0.0f, 0.0f, 90.0f } }, { 2, { 290.0f, 0.0f, 0.0f } },
			{ 3, { 50.0f, 0.0f, 50.0f } } };
		field.control_points = { { 1, 0 }, { 2, 1 } };
		Player &body = world.add(1, true, {});
		Spawnee bot;
		bot.body = &body;
		int rich_picks = 0;
		for (int i = 0; i < 200; ++i) {
			const SpawnObjective *object = nullptr;
			const int group = spawner.find_suitable_spawn_group(1, object, bot);
			REQUIRE((group == 1 || group == 2));
			CHECK(object == (group == 1 ? static_cast<const SpawnObjective *>(&rich) : &poor));
			CHECK(field.spawns_added.back() == group);
			rich_picks += group == 1 ? 1 : 0;
		}
		REQUIRE(spawner.point_weights().size() == 2);
		CHECK(spawner.point_weights()[0].weight == doctest::Approx(60.0f - 10.0f));
		CHECK(spawner.point_weights()[1].weight == 1.0f);
		CHECK(rich_picks > 180);
		CHECK(rich_picks < 200);
	}

	TEST_CASE("BotSpawner: kits follow the squad's medic, enemy armour, infantry pressure and damaged friendly "
			  "vehicles") {
		World world;
		SquadManager squads(world.services());
		AIRandom random(AIRandom::seed_for(0xB07ull, 3));
		random.init();
		Knowledge knowledge;
		AIInformationGrid grid(knowledge.information, 1, 8, 8, { -64.0f, -64.0f }, { 64.0f, 64.0f });
		SpawnField field;
		field.grid = &grid;
		BotSpawner spawner({ &field, &squads, &random, &knowledge.information });

		for (int i = 0; i < 300; ++i) {
			const int kit = spawner.random_kit();
			CHECK(kit != kKitMedic);
			CHECK((kit >= 0 && kit <= kKitAntiTank));
		}
		field.kits = { -1, -1, -1, -1, -1, 5 };
		CHECK(spawner.random_kit() == kKitNone);
		field.kits = { 0, 1, 2, 3, 4, 5, 6 };

		Player &leader = world.add(1, true, {});
		Player &member = world.add(2, true, {});
		squads.add_squad_member(1, 1);
		squads.add_squad_member(2, 1);
		Spawnee bot;
		bot.id = 2;
		bot.body = &member;
		bot.member = squads.squad_member(2, 1);
		REQUIRE(bot.member != nullptr);
		REQUIRE_FALSE(bot.member->is_squad_leader());
		CHECK(spawner.find_suitable_kit(nullptr, 1, bot) == kKitMedic);
		leader.kit = kKitMedic;
		CHECK(spawner.find_suitable_kit(nullptr, 1, bot) != kKitMedic);
		member.kit = kKitMedic;
		CHECK(spawner.find_suitable_kit(nullptr, 1, bot) == kKitMedic);
		member.kit = 0;

		Objective area;
		area.area = 4;
		area.where = { 10.0f, 10.0f };
		Soldier tank;
		tank.with_unit = true;
		tank.occupied = true;
		tank.strength_class = 1;
		tank.team = 2;
		grid.add_info(knowledge.record(tank, 1));
		CHECK(spawner.find_suitable_kit(&area, 1, bot) == kKitAntiTank);
		Spawnee rocket;
		rocket.id = 9;
		rocket.heading_to = 4;
		field.roster[9] = &rocket;
		field.dead[9] = { 0, kKitAntiTank };
		int anti_tank = 0;
		for (int i = 0; i < 120; ++i) {
			anti_tank += spawner.find_suitable_kit(&area, 1, bot) == kKitAntiTank ? 1 : 0;
		}
		CHECK(anti_tank < 50);

		Soldier jeep;
		jeep.with_unit = true;
		jeep.occupied = true;
		jeep.strength_class = 2;
		jeep.team = 1;
		jeep.hp = 30.0f;
		grid.add_info(knowledge.record(jeep, 1));
		CHECK(spawner.find_suitable_kit(&area, 1, bot) == kKitEngineer);
		Spawnee wrench;
		wrench.id = 10;
		wrench.heading_to = 4;
		field.roster[10] = &wrench;
		field.dead[10] = { 0, kKitEngineer };
		std::map<int, int> relieved;
		for (int i = 0; i < 100; ++i) {
			++relieved[spawner.find_suitable_kit(&area, 1, bot)];
		}
		CHECK(relieved.count(kKitMedic) == 0);
		CHECK(relieved[kKitEngineer] < 50);

		std::vector<std::unique_ptr<Soldier>> infantry;
		for (int i = 0; i < 2; ++i) {
			auto enemy = std::make_unique<Soldier>();
			enemy->with_unit = true;
			enemy->occupied = true;
			enemy->team = 2;
			enemy->where = { float(i), 0.0f, -float(i) };
			grid.add_info(knowledge.record(*enemy, 1));
			infantry.push_back(std::move(enemy));
		}
		std::map<int, int> pressure;
		for (int i = 0; i < 400; ++i) {
			++pressure[spawner.find_suitable_kit(&area, 1, bot)];
		}
		CHECK(pressure.size() == 4);
		for (const int kit : { 0, 2, 3, kKitEngineer }) {
			CHECK(pressure[kit] > 40);
		}
	}
}

namespace {

struct ControlPoints final : StrategicGame {
	std::vector<Vec3> points;
	bool control_point_info(uint32_t p_id, Vec3 &r_position, float &r_radius, bool &r_active) const override {
		if (p_id >= points.size()) {
			return false;
		}
		r_position = points[p_id];
		r_radius = 10.0f;
		r_active = true;
		return true;
	}
	int owner_of_control_point(uint32_t) const override { return 0; }
	bool control_point_takeable(uint32_t p_id) const override { return p_id < points.size(); }
	float control_point_take_over_counter(uint32_t) const override { return 1.0f; }
	float terrain_altitude(float, float) const override { return 0.0f; }
	int number_of_maps() const override { return 1; }
	bool polygon(const Vec3 &, int, uint32_t &, uint32_t &, Vec3 &) const override { return false; }
};

struct AreaHeat final : StrategyHeat {
	std::map<int, float> heat;
	float area_heat(const AIStrategicArea &p_area) const override {
		const auto found = heat.find(p_area.index());
		return found != heat.end() ? found->second : 0.0f;
	}
};

void form_squad(World &r_world, SquadManager &r_squads, int p_squad, std::initializer_list<int> p_ids, Vec3 p_where,
		bool p_human_leader = false) {
	bool first = true;
	for (const int id : p_ids) {
		Player &player = r_world.add(id, !(first && p_human_leader), p_where);
		r_world.join_squad(p_squad, player);
		r_squads.player_joins_squad(id, p_squad, 1);
		first = false;
	}
}

void place_squad(World &r_world, SquadManager &r_squads, int p_squad, Vec3 p_where) {
	SquadCoordinator &squad = r_squads.coordinator(1, p_squad);
	for (int i = 0; i < squad.number_of_squad_members(); ++i) {
		r_world.players[squad.squad_member(i)->player_id()]->body.where = p_where;
	}
	squad.update(++r_world.tick);
}

}

TEST_SUITE("ai-strategy") {
	TEST_CASE("StrategyManager: a created strategy is the one being edited, a duplicate clears it, forced per side, "
			  "reset clears") {
		StrategyManager manager;
		CHECK(manager.active_strategy_name() == "No Strategies");
		CHECK(manager.active_strategy() == nullptr);
		CHECK(manager.create_strategy("Hunt"));
		CHECK(manager.active_strategy_name() == "Hunt");
		CHECK(manager.create_strategy("Spread"));
		CHECK_FALSE(manager.create_strategy("Hunt"));
		CHECK(manager.active_strategy() == nullptr);
		CHECK(manager.active_strategy_name() == "NULL");
		CHECK(manager.set_active_strategy("Spread") == manager.strategy("Spread"));
		CHECK(manager.set_active_strategy("Nope") == nullptr);
		CHECK(manager.active_strategy_name() == "NULL");
		CHECK(manager.list_strategies() == "Hunt\nSpread\n");
		CHECK(manager.force_strategy("Spread", 1) == "Strategy is activated : Spread");
		CHECK(manager.force_strategy("Nope", 2) == "No active strategy chosen : Nope");
		CHECK(manager.force_strategy("Spread", 3) == "Side must be 1 or 2, current side: 3");
		REQUIRE(manager.forced_strategy(1) == manager.strategy("Spread"));
		CHECK(manager.forced_strategy(2) == nullptr);
		CHECK_FALSE(manager.set_prerequisite("p"));
		CHECK_FALSE(manager.add_required_preceding_strategy("Hunt"));
		manager.set_active_strategy("Spread");
		CHECK(manager.add_required_preceding_strategy("Hunt"));
		CHECK_FALSE(manager.add_required_preceding_strategy("Hunt"));
		CHECK_FALSE(manager.add_prohibited_preceding_strategy("Nope"));
		CHECK_FALSE(manager.add_condition("c"));
		CHECK(manager.create_prerequisite("p"));
		CHECK_FALSE(manager.create_prerequisite("p"));
		CHECK(manager.active_prerequisite_name() == "p");
		CHECK(manager.create_constant_condition("c", StrategyCondition::CompareType::Crisp,
				StrategyCondition::Compare::EqualGreater, StrategyCondition::Side::Friendly, StrategyCondition::Ticket,
				10.0f));
		CHECK(manager.active_condition_name() == "c");
		CHECK_FALSE(manager.create_homogenous_condition(
				"c", StrategyCondition::CompareType::Fuzzy, StrategyCondition::Compare::Equal, StrategyCondition::Air));
		CHECK(manager.active_condition() == nullptr);
		CHECK(manager.add_condition("c", 2.0f));
		CHECK(manager.active_prerequisite()->size() == 1);
		CHECK(manager.set_prerequisite("p"));
		manager.reset();
		CHECK(manager.active_strategy_name() == "No Strategies");
		CHECK(manager.active_prerequisite_name() == "No Prerequisites");
		CHECK(manager.strategy("Hunt") == nullptr);
		CHECK(manager.condition("c") == nullptr);
		CHECK(manager.forced_strategy(1) == nullptr);
	}

	TEST_CASE("StrategyCondition: crisp answers +-1, fuzzy the signed distance, squared fuzzy keeps the sign, and a "
			  "zero divisor answers OpenBF2's constants") {
		std::array<float, 44> friendly{};
		std::array<float, 44> enemy{};
		friendly[StrategyCondition::Ticket] = 30.0f;
		enemy[StrategyCondition::Ticket] = 10.0f;
		friendly[StrategyCondition::Air] = 4.0f;
		enemy[StrategyCondition::Air] = 0.0f;
		const auto homogeneous = [&](StrategyCondition::CompareType p_type, StrategyCondition::Compare p_compare,
										 StrategyCondition::Conditional p_conditional, float p_target) {
			SCHomogeneous condition("h", p_type, p_compare, p_conditional, p_target);
			return condition.compare(friendly.data(), enemy.data(), 1);
		};
		using CT = StrategyCondition::CompareType;
		using C = StrategyCondition::Compare;
		CHECK(homogeneous(CT::Crisp, C::Equal, StrategyCondition::Ticket, 20.0f) == 1.0f);
		CHECK(homogeneous(CT::Crisp, C::Difference, StrategyCondition::Ticket, 19.0f) == -1.0f);
		CHECK(homogeneous(CT::Crisp, C::EqualGreater, StrategyCondition::Ticket, 20.0f) == 1.0f);
		CHECK(homogeneous(CT::Crisp, C::EqualSmaller, StrategyCondition::Ticket, 19.0f) == -1.0f);
		CHECK(homogeneous(CT::Crisp, C::Quotient, StrategyCondition::Ticket, 3.0f) == 1.0f);
		CHECK(homogeneous(CT::Crisp, C::QuotientGreater, StrategyCondition::Air, 100.0f) == 1.0f);
		CHECK(homogeneous(CT::Crisp, C::QuotientSmaller, StrategyCondition::Air, 100.0f) == -1.0f);
		CHECK(homogeneous(CT::Crisp, C::Quotient, StrategyCondition::Air, 0.0f) == -1.0f);
		CHECK(homogeneous(CT::Fuzzy, C::Equal, StrategyCondition::Ticket, 25.0f) == -5.0f);
		CHECK(homogeneous(CT::Fuzzy, C::EqualGreater, StrategyCondition::Ticket, 25.0f) == -5.0f);
		CHECK(homogeneous(CT::Fuzzy, C::DifferenceSmaller, StrategyCondition::Ticket, 25.0f) == 5.0f);
		CHECK(homogeneous(CT::Fuzzy, C::QuotientSmaller, StrategyCondition::Ticket, 1.0f) == -2.0f);
		CHECK(homogeneous(CT::Fuzzy, C::QuotientGreater, StrategyCondition::Air, 1.0f) == 1000.0f);
		CHECK(homogeneous(CT::Fuzzy, C::Quotient, StrategyCondition::Air, 1.0f) == -1000.0f);
		CHECK(homogeneous(CT::FuzzySqr, C::EqualGreater, StrategyCondition::Ticket, 25.0f) == -25.0f);
		CHECK(homogeneous(CT::FuzzySqr, C::EqualGreater, StrategyCondition::Ticket, 17.0f) == 9.0f);

		SCConstant constant(
				"k", CT::Crisp, C::EqualGreater, StrategyCondition::Side::Enemy, StrategyCondition::Ticket, 4.0f, 6.0f);
		CHECK(constant.kind() == StrategyCondition::Kind::Constant);
		CHECK(constant.compare(friendly.data(), enemy.data(), 1) == 1.0f);
		constant.set_target_value(7.0f);
		CHECK(constant.compare(friendly.data(), enemy.data(), 1) == -1.0f);
		SCHeterogeneous mixed("x", CT::Fuzzy, C::EqualGreater, StrategyCondition::Side::Friendly,
				StrategyCondition::Ticket, StrategyCondition::Air, 0.0f);
		CHECK(mixed.compare(friendly.data(), enemy.data(), 1) == 26.0f);
		SCHeterogeneous across("y", CT::Fuzzy, C::EqualGreater, StrategyCondition::Side::Both,
				StrategyCondition::Ticket, StrategyCondition::Ticket, 0.0f);
		CHECK(across.compare(friendly.data(), enemy.data(), 1) == 20.0f);
	}

	TEST_CASE("StrategyPrerequisite and Strategy: required conditions veto, advisory ones clamp, abort conditions "
			  "count only for the running strategy, and the preceding rules gate the score") {
		std::array<float, 44> friendly{};
		std::array<float, 44> enemy{};
		friendly[StrategyCondition::NumberOfFriendlyAreas] = 3.0f;
		using CT = StrategyCondition::CompareType;
		using C = StrategyCondition::Compare;
		using Side = StrategyCondition::Side;
		SCConstant areas("areas", CT::Fuzzy, C::EqualGreater, Side::Friendly, StrategyCondition::NumberOfFriendlyAreas,
				0.0f, 1.0f);
		SCConstant few("few", CT::Fuzzy, C::EqualSmaller, Side::Friendly, StrategyCondition::NumberOfFriendlyAreas,
				0.0f, 1.0f);
		SCConstant abort("abort", CT::Crisp, C::EqualGreater, Side::Friendly, StrategyCondition::NumberOfFriendlyAreas,
				0.0f, 0.0f);
		abort.set_abort_condition(true);
		StrategyPrerequisite prerequisite("p");
		prerequisite.add_condition(&areas, 2.0f);
		prerequisite.add_condition(nullptr, 1.0f);
		prerequisite.add_condition(&few, 1.0f);
		prerequisite.add_condition(&abort, 10.0f);
		CHECK(prerequisite.size() == 3);
		few.set_strength(StrategyCondition::Strength::AdvisoryPositive);
		CHECK(prerequisite.evaluate(friendly.data(), enemy.data(), false, 1) == 4.0f);
		CHECK(prerequisite.evaluate(friendly.data(), enemy.data(), true, 1) == 14.0f);
		few.set_strength(StrategyCondition::Strength::Advisory);
		CHECK(prerequisite.evaluate(friendly.data(), enemy.data(), false, 1) == 2.0f);
		few.set_strength(StrategyCondition::Strength::Required);
		CHECK(prerequisite.evaluate(friendly.data(), enemy.data(), false, 1) == 0.0f);
		few.set_strength(StrategyCondition::Strength::RequiredNegative);
		CHECK(prerequisite.evaluate(friendly.data(), enemy.data(), false, 1) == 2.0f);

		Strategy attack("Attack");
		Strategy defend("Defend");
		CHECK(attack.aggression() == 0.5f);
		CHECK(attack.minimum_active_time() == 1e9f);
		CHECK(attack.calculate_prerequisite(friendly.data(), enemy.data(), nullptr, 1) == 0.0f);
		attack.set_prerequisite(&prerequisite);
		attack.set_prerequisite_weight(0.5f);
		CHECK(attack.calculate_prerequisite(friendly.data(), enemy.data(), &defend, 1) == 1.0f);
		CHECK(attack.calculate_prerequisite(friendly.data(), enemy.data(), &attack, 1) == 6.0f);
		CHECK(attack.add_prohibited_preceding_strategy(&defend));
		CHECK_FALSE(attack.add_prohibited_preceding_strategy(nullptr));
		CHECK(attack.calculate_prerequisite(friendly.data(), enemy.data(), &defend, 1) == 0.0f);
		CHECK(defend.add_required_preceding_strategy(&attack));
		defend.set_prerequisite(&prerequisite);
		CHECK(defend.calculate_prerequisite(friendly.data(), enemy.data(), nullptr, 1) == 0.0f);
		CHECK(defend.calculate_prerequisite(friendly.data(), enemy.data(), &attack, 1) == 2.0f);
		attack.set_abort_level(5.0f);
		attack.set_abort_condition(&prerequisite);
		CHECK(attack.calculate_abort_condition(friendly.data(), enemy.data(), &defend, 1) == 3.0f);

		CHECK(attack.strategic_objects_modifier(AIStrategicObject::Front) == 1.0f);
		attack.set_strategic_objects_modifier(AIStrategicObject::Front, 3.0f);
		attack.set_strategic_objects_modifier(AIStrategicObject::Front, 0.0f, StrategicStatus::Neutral);
		CHECK(attack.strategic_objects_modifier(AIStrategicObject::Front) == 2.0f);
		CHECK(attack.strategic_objects_modifiers(StrategicStatus::Hostile)[AIStrategicObject::Front] == 3.0f);
		CHECK(attack.specific_strategic_object_modifier(4) == 1.0f);
		attack.set_specific_strategic_object_modifier(4, 0.25f);
		attack.set_specific_strategic_object_modifier(-1, 9.0f);
		CHECK(attack.specific_strategic_object_modifier(2) == 1.0f);
		CHECK(attack.specific_strategic_object_modifier(4) == 0.25f);
		attack.clear_all_strategic_objects_modifiers();
		CHECK(attack.strategic_objects_modifier(AIStrategicObject::Front, StrategicStatus::Owned) == 1.0f);
		attack.set_tree_modifier(40, 2.0f);
		attack.set_tree_modifier(41, 3.0f);
		attack.set_tree_modifier(3, 5, 4.0f);
		attack.set_tree_modifier(3, 6, 5.0f);
		CHECK(attack.tree_modifier(40) == 2.0f);
		CHECK(attack.tree_modifier(3, 5) == 4.0f);
		CHECK(attack.temperature_tree_modifiers().combat_strength[55][5] == 1.0f);
	}

	TEST_CASE("SCArea and StrategyManager::complete: an area condition and a specific modifier resolve their area "
			  "by name once the areas exist") {
		AIStrategicObjectManager objects{ StrategicServices{} };
		REQUIRE(objects.create("north", { -10.0f, -10.0f }, { 0.0f, 0.0f }, 0.0f, 10.0f, std::string()));
		REQUIRE(objects.create("south", { 90.0f, -10.0f }, { 100.0f, 0.0f }, 0.0f, 10.0f, std::string()));
		StrategyManager manager(&objects);
		REQUIRE(manager.create_strategy("Hold"));
		CHECK(manager.add_specific_object_modifier("south", 0.5f));
		CHECK(manager.add_specific_object_modifier("west", 2.0f));
		REQUIRE(manager.create_area_condition("south owned", "south", StrategicStatus::Owned));
		CHECK_FALSE(manager.create_area_condition("south owned", "north", StrategicStatus::Owned));
		const StrategyCondition *condition = manager.condition("south owned");
		REQUIRE(condition != nullptr);
		CHECK(condition->kind() == StrategyCondition::Kind::Area);
		CHECK(condition->compare(nullptr, nullptr, 1) == -1.0f);
		manager.complete_conditions();
		manager.complete_strategies();
		CHECK(static_cast<const SCArea *>(condition)->object() == 1);
		CHECK(manager.strategy("Hold")->specific_strategic_object_modifier(1) == 0.5f);
		objects.strategic_object(1)->set_status(1, StrategicStatus::Owned);
		CHECK(condition->compare(nullptr, nullptr, 1) == 1.0f);
		CHECK(condition->compare(nullptr, nullptr, 2) == -1.0f);
	}

	TEST_CASE("AIStrategicObject: available, present and assigned units by side, owner side, priority, vehicle "
			  "groups and the assigned part of the wanted strength") {
		AIStrategicObjectManager objects{ StrategicServices{} };
		REQUIRE(objects.create("a", { -10.0f, -10.0f }, { 0.0f, 0.0f }, 2.0f, 10.0f, std::string()));
		AIStrategicObject &a = *objects.strategic_object(0);
		CHECK(a.register_available_attacker(3, 1));
		CHECK(a.register_available_attacker(5, 1));
		CHECK(a.register_available_defender(4, 2));
		CHECK(a.number_of_available_attackers(1) == 2);
		CHECK(a.number_of_available_bots(false, 2) == 1);
		CHECK(a.unregister_available_attacker(3, 1));
		CHECK_FALSE(a.unregister_available_attacker(3, 1));
		std::set<int>::iterator it = a.available_defenders(2).begin();
		CHECK(a.unregister_available_defender(it, 2));
		CHECK(it == a.available_defenders(2).end());
		CHECK_FALSE(a.unregister_available_defender(it, 2));
		CHECK(a.unregister_all_available_attackers(1) == 1);
		CHECK(a.set_as_present_unit(7, 1));
		CHECK_FALSE(a.set_as_present_unit(7, 1));
		CHECK(a.is_present_unit(7, 1));
		CHECK_FALSE(a.is_present_unit(7, 2));
		CHECK(a.remove_as_present_unit(7, 1));
		CHECK(a.number_of_present_units(1) == 0);
		CHECK(a.side() == 0);
		a.set_side(2);
		CHECK(a.side() == 2);
		CHECK(a.status(1) == StrategicStatus::Hostile);
		a.set_status(1, StrategicStatus::Neutral);
		a.reset_status();
		CHECK(a.status(1) == StrategicStatus::Hostile);
		a.set_side(0);
		CHECK(a.side() == 0);
		CHECK(a.status(2) == StrategicStatus::Owned);
		a.set_priority(4, 2);
		CHECK(a.priority(2) == 4);
		CHECK(a.priority(1) == 0);
		CHECK(a.is_allowed_vehicle_group(7));
		a.add_allowed_vehicle_group(2);
		CHECK(a.allowed_vehicle_groups() == 4);
		CHECK_FALSE(a.is_allowed_vehicle_group(7));
		CHECK(a.allows_some_vehicle_groups(6));
		CHECK_FALSE(a.allows_some_vehicle_groups(1));
		a.add_expelled_unit(3);
		CHECK(a.is_expelled_unit(3));
		CHECK(a.expelled_vehicle_types() == 8);
		CHECK(a.assigned_part_of_wanted_strength(1) == 1.0f);
		AIRandom random(7);
		random.init();
		Vec3 spot{};
		for (int i = 0; i < 32; ++i) {
			a.randomize_pos(1, 1.0f, random, spot);
			CHECK(std::abs(spot.x) <= 10.0f);
			CHECK(std::abs(spot.z) <= 10.0f);
			CHECK(spot.y == 2.0f);
			Vec3 around{ 5.0f, 1.0f, 5.0f };
			a.randomize_controlpoint_pos(4.0f, 2.0f, random, around);
			const float reach =
					std::sqrt((around.x - 5.0f) * (around.x - 5.0f) + (around.z - 5.0f) * (around.z - 5.0f));
			CHECK(reach >= 2.0f - 1e-4f);
			CHECK(reach <= 8.0f + 1e-4f);
			CHECK(around.y == 1.0f);
		}
	}

	TEST_CASE("AIStrategicArea: a strategy scales both temperatures by flag, status and specific modifiers; "
			  "defence need, attack cost and attack value follow OpenBF2's formulas") {
		struct Squads final : StrategicSquads {
			float distance = 50.0f;
			int squad_strengths(int, int, int) const override { return 0; }
			float distance_to_closest_squad(const Vec3 &, int) const override { return distance; }
		};
		Squads squads;
		StrategicServices services;
		services.squads = &squads;
		AIStrategicObjectManager objects(services);
		REQUIRE(objects.create("a", { -10.0f, -10.0f }, { 0.0f, 0.0f }, 0.0f, 10.0f, std::string()));
		REQUIRE(objects.create("b", { 90.0f, -10.0f }, { 100.0f, 0.0f }, 0.0f, 20.0f, std::string()));
		REQUIRE(objects.create("c", { -10.0f, 90.0f }, { 0.0f, 100.0f }, 0.0f, 30.0f, std::string()));
		REQUIRE(objects.add_two_way_neighbour("a", "b"));
		REQUIRE(objects.add_two_way_neighbour("a", "c"));
		AIStrategicArea &a = *objects.strategic_area("a");
		AIStrategicArea &b = *objects.strategic_area("b");
		AIStrategicArea &c = *objects.strategic_area("c");
		a.set_status(1, StrategicStatus::Owned);
		b.set_status(1, StrategicStatus::Hostile);
		c.set_status(1, StrategicStatus::Neutral);
		CHECK(a.is_neighbour(&b));
		CHECK_FALSE(b.is_neighbour(&c));
		CHECK_FALSE(a.is_neighbouring_object(&b));
		a.set_object_type_flags((1 << AIStrategicObject::Flank) | (1 << AIStrategicObject::Base));
		CHECK(a.has_object_type_flags((1 << AIStrategicObject::Flank) | (1 << AIStrategicObject::Base)));
		CHECK_FALSE(a.has_object_type_flags((1 << AIStrategicObject::Flank) | (1 << AIStrategicObject::Close)));

		Strategy strategy("Push");
		strategy.set_strategic_objects_modifier(AIStrategicObject::Front, 2.0f, StrategicStatus::Owned);
		strategy.set_strategic_objects_modifier(AIStrategicObject::Safe, 7.0f, StrategicStatus::Owned);
		strategy.set_strategic_objects_modifier(AIStrategicObject::Flank, 3.0f, StrategicStatus::Owned);
		strategy.set_strategic_objects_modifier(AIStrategicObject::Base, 0.5f, StrategicStatus::Hostile);
		strategy.set_strategic_objects_modifier(AIStrategicObject::Enemy, 4.0f, StrategicStatus::Hostile);
		strategy.set_strategic_objects_modifier(AIStrategicObject::NeutralType, 0.1f, StrategicStatus::Neutral);
		strategy.set_specific_strategic_object_modifier(0, 0.25f);
		CHECK(a.update_temperature(strategy, 1) == 15.0f);
		CHECK(a.security_sum(1) == 15.0f);
		CHECK(b.update_temperature(strategy, 1) == 80.0f);
		CHECK(c.update_temperature(strategy, 1) == doctest::Approx(3.0f));

		const float need = (2.0f - SCurve::calculate(0.0f)) * 15.0f * 1.5f;
		CHECK(a.calculate_defense_need(1) == doctest::Approx(need));
		CHECK(b.calculate_defense_need(1) == 0.0f);
		CHECK(b.calculate_attack_cost(1) == 0.0f);
		CHECK(b.calculate_attack_value(1) == doctest::Approx(100.0f / 50.0f * 80.0f * 1.25f));
		CHECK(c.calculate_attack_value(1) == doctest::Approx(100.0f / 50.0f * 3.0f * 1.25f));
		const float strengths[6] = { 1.0f, 4.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		const int counts[6] = { 1, 1, 0, 0, 0, 0 };
		b.battle_statistics(1).sample(strengths, counts);
		CHECK(b.calculate_attack_cost(1) == 6.0f);
		squads.distance = 0.5f;
		CHECK(b.calculate_attack_value(1) == -6.0f);
		CHECK(a.calculate_attack_value(1) == 0.0f);
		CHECK(a.changed_attack_defence(1) == 0);
		CHECK(a.tactical_status(1) == 25);
		CHECK(a.tactical_status(2) == 25);
		CHECK(a.tactical_status(0) == -1);
		CHECK(a.is_inside(Vec2{ 10.0f, -10.0f }));
		CHECK(a.is_inside(Vec3{ -5.0f, 40.0f, 5.0f }));
		CHECK_FALSE(a.is_inside(Vec2{ 10.5f, 0.0f }));
	}

	TEST_CASE("AIStrategicArea: distances relax over the neighbour graph, 1.1 into neutral and 5 into hostile areas, "
			  "and keep each route back to the area") {
		AIStrategicObjectManager objects{ StrategicServices{} };
		REQUIRE(objects.create("a", { -10.0f, -10.0f }, { 0.0f, 0.0f }, 0.0f, 10.0f, std::string()));
		REQUIRE(objects.create("b", { 90.0f, -10.0f }, { 100.0f, 0.0f }, 0.0f, 10.0f, std::string()));
		REQUIRE(objects.create("c", { 90.0f, 290.0f }, { 100.0f, 300.0f }, 0.0f, 10.0f, std::string()));
		REQUIRE(objects.create("d", { -10.0f, 290.0f }, { 0.0f, 300.0f }, 0.0f, 10.0f, std::string()));
		REQUIRE(objects.add_two_way_neighbour("a", "b"));
		REQUIRE(objects.add_two_way_neighbour("b", "c"));
		REQUIRE(objects.add_two_way_neighbour("c", "d"));
		REQUIRE(objects.add_two_way_neighbour("a", "d"));
		AIStrategicArea &a = *objects.strategic_area("a");
		const AIStrategicArea &c = *objects.strategic_area("c");
		const AIStrategicArea &d = *objects.strategic_area("d");
		objects.strategic_area("b")->set_status(1, StrategicStatus::Neutral);
		objects.strategic_area("d")->set_status(1, StrategicStatus::Hostile);
		objects.strategic_area("c")->set_status(1, StrategicStatus::Owned);
		a.validate_distances(1);
		const std::list<AreaDistanceInfo> &distances = a.distances(1);
		REQUIRE(distances.size() == 4);
		std::vector<std::pair<std::string, float>> got;
		for (const AreaDistanceInfo &info : distances) {
			got.emplace_back(static_cast<const AIStrategicArea *>(info.area)->name(), info.distance);
			CHECK(info.vehicle_groups == 1);
		}
		CHECK(got[0].first == "a");
		CHECK(got[0].second == 0.0f);
		CHECK(got[1].first == "b");
		CHECK(got[1].second == doctest::Approx(110.0f));
		CHECK(got[2].first == "c");
		CHECK(got[2].second == doctest::Approx(410.0f));
		CHECK(got[3].first == "d");
		CHECK(got[3].second == doctest::Approx(910.0f));
		const std::list<const AIStrategicObject *> *route = a.route(1, 0, &d);
		REQUIRE(route != nullptr);
		CHECK(std::vector<const AIStrategicObject *>(route->begin(), route->end()) ==
				std::vector<const AIStrategicObject *>{ &d, &c, objects.strategic_area("b"), &a });
		CHECK(a.route(1, 1, &d) == nullptr);
		CHECK(a.route(2, 0, &d) == nullptr);
	}

	TEST_CASE("StrategyDecisions without areas to take: Spread sends bot-led squads to different areas, Hunt to the "
			  "hottest") {
		World world;
		world.squads = 4;
		SquadManager squads(world.services());
		AIStrategicObjectManager objects{ StrategicServices{} };
		const Vec2 centres[] = { { 0.0f, 0.0f }, { 100.0f, 0.0f }, { 0.0f, 100.0f }, { 200.0f, 200.0f } };
		for (size_t i = 0; i < 4; ++i) {
			REQUIRE(objects.create("a" + std::to_string(i), { centres[i].x - 40.0f, centres[i].y - 40.0f }, centres[i],
					0.0f, 40.0f, std::string()));
		}
		objects.post_load_init_areas();
		form_squad(world, squads, 1, { 1, 2 }, { 1.0f, 0.0f, 0.0f });
		form_squad(world, squads, 2, { 3, 4 }, { 5.0f, 0.0f, 5.0f });
		form_squad(world, squads, 3, { 5, 6 }, { 1.0f, 0.0f, 0.0f }, true);
		for (int q = 1; q <= 3; ++q) {
			squads.coordinator(1, q).update(++world.tick);
		}
		StrategyManager manager;
		AreaHeat heat;
		StrategyTuning tuning;
		StrategyDecisions decisions(manager, squads, objects, &heat, tuning);
		SaiServices services;
		services.objects = &objects;
		services.squads = &squads;
		services.decisions = &decisions;
		Sai sai(1, services);
		const auto cycle = [&] {
			decisions.choose_strategy(sai);
			decisions.modify_strategic_objects_temperatures(sai);
			decisions.update_bot_positions(sai);
			decisions.update_distribution(sai);
			decisions.distribute_with_no_areas(sai);
		};
		cycle();
		CHECK(decisions.active_strategy_name(1) == "Spread");
		const SquadCoordinator &first = squads.coordinator(1, 1);
		const SquadCoordinator &second = squads.coordinator(1, 2);
		REQUIRE(first.squad_strategic_area_assignment() != nullptr);
		REQUIRE(second.squad_strategic_area_assignment() != nullptr);
		CHECK(first.mission_status() == SquadMissionStatus::Issued);
		CHECK(first.order() == SquadOrder::Move);
		CHECK(first.squad_strategic_area_assignment()->index() == 1);
		CHECK(second.squad_strategic_area_assignment()->index() == 2);
		CHECK(first.squad_goto_position().x == 100.0f);
		CHECK(squads.coordinator(1, 3).mission_status() == SquadMissionStatus::None);
		CHECK(decisions.squad_area(1, 1) == 0);
		CHECK(decisions.stats().spread_missions == 2);

		heat.heat[3] = tuning.hunt_heat + 1.0f;
		heat.heat[1] = 0.5f;
		cycle();
		CHECK(decisions.active_strategy_name(1) == "Hunt");
		CHECK(decisions.hunted_area(1) == 3);
		CHECK(first.squad_strategic_area_assignment()->index() == 3);
		CHECK(second.squad_strategic_area_assignment()->index() == 3);
		CHECK(squads.coordinator(1, 3).mission_status() == SquadMissionStatus::None);
		CHECK(decisions.stats().hunt_missions == 2);
		heat.heat[1] = 0.6f * heat.heat[3];
		cycle();
		CHECK(decisions.hunted_area(1) == 3);
		CHECK(decisions.stats().hunt_missions == 2);

		place_squad(world, squads, 1, { 200.0f, 0.0f, 200.0f });
		squads.coordinator(1, 1).un_assign_squad();
		squads.coordinator(1, 1).assign_squad(
				&decisions.area(2), { 0.0f, 0.0f, 100.0f }, SquadOrder::Move, SquadMissionStatus::Accepted);
		cycle();
		CHECK(decisions.squad_area(1, 1) == 3);
		CHECK(squads.coordinator(1, 1).mission_status() == SquadMissionStatus::None);
		CHECK(decisions.release_squads_from_object(sai, 3, 1) == 1);
		CHECK(second.mission_status() == SquadMissionStatus::None);
		CHECK(decisions.changes(1) == 2);
		manager.force_strategy("Spread", 1);
		cycle();
		CHECK(decisions.active_strategy_name(1) == "Spread");
		CHECK(decisions.active_strategy(2) == nullptr);
	}

	TEST_CASE(
			"StrategyDecisions, Spread: a squad walks on to the area it saw least recently, not back to the nearest") {
		World world;
		world.squads = 4;
		SquadManager squads(world.services());
		AIStrategicObjectManager objects{ StrategicServices{} };
		const Vec2 centres[] = { { 0.0f, 0.0f }, { 100.0f, 0.0f }, { 0.0f, 100.0f }, { 200.0f, 200.0f } };
		for (size_t i = 0; i < 4; ++i) {
			REQUIRE(objects.create("a" + std::to_string(i), { centres[i].x - 40.0f, centres[i].y - 40.0f }, centres[i],
					0.0f, 40.0f, std::string()));
		}
		objects.post_load_init_areas();
		form_squad(world, squads, 1, { 1, 2 }, { 1.0f, 0.0f, 0.0f });
		SquadCoordinator &squad = squads.coordinator(1, 1);
		squad.update(++world.tick);
		StrategyManager manager;
		AreaHeat heat;
		StrategyTuning tuning;
		StrategyDecisions decisions(manager, squads, objects, &heat, tuning);
		SaiServices services;
		services.objects = &objects;
		services.squads = &squads;
		services.decisions = &decisions;
		Sai sai(1, services);
		const auto next_from = [&](const Vec3 &p_at) {
			squad.un_assign_squad();
			place_squad(world, squads, 1, p_at);
			decisions.choose_strategy(sai);
			decisions.update_bot_positions(sai);
			decisions.update_distribution(sai);
			decisions.distribute_with_no_areas(sai);
			const SquadArea *const area = squad.squad_strategic_area_assignment();
			return area != nullptr ? area->index() : -1;
		};
		CHECK(next_from({ 1.0f, 0.0f, 0.0f }) == 1);
		CHECK(next_from({ 100.0f, 0.0f, 0.0f }) == 2);
		CHECK(next_from({ 0.0f, 0.0f, 100.0f }) == 3);
		CHECK(next_from({ 200.0f, 0.0f, 200.0f }) == 0);
		CHECK(next_from({ 1.0f, 0.0f, 0.0f }) == 1);
	}

	TEST_CASE("StrategyDecisions with areas to take: a free squad defends a contested owned area, the next attacks the "
			  "nearest") {
		World world;
		world.squads = 4;
		SquadManager squads(world.services());
		ControlPoints points;
		points.points = { { 0.0f, 0.0f, 0.0f }, { 150.0f, 0.0f, 0.0f }, { 400.0f, 0.0f, 0.0f } };
		StrategicServices strategic;
		strategic.game = &points;
		AIStrategicObjectManager objects{ strategic };
		for (uint32_t i = 0; i < 3; ++i) {
			REQUIRE(objects.create_from_control_point("f" + std::to_string(i), i, 10.0f, std::string()));
		}
		objects.post_load_init_areas();
		objects.strategic_object(0)->set_status(1, StrategicStatus::Owned);
		objects.strategic_object(2)->set_status(1, StrategicStatus::Hostile);
		form_squad(world, squads, 1, { 1, 2 }, { 10.0f, 0.0f, 0.0f });
		form_squad(world, squads, 2, { 3, 4 }, { 140.0f, 0.0f, 0.0f });
		for (int q = 1; q <= 2; ++q) {
			squads.coordinator(1, q).update(++world.tick);
		}
		StrategyManager manager;
		AreaHeat heat;
		heat.heat[0] = 1.0f;
		StrategyDecisions decisions(manager, squads, objects, &heat, StrategyTuning{});
		SaiServices services;
		services.objects = &objects;
		services.squads = &squads;
		services.decisions = &decisions;
		Sai sai(1, services);
		decisions.modify_strategic_objects_temperatures(sai);
		decisions.update_bot_positions(sai);
		decisions.update_distribution(sai);
		decisions.distribute_resources(sai);
		decisions.distribute_defence(sai);
		decisions.distribute_attack(sai);
		const SquadCoordinator &first = squads.coordinator(1, 1);
		const SquadCoordinator &second = squads.coordinator(1, 2);
		REQUIRE(first.squad_strategic_area_assignment() != nullptr);
		REQUIRE(second.squad_strategic_area_assignment() != nullptr);
		CHECK(first.order() == SquadOrder::Defend);
		CHECK(first.squad_strategic_area_assignment()->index() == 0);
		CHECK(second.order() == SquadOrder::Attack);
		CHECK(second.squad_strategic_area_assignment()->index() == 1);
		CHECK(second.squad_goto_position().x == 150.0f);
		CHECK(decisions.stats().defend_missions == 1);
		CHECK(decisions.stats().attack_missions == 1);
		decisions.distribute_defence(sai);
		decisions.distribute_attack(sai);
		CHECK(decisions.stats().missions == 2);
	}

	TEST_CASE("BotSpawner: a member follows its living leader past the draw, never a leader, always a human leader; "
			  "past the "
			  "same draw a bot spawns on any living mate") {
		World world;
		SquadManager squads(world.services());
		form_squad(world, squads, 1, { 1, 2, 3 }, { 0.0f, 0.0f, 0.0f });
		AIRandom random(7);
		random.init();
		SpawnServices services;
		services.random = &random;
		BotSpawner spawner(services);
		int followed = 0;
		for (int i = 0; i < 200; ++i) {
			followed += spawner.squad_leader_to_follow(squads.squad_member(2, 1)) != nullptr ? 1 : 0;
		}
		CHECK(followed > 120);
		CHECK(followed < 160);
		CHECK(spawner.squad_leader_to_follow(squads.squad_member(1, 1)) == nullptr);
		world.players[1]->down = true;
		for (int i = 0; i < 20; ++i) {
			CHECK(spawner.squad_leader_to_follow(squads.squad_member(3, 1)) == nullptr);
		}
		int follows = 0;
		for (int i = 0; i < 200; ++i) {
			follows += spawner.follows_squad(squads.squad_member(1, 1), 0.3f) ? 1 : 0;
		}
		CHECK(follows > 120);
		CHECK(follows < 160);
		for (int i = 0; i < 20; ++i) {
			CHECK(spawner.follows_squad(squads.squad_member(1, 1), 0.0f));
		}
		const SquadMember *const second = spawner.squad_mate_to_follow(squads.squad_member(1, 1));
		REQUIRE(second != nullptr);
		CHECK(second->player_id() == 2);
		world.players[2]->alive = false;
		const SquadMember *const third = spawner.squad_mate_to_follow(squads.squad_member(1, 1));
		REQUIRE(third != nullptr);
		CHECK(third->player_id() == 3);
		world.players[3]->alive = false;
		CHECK(spawner.squad_mate_to_follow(squads.squad_member(1, 1)) == nullptr);
		CHECK_FALSE(spawner.follows_squad(nullptr, 0.0f));
		world.players[2]->alive = true;
		world.players[3]->alive = true;
		world.players[1]->down = false;
		world.players[1]->ai = false;
		for (int i = 0; i < 20; ++i) {
			CHECK(spawner.squad_leader_to_follow(squads.squad_member(3, 1)) != nullptr);
		}
	}
}
