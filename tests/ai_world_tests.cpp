#include <doctest/doctest.h>

#include "bf3/ai/main/ai_settings.h"
#include "bf3/ai/nav/bot_navigation.h"
#include "bf3/ai/nav/nav_clusters.h"
#include "bf3/ai/strategic/sai.h"
#include "bf3/ai/strategic/strategic_object.h"
#include "bf3/sim/nav.h"
#include "bf3/sim/static_world.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace bf3;

namespace {

struct Quad {
	sim::Vec3 a, b, c, d;
};

std::vector<sim::StaticTriangle> walled_floor() {
	const Quad quads[] = {
		{ { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 60.0f }, { 60.0f, 0.0f, 60.0f }, { 60.0f, 0.0f, 0.0f } },
		{ { 29.0f, 3.0f, 0.0f }, { 29.0f, 3.0f, 50.0f }, { 31.0f, 3.0f, 50.0f }, { 31.0f, 3.0f, 0.0f } },
		{ { 29.0f, 0.0f, 0.0f }, { 29.0f, 3.0f, 0.0f }, { 29.0f, 3.0f, 50.0f }, { 29.0f, 0.0f, 50.0f } },
		{ { 31.0f, 0.0f, 0.0f }, { 31.0f, 0.0f, 50.0f }, { 31.0f, 3.0f, 50.0f }, { 31.0f, 3.0f, 0.0f } },
		{ { 29.0f, 0.0f, 50.0f }, { 29.0f, 3.0f, 50.0f }, { 31.0f, 3.0f, 50.0f }, { 31.0f, 0.0f, 50.0f } },
	};
	std::vector<sim::StaticTriangle> out;
	for (const Quad &q : quads) {
		sim::StaticTriangle t;
		t.v = { q.a, q.b, q.c };
		out.push_back(t);
		t.v = { q.a, q.c, q.d };
		out.push_back(t);
	}
	return out;
}

std::shared_ptr<const sim::NavWorld> walled_nav() {
	static std::shared_ptr<const sim::NavWorld> s_nav = [] {
		const std::vector<sim::StaticTriangle> tris = walled_floor();
		core::Result<std::shared_ptr<const sim::NavWorld>> nav = sim::bake_nav_world(
				std::span<const sim::StaticTriangle>(tris), std::vector<sim::NavTileKey>{ { 0, 0 } });
		REQUIRE_MESSAGE(nav.ok(), (nav.ok() ? std::string() : nav.error().message));
		return *nav;
	}();
	return s_nav;
}

class FakeTimer final : public ai::AiTimer {
public:
	sim::Tick game_tick() const override { return tick; }
	double tick_seconds() const override { return double(sim::kTickSeconds); }
	double update_time() const override { return clock += step; }
	sim::Tick tick = 0;
	double step = 0.0;
	mutable double clock = 0.0;
};

class FakeStrategicGame final : public ai::StrategicGame {
public:
	bool control_point_info(uint32_t p_id, ai::Vec3 &r_position, float &r_radius, bool &r_active) const override {
		if (p_id >= points.size()) {
			return false;
		}
		r_position = points[p_id];
		r_radius = 10.0f;
		r_active = true;
		return true;
	}
	int owner_of_control_point(uint32_t p_id) const override { return p_id < owners.size() ? owners[p_id] : 0; }
	bool control_point_takeable(uint32_t p_id) const override { return p_id < takeable.size() && takeable[p_id]; }
	float control_point_take_over_counter(uint32_t) const override { return 1.0f; }
	float terrain_altitude(float, float) const override { return 2.0f; }
	int number_of_maps() const override { return 1; }
	bool polygon(
			const ai::Vec3 &p_position, int, uint32_t &r_polygon, uint32_t &r_island, ai::Vec3 &r_on) const override {
		r_polygon = 7;
		r_island = 3;
		r_on = { p_position.x, p_position.y + 0.5f, p_position.z };
		return true;
	}
	std::vector<ai::Vec3> points;
	std::vector<int> owners;
	std::vector<bool> takeable;
};

class RecordingDecisions final : public ai::SaiDecisions {
public:
	void choose_strategy(ai::Sai &) override { calls.push_back("choose_strategy"); }
	void modify_strategic_objects_temperatures(ai::Sai &) override { calls.push_back("temperatures"); }
	void update_bot_positions(ai::Sai &) override { calls.push_back("bot_positions"); }
	void update_distribution(ai::Sai &) override { calls.push_back("distribution"); }
	void distribute_resources(ai::Sai &) override { calls.push_back("resources"); }
	void distribute_defence(ai::Sai &) override { calls.push_back("defence"); }
	void distribute_attack(ai::Sai &) override { calls.push_back("attack"); }
	void distribute_with_no_areas(ai::Sai &) override { calls.push_back("no_areas"); }
	int release_squads_from_object(ai::Sai &, int, int) override {
		calls.push_back("release");
		return 0;
	}
	std::vector<std::string> calls;
};

}

