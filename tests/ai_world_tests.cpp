#include <doctest/doctest.h>

#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/main/ai_settings.h"
#include "bf3/ai/nav/bot_navigation.h"
#include "bf3/ai/nav/nav_clusters.h"
#include "bf3/ai/strategic/sai.h"
#include "bf3/ai/strategic/strategic_object.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/level/mode_areas.h"
#include "bf3/host/ai_world.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/level_load.h"
#include "bf3/sim/nav.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/weapons/weapon_sim.h"
#include "tdm_fixture.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <future>
#include <map>
#include <set>
#include <thread>
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

	TEST_CASE("area heat: an area takes the heat of the cells whose centres lie within its radius") {
		std::vector<float> heat(8 * 8, 0.0f);
		heat[size_t(2 * 8 + 3)] = 2.0f;
		heat[size_t(2 * 8 + 4)] = 1.0f;
		heat[size_t(7 * 8 + 7)] = 5.0f;
		const sim::Vec3 origin{ 0.0f, 0.0f, 0.0f };
		CHECK(host::area_heat(heat, 8, origin, 16.0f, { 56.0f, 0.0f, 40.0f }, 1.0f) == doctest::Approx(2.0f));
		CHECK(host::area_heat(heat, 8, origin, 16.0f, { 64.0f, 0.0f, 40.0f }, 8.0f) == doctest::Approx(3.0f));
		CHECK(host::area_heat(heat, 8, origin, 16.0f, { 64.0f, 0.0f, 40.0f }, 200.0f) == doctest::Approx(8.0f));
		CHECK(host::area_heat(heat, 8, origin, 0.0f, { 64.0f, 0.0f, 40.0f }, 8.0f) == 0.0f);
	}
}

namespace {

struct ModeLevel {
	std::shared_ptr<const host::SharedLevel> shared;
	std::shared_ptr<const host::BotLevel> bots;
	std::vector<core::level::ModeObjectiveDef> objectives;
	double seconds = 0.0;
	std::string error;
};

ModeLevel load_mode_level(
		std::shared_ptr<core::Install> p_install, const core::InstallReader &p_reader, const std::string &p_mode) {
	ModeLevel out;
	const auto start = std::chrono::steady_clock::now();
	const std::filesystem::path root = std::filesystem::temp_directory_path() /
			("openbf3-ai-world-" + p_mode + "-" +
					std::to_string(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())));
	core::Cache cache;
	core::CacheOptions co;
	co.root = root;
	co.guard = p_reader.write_guard();
	std::string detail;
	if (cache.open(std::move(co), &detail) != core::CacheStatus::Ok) {
		out.error = detail;
		return out;
	}
	core::Result<host::LevelDatabase> db = host::open_level_database(p_install, "MP_Subway", p_mode, 0, nullptr);
	if (!db) {
		out.error = db.error().message;
		return out;
	}
	host::SharedLoadOptions so;
	so.cache = &cache;
	so.tuning = bf3::test::gunplay::shipped_motor_tuning();
	core::Result<std::shared_ptr<const host::SharedLevel>> shared = host::load_shared_level(*db, so, nullptr);
	if (!shared) {
		out.error = shared.error().message;
		return out;
	}
	out.shared = *shared;
	host::BotLoadOptions bo;
	bo.cache = &cache;
	bo.threads = std::max(1u, std::thread::hardware_concurrency() / 4u);
	core::Result<std::shared_ptr<const host::BotLevel>> bots = host::load_bot_level(*db, *out.shared, bo, nullptr);
	if (!bots) {
		out.error = bots.error().message;
		return out;
	}
	out.bots = *bots;
	core::Result<core::level::ModeAreas> areas = core::level::read_mode_areas(*db->world);
	if (areas) {
		out.objectives = areas->objectives;
	}
	cache.close();
	std::error_code ec;
	std::filesystem::remove_all(root, ec);
	out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	return out;
}

