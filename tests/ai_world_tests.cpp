#include <doctest/doctest.h>

#include "bf3/ai/nav/bot_navigation.h"
#include "bf3/ai/nav/nav_clusters.h"
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
}