TEST_SUITE("ai-world-unit") {
	TEST_CASE("nav clusters: strongly connected clusters over the Detour polygons, portals only where links are") {
		const std::shared_ptr<const sim::NavWorld> nav = walled_nav();
		const sim::NavPolyGraph graph = sim::nav_poly_graph(*nav);
		REQUIRE(graph.polys.size() == nav->poly_count());
		REQUIRE(graph.first_link.size() == graph.polys.size() + 1);
		ai::NavClusterOptions options;
		options.target_area_m2 = 150.0f;
		core::Result<ai::NavClusterMap> built = ai::build_nav_clusters(graph, options);
		REQUIRE_MESSAGE(built.ok(), (built.ok() ? std::string() : built.error().message));
		const ai::NavClusterMap &map = *built;
		CHECK(map.cluster_count() > 4);
		for (size_t p = 0; p < graph.polys.size(); ++p) {
			CHECK(map.poly_cluster[p] != ai::kNoCluster);
		}
		for (uint32_t c = 0; c < map.cluster_count(); ++c) {
			const ai::Cluster &cluster = map.graph.cluster(c);
			std::set<uint32_t> members(map.graph.cluster_polygons().begin() + cluster.first_polygon,
					map.graph.cluster_polygons().begin() + cluster.first_polygon + cluster.number_of_polygons);
			CHECK(members.size() == cluster.number_of_polygons);
			std::set<uint32_t> reached{ *members.begin() };
			std::vector<uint32_t> open{ *members.begin() };
			while (!open.empty()) {
				const uint32_t p = open.back();
				open.pop_back();
				for (uint32_t l = graph.first_link[p]; l < graph.first_link[p + 1]; ++l) {
					const uint32_t q = graph.links[l].to;
					if (members.contains(q) && !reached.contains(q)) {
						reached.insert(q);
						open.push_back(q);
					}
				}
			}
			CHECK_MESSAGE(reached == members, "cluster " << c << " is not connected inside itself");
			CHECK(map.cluster_of(*nav, map.centre_polys[c]) == c);
			for (uint32_t i = 0; i < cluster.number_of_portals; ++i) {
				const ai::ClusterPortal &portal = map.graph.cluster_portal(c, i);
				bool real = false;
				for (const uint32_t p : members) {
					for (uint32_t l = graph.first_link[p]; l < graph.first_link[p + 1]; ++l) {
						real = real ||
								(graph.links[l].to < map.poly_cluster.size() &&
										map.poly_cluster[graph.links[l].to] == portal.to_cluster);
					}
				}
				CHECK(real);
				CHECK(map.graph.portal_cost(c, i) > 0.0f);
			}
		}
	}

	TEST_CASE("nav clusters: the cluster path around the wall goes through the gap") {
		const std::shared_ptr<const sim::NavWorld> nav = walled_nav();
		core::Result<ai::NavClusterMap> built = ai::build_nav_clusters(sim::nav_poly_graph(*nav));
		REQUIRE(built.ok());
		core::Result<std::unique_ptr<sim::NavQuery>> query = sim::NavQuery::create(nav);
		REQUIRE(query.ok());
		sim::NavPolyRef from = 0, to = 0;
		sim::Vec3 at{};
		REQUIRE((*query)->nearest({ 10.0f, 0.0f, 10.0f }, { 2.0f, 2.0f, 2.0f }, &from, &at));
		REQUIRE((*query)->nearest({ 50.0f, 0.0f, 10.0f }, { 2.0f, 2.0f, 2.0f }, &to, &at));
		const ai::MaterialManager materials;
		ai::ClusterNodeManager nodes;
		std::vector<uint32_t> path;
		const float cost = built->graph.shortest_path(
				built->cluster_of(*nav, from), built->cluster_of(*nav, to), materials, nodes, path);
		REQUIRE(cost > 0.0f);
		CHECK(cost > 2.0f * 40.0f);
		bool gap = false;
		for (size_t i = 0; i + 1 < path.size(); ++i) {
			for (uint32_t p = 0; p < built->graph.cluster(path[i]).number_of_portals; ++p) {
				const ai::ClusterPortal &portal = built->graph.cluster_portal(path[i], p);
				gap = gap || (portal.to_cluster == path[i + 1] && portal.position.z > 48.0f);
			}
		}
		CHECK(gap);
		CHECK(ai::nav_area_material(sim::kNavAreaWalk) == ai::kMaterialGround);
		CHECK(ai::nav_area_material(0) == ai::kMaterialInvalid);
		ai::NavClusterOptions bad;
		bad.target_area_m2 = 0.0f;
		CHECK_FALSE(ai::build_nav_clusters(sim::nav_poly_graph(*nav), bad).ok());
	}

	TEST_CASE("bot navigation: cluster search, time-sliced polygon searches and corners walk a bot round the wall") {
		const std::shared_ptr<const sim::NavWorld> nav = walled_nav();
		core::Result<ai::NavClusterMap> built = ai::build_nav_clusters(sim::nav_poly_graph(*nav));
		REQUIRE(built.ok());
		core::Result<std::unique_ptr<sim::NavQuery>> query = sim::NavQuery::create(nav);
		REQUIRE(query.ok());
		sim::bots::PathService::Options po;
		po.iterations_per_tick = 64;
		sim::bots::PathService paths(**query, po);
		const ai::MaterialManager materials;
		const ai::BotNavigationWorld world{ nav.get(), &*built, &materials, query->get(), &paths };
		ai::BotNavigationParams params;
		params.clusters_ahead = 2;
		ai::BotNavigation navigation(3, params);
		sim::Vec3 position{ 10.0f, 0.0f, 10.0f };
		REQUIRE(navigation.init_pathfinding(world, position, { 50.0f, 0.0f, 10.0f }, 0));
		CHECK(navigation.is_path_search_pending());
		CHECK(navigation.cluster_path().size() > 2);
		bool crossed_wall = false;
		sim::Tick tick = 0;
		for (; tick < 3000 && navigation.state() != ai::BotNavigationState::Arrived; ++tick) {
			paths.run();
			navigation.update(world, position, tick);
			REQUIRE(navigation.state() != ai::BotNavigationState::Failed);
			if (navigation.state() == ai::BotNavigationState::Following) {
				const sim::Vec3 steer = navigation.steer_point();
				const float dx = steer.x - position.x, dz = steer.z - position.z;
				const float d = std::sqrt(dx * dx + dz * dz);
				if (d > 1e-3f) {
					const float step = std::min(d, 0.1f);
					position = { position.x + dx / d * step, steer.y, position.z + dz / d * step };
				}
			}
			crossed_wall = crossed_wall || (position.x > 29.0f && position.x < 31.0f && position.z < 50.0f);
		}
		CHECK(navigation.state() == ai::BotNavigationState::Arrived);
		CHECK_FALSE(crossed_wall);
		CHECK(navigation.searches() > 1);
		CHECK(navigation.cluster_cursor() > 0);
		std::printf("bot navigation: arrived after %u ticks, %u polygon searches over %zu clusters\n", tick,
				navigation.searches(), navigation.cluster_path().size());

		ai::BotNavigation stuck(4, params);
		REQUIRE(stuck.init_pathfinding(world, { 10.0f, 0.0f, 10.0f }, { 50.0f, 0.0f, 10.0f }, 0));
		for (sim::Tick t = 0; t <= params.progress_ticks + 1; ++t) {
			paths.run();
			stuck.update(world, { 10.0f, 0.0f, 10.0f }, t);
		}
		CHECK(stuck.is_stuck());
		CHECK_FALSE(ai::BotNavigation(5).init_pathfinding(world, { 500.0f, 0.0f, 500.0f }, { 50.0f, 0.0f, 10.0f }, 0));
	}

	TEST_CASE("strategic objects: control-point areas take their owner, extents, gravity and neighbours") {
		FakeStrategicGame game;
		game.points = { { 100.0f, 5.0f, 40.0f }, { -60.0f, 1.0f, 0.0f } };
		game.owners = { 1, 0 };
		game.takeable = { false, true };
		ai::StrategicServices services;
		services.game = &game;
		ai::AIStrategicObjectManager manager(services);
		REQUIRE(manager.create_from_control_point("hq", 0, 10.0f, ""));
		REQUIRE(manager.create_from_control_point("flag", 1, 12.0f, ""));
		CHECK_FALSE(manager.create_from_control_point("flag", 1, 12.0f, ""));
		REQUIRE(manager.create("free", { 0.0f, 0.0f }, { 20.0f, 20.0f }, 3.0f, 20.0f, ""));
		REQUIRE(manager.add_two_way_neighbour("hq", "flag"));
		manager.add_way_point({ 1.0f, 2.0f, 3.0f }, 0, "hq", "flag");
		manager.post_load_init_areas();
		REQUIRE(manager.number_of_strategic_objects() == 3);
		ai::AIStrategicArea *hq = manager.strategic_area("hq");
		ai::AIStrategicArea *flag = manager.strategic_area("flag");
		ai::AIStrategicArea *free = manager.strategic_area("free");
		CHECK(hq->index() == 0);
		CHECK(hq->position(1).x == doctest::Approx(100.0f));
		CHECK(hq->position(2).y == doctest::Approx(40.0f));
		CHECK(hq->x_extent(1) == doctest::Approx(20.0f));
		CHECK(hq->height() == doctest::Approx(5.0f));
		const ai::Vec3 order = hq->order_position(0);
		CHECK(order.x == doctest::Approx(100.0f));
		CHECK(order.y == doctest::Approx(5.5f));
		CHECK(hq->map_data()[0].island == 3u);
		CHECK(free->height() == doctest::Approx(3.0f));
		CHECK(free->order_position(-1).x == doctest::Approx(20.0f));
		CHECK_FALSE(hq->is_area_takeable());
		CHECK(flag->is_area_takeable());
		CHECK_FALSE(free->is_area_takeable());
		const std::vector<std::unique_ptr<ai::SAWayPoint>> *points = manager.way_points(1, 0);
		REQUIRE(points != nullptr);
		REQUIRE(points->size() == 1);
		CHECK((*points)[0]->polygon() == 7u);
		CHECK(hq->gravity(ai::Vec3{ 100.0f, 0.0f, 40.0f }, 1) == doctest::Approx(hq->gravity(0.5f, 1)));
		CHECK(hq->gravity(ai::Vec3{ 110.0f, 0.0f, 40.0f }, 1) == doctest::Approx(hq->gravity(0.5f, 1) / 100.0f));
		CHECK(manager.closest_object({ 99.0f, 0.0f, 40.0f }, 1) == hq);

		FakeTimer timer;
		ai::AiSettings settings;
		ai::SaiServices sai_services;
		sai_services.timer = &timer;
		sai_services.settings = &settings;
		sai_services.objects = &manager;
		for (int side = 1; side <= 2; ++side) {
			ai::Sai sai(side, sai_services);
			for (ai::AIStrategicObject *object : manager.strategic_objects()) {
				bool changed = false;
				object->update(side, sai, changed);
			}
			CHECK(hq->status(side) == (side == 1 ? ai::StrategicStatus::Owned : ai::StrategicStatus::Hostile));
			CHECK(flag->status(side) == ai::StrategicStatus::Neutral);
			CHECK(flag->has_neighbour_with_status(
					side == 1 ? ai::StrategicStatus::Owned : ai::StrategicStatus::Hostile, side));
			CHECK(sai.have_any_owned_control_points(side) == (side == 1));
			CHECK_FALSE(sai.have_any_owned_and_takeable_control_points(side));
			CHECK_FALSE(sai.own_all_takeable_control_points(side));
		}
		game.owners[1] = 2;
		ai::Sai sai2(2, sai_services);
		bool changed = false;
		flag->update(2, sai2, changed);
		CHECK(changed);
		CHECK(flag->status(2) == ai::StrategicStatus::Owned);
		CHECK(sai2.own_all_takeable_control_points(2));
	}

	TEST_CASE("sai: the staged update runs its stages in order within the budget and routes to the no-area stage") {
		FakeStrategicGame game;
		game.points = { { 0.0f, 0.0f, 0.0f } };
		game.owners = { 2 };
		ai::StrategicServices services;
		services.game = &game;
		ai::AIStrategicObjectManager manager(services);
		REQUIRE(manager.create_from_control_point("a", 0, 10.0f, ""));
		REQUIRE(manager.create("b", { 0.0f, 0.0f }, { 50.0f, 0.0f }, 0.0f, 10.0f, ""));
		manager.post_load_init_areas();
		FakeTimer timer;
		timer.tick = 60 * 60;
		ai::AiSettings settings;
		RecordingDecisions decisions;
		ai::SaiServices sai_services;
		sai_services.timer = &timer;
		sai_services.settings = &settings;
		sai_services.objects = &manager;
		sai_services.decisions = &decisions;
		ai::Sai sai(1, sai_services);
		CHECK(sai.is_friend(1));
		CHECK(sai.is_enemy(2));
		CHECK_FALSE(sai.is_enemy(1));
		sai.update(1.0);
		CHECK(sai.stage() == ai::SaiStage::StrategicObjects);
		CHECK(sai.own_state()[20] == doctest::Approx(2.0f));
		sai.update(1.0);
		CHECK(sai.stage() == ai::SaiStage::NoAreas);
		CHECK(sai.completed_updates() == 0);
		sai.update(1.0);
		CHECK(sai.stage() == ai::SaiStage::Done);
		CHECK(sai.completed_updates() == 1);
		const std::vector<std::string> expected{ "choose_strategy", "temperatures", "bot_positions", "distribution",
			"resources", "no_areas" };
		CHECK(decisions.calls == expected);
		sai.update(1.0);
		CHECK(sai.stage() == ai::SaiStage::StrategicObjects);
		CHECK(sai.completed_updates() == 1);
		CHECK(sai.own_state()[15] == doctest::Approx(60.0f));
		CHECK(sai.own_state()[21] == doctest::Approx(1.0f));
		CHECK(sai.own_state()[20] == doctest::Approx(1.0f));
		CHECK(sai.enemy_state()[19] == doctest::Approx(1.0f));
		CHECK(sai.own_state()[ai::kSaiStateCount - 1] == doctest::Approx(1.0f));

		decisions.calls.clear();
		game.owners = { 1 };
		timer.step = 1.0;
		ai::Sai sliced(1, sai_services);
		uint32_t calls = 0;
		while (sliced.completed_updates() == 0 && calls < 64) {
			sliced.update(0.5);
			++calls;
		}
		CHECK(sliced.completed_updates() == 1);
		CHECK(calls > 10);
		const std::vector<std::string> owned{ "release", "release", "choose_strategy", "temperatures", "bot_positions",
			"distribution", "resources", "defence", "attack" };
		CHECK(decisions.calls == owned);

		ai::EnemyStatistics statistics(2, 1.0f);
		const float strength[2] = { 4.0f, 2.0f };
		const int count[2] = { 2, 0 };
		statistics.sample(strength, count);
		const float half[2] = { 0.0f, 4.0f };
		const int none[2] = { 0, 2 };
		statistics.sample(half, none);
		CHECK(statistics.strength(0) == doctest::Approx(2.0f));
		CHECK(statistics.strength(1) == doctest::Approx(3.0f));
		CHECK(statistics.count(1) == doctest::Approx(1.0f));
		CHECK(statistics.samples() == 2);
	}
}