void check_pathable(const host::BotLevel &p_level) {
	const ai::MaterialManager materials;
	ai::ClusterNodeManager nodes;
	std::vector<uint32_t> path;
	size_t pairs = 0;
	for (size_t i = 0; i < p_level.areas.size(); ++i) {
		const host::BotStrategicArea &a = p_level.areas[i];
		REQUIRE(a.cluster != ai::kNoCluster);
		CHECK_MESSAGE(sim::nav_poly_component(*p_level.nav, p_level.components, a.poly) == p_level.main_component,
				a.name << " is off the main component");
		for (size_t j = i + 1; j < p_level.areas.size(); ++j) {
			const float cost =
					p_level.clusters->graph.shortest_path(a.cluster, p_level.areas[j].cluster, materials, nodes, path);
			CHECK_MESSAGE(cost >= 0.0f, a.name << " to " << p_level.areas[j].name << " has no cluster path");
			const float back =
					p_level.clusters->graph.shortest_path(p_level.areas[j].cluster, a.cluster, materials, nodes, path);
			CHECK_MESSAGE(back >= 0.0f, p_level.areas[j].name << " to " << a.name << " has no cluster path");
			++pairs;
		}
	}
	std::printf("ai world: %s %s: %zu areas, %zu pairs pathable over %u clusters (%u portals, %zu polygons)\n",
			p_level.level.c_str(), p_level.mode.c_str(), p_level.areas.size(), pairs, p_level.clusters->cluster_count(),
			p_level.clusters->graph.number_of_portals(), p_level.clusters->poly_cluster.size());
}

}

TEST_SUITE("ai-world-install") {
	TEST_CASE("ai world: Metro TDM areas, Metro CQL control points and the perception courier for 32 bots") {
		test::tdm::TdmLevel *level = test::tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		std::future<ModeLevel> cql = std::async(
				std::launch::async, [&] { return load_mode_level(level->install, *level->reader, "ConquestLarge0"); });

		const host::BotLevel &tdm = *level->bots;
		REQUIRE(tdm.clusters != nullptr);
		std::set<uint32_t> covered;
		size_t spawn_rows = 0;
		for (const host::BotPointRow &row : tdm.points) {
			spawn_rows += row.kind == host::BotPointRow::Kind::Spawn ? 1 : 0;
		}
		for (const host::BotStrategicArea &area : tdm.areas) {
			CHECK(area.kind == host::BotStrategicArea::Kind::TdmCluster);
			CHECK_FALSE(area.takeable);
			CHECK(area.team == 0);
			for (const uint32_t s : area.spawns) {
				CHECK_MESSAGE(covered.insert(s).second, "spawn " << s << " is in two areas");
			}
		}
		CHECK(spawn_rows == 102);
		CHECK(covered.size() == spawn_rows);
		CHECK(tdm.areas.size() >= 4);
		CHECK(tdm.areas.size() <= 12);
		check_pathable(tdm);
		std::printf("ai world: Metro TDM strategic map built in %.3f s\n", tdm.seconds_strategic);

		host::AiWorldConfig config;
		config.level = level->bots;
		config.static_world = level->shared->static_world;
		config.seed = 7;
		std::unique_ptr<host::HostAiWorld> world;
		std::array<uint32_t, 64> cursor{};
		std::array<sim::Tick, 64> fired{};
		std::vector<sim::bots::HeardShot> heard;
		size_t compared = 0, detections = 0, partial = 0, live_facings = 0;
		bool mismatch = false;
		test::tdm::TdmRunConfig run;
		run.peer = false;
		run.bots = 32;
		run.max_ticks = 60 * 40;
		run.hashes = false;
		run.on_tick = [&](sim::Tick p_tick, host::GameServer &r_server, test::tdm::ScriptedPeer *) {
			const host::ServerView &view = r_server.view();
			if (world == nullptr) {
				core::Result<std::unique_ptr<host::HostAiWorld>> made = host::HostAiWorld::create(config);
				REQUIRE_MESSAGE(made.ok(), (made.ok() ? std::string() : made.error().message));
				world = std::move(*made);
				for (uint8_t s = 0; s < 32; ++s) {
					world->add_bot(s);
				}
				REQUIRE(world->strategic().number_of_strategic_objects() == int(tdm.areas.size()));
			}
			if (r_server.gunplay() != nullptr) {
				for (const sim::weapons::ShotEvent &shot : r_server.gunplay()->interval_shots()) {
					if (shot.player_slot < 64 && shot.shot_index >= cursor[shot.player_slot]) {
						cursor[shot.player_slot] = shot.shot_index + 1;
						fired[shot.player_slot] = p_tick;
						heard.push_back({ uint8_t(shot.player_slot), shot.origin, p_tick });
					}
				}
			}
			heard.erase(std::remove_if(heard.begin(), heard.end(),
								[&](const sim::bots::HeardShot &p_s) { return p_s.tick + 30 < p_tick; }),
					heard.end());
			std::array<uint8_t, 64> firing{};
			for (size_t s = 0; s < 64; ++s) {
				firing[s] = fired[s] != 0 && p_tick - fired[s] <= 30 ? 1 : 0;
			}
			world->step(view, heard, firing, p_tick);
			if (p_tick % 6 != 0) {
				return;
			}
			const size_t slots = std::min<size_t>(view.states().size(), 64);
			for (int side = 1; side <= 2; ++side) {
				std::map<uint8_t, const sim::bots::PerceivedEnemy *> detected;
				size_t enemies_alive = 0;
				for (size_t s = 0; s < slots; ++s) {
					enemies_alive +=
							view.alive(uint8_t(s)) && view.team(uint8_t(s)) != side && view.team(uint8_t(s)) != 0 ? 1
																												  : 0;
				}
				for (const uint8_t bot : world->bots()) {
					if (bot >= slots || view.team(bot) != side) {
						continue;
					}
					for (const sim::bots::PerceivedEnemy &e : world->perception(bot)->enemies()) {
						if (e.detected && (detected[e.slot] == nullptr || e.last_seen > detected[e.slot]->last_seen)) {
							detected[e.slot] = &e;
						}
					}
				}
				size_t known_enemies = 0;
				for (size_t s = 0; s < slots; ++s) {
					const uint32_t handle = world->information_of(side, uint8_t(s));
					const bool enemy = view.team(uint8_t(s)) != side;
					if (!enemy) {
						const bool own = view.alive(uint8_t(s)) && view.team(uint8_t(s)) == side;
						if (own != (handle != ai::kNoHandle)) {
							mismatch = true;
						}
						continue;
					}
					const auto found = detected.find(uint8_t(s));
					if ((found != detected.end()) != (handle != ai::kNoHandle)) {
						mismatch = true;
						continue;
					}
					if (handle == ai::kNoHandle) {
						continue;
					}
					++known_enemies;
					const ai::Information *information = world->information().information(handle);
					REQUIRE(information != nullptr);
					const sim::Vec3 at = information->position();
					const sim::Vec3 want = found->second->last_position;
					if (at.x != want.x || at.y != want.y || at.z != want.z ||
							information->side() != int(view.team(uint8_t(s)))) {
						mismatch = true;
					}
					const ai::Vec3 facing = information->facing();
					if (facing.x != 0.0f || facing.y != 0.0f || facing.z != 1.0f) {
						++live_facings;
					}
				}
				std::vector<uint32_t> in_grid;
				world->grid(side).all_information(in_grid, nullptr);
				size_t own_alive = 0;
				for (size_t s = 0; s < slots; ++s) {
					own_alive += view.alive(uint8_t(s)) && view.team(uint8_t(s)) == side ? 1 : 0;
				}
				CHECK(in_grid.size() == own_alive + known_enemies);
				detections += known_enemies;
				partial += known_enemies < enemies_alive ? 1 : 0;
				++compared;
			}
		};
		core::Result<test::tdm::TdmRunResult> result = test::tdm::run_tdm(*level, run);
		REQUIRE_MESSAGE(result.ok(), (result.ok() ? std::string() : result.error().message));
		CHECK_FALSE(mismatch);
		CHECK(live_facings == 0);
		CHECK(compared > 600);
		CHECK(detections > 0);
		CHECK(partial > 0);
		CHECK(world->perception_ticks() > 0);
		std::printf("ai world: courier compared %zu side ticks, %zu perceived enemy records, %zu partial views, %u "
					"perception updates\n",
				compared, detections, partial, world->perception_ticks());

		ModeLevel conquest = cql.get();
		REQUIRE_MESSAGE(conquest.error.empty(), conquest.error);
		const host::BotLevel &cq = *conquest.bots;
		size_t flags = 0, hqs = 0;
		for (const core::level::ModeObjectiveDef &o : conquest.objectives) {
			flags += o.kind == core::level::ModeObjectiveDef::Kind::Flag ? 1 : 0;
			hqs += o.kind == core::level::ModeObjectiveDef::Kind::Hq ? 1 : 0;
		}
		CHECK(flags > 0);
		CHECK(hqs == 2);
		size_t area_flags = 0, area_hqs = 0;
		std::set<int> hq_teams;
		for (const host::BotStrategicArea &area : cq.areas) {
			REQUIRE(area.objective < conquest.objectives.size());
			CHECK(area.team == conquest.objectives[area.objective].team);
			if (area.kind == host::BotStrategicArea::Kind::Flag) {
				++area_flags;
				CHECK(area.takeable);
			} else if (area.kind == host::BotStrategicArea::Kind::Hq) {
				++area_hqs;
				CHECK_FALSE(area.takeable);
				hq_teams.insert(area.team);
			}
		}
		CHECK(area_flags == flags);
		CHECK(area_hqs == hqs);
		CHECK(hq_teams == std::set<int>{ 1, 2 });
		check_pathable(cq);
		host::AiWorldConfig cq_config;
		cq_config.level = conquest.bots;
		cq_config.static_world = conquest.shared->static_world;
		core::Result<std::unique_ptr<host::HostAiWorld>> cq_world = host::HostAiWorld::create(cq_config);
		REQUIRE_MESSAGE(cq_world.ok(), (cq_world.ok() ? std::string() : cq_world.error().message));
		ai::AiSettings settings;
		for (int side = 1; side <= 2; ++side) {
			ai::SaiServices services;
			services.objects = &(*cq_world)->strategic();
			services.settings = &settings;
			ai::Sai sai(side, services);
			for (size_t i = 0; i < cq.areas.size(); ++i) {
				ai::AIStrategicArea *area = (*cq_world)->area(i);
				REQUIRE(area != nullptr);
				bool changed = false;
				area->update(side, sai, changed);
				const int team = cq.areas[i].team;
				const ai::StrategicStatus want = team == 0 ? ai::StrategicStatus::Neutral
						: team == side					   ? ai::StrategicStatus::Owned
														   : ai::StrategicStatus::Hostile;
				CHECK_MESSAGE(area->status(side) == want, cq.areas[i].name);
				CHECK(area->is_area_takeable() == cq.areas[i].takeable);
				CHECK(area->order_positions_initialised());
				const ai::Vec3 order = area->order_position(0);
				CHECK(std::fabs(order.x - cq.areas[i].centre.x) < 2.5f);
				CHECK(std::fabs(order.z - cq.areas[i].centre.z) < 2.5f);
			}
		}
		std::printf("ai world: Metro CQL %zu flags and %zu HQs (loaded in %.1f s), TDM fixture %.1f s\n", area_flags,
				area_hqs, conquest.seconds, level->seconds_load);
		test::tdm::check_install_unchanged(*level);
	}
}
