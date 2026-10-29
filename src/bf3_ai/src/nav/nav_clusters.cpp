#include "bf3/ai/nav/nav_clusters.h"

#include "bf3/ai/squad/squad_world.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <string>

namespace bf3::ai {

namespace {

bool linked(const sim::NavPolyGraph &p_graph, uint32_t p_from, uint32_t p_to) {
	for (uint32_t l = p_graph.first_link[p_from]; l < p_graph.first_link[p_from + 1]; ++l) {
		if (p_graph.links[l].to == p_to) {
			return true;
		}
	}
	return false;
}

}

MaterialId nav_area_material(uint8_t p_area) {
	switch (p_area) {
		case sim::kNavAreaWalk:
		case sim::kNavAreaCrouch:
		case sim::kNavAreaDrop:
			return kMaterialGround;
		default:
			return kMaterialInvalid;
	}
}

uint32_t NavClusterMap::cluster_of(const sim::NavWorld &p_world, sim::NavPolyRef p_poly) const {
	const uint32_t index = sim::nav_poly_index(p_world, p_poly);
	return index < poly_cluster.size() ? poly_cluster[index] : kNoCluster;
}

core::Result<NavClusterMap> build_nav_clusters(const sim::NavPolyGraph &p_graph, const NavClusterOptions &p_options) {
	const size_t n = p_graph.polys.size();
	if (p_graph.first_link.size() != n + 1 || (n > 0 && p_graph.first_link[n] != p_graph.links.size())) {
		return core::make_error("nav clusters: the polygon graph's link table does not match its polygons");
	}
	if (!(p_options.target_area_m2 > 0.0f) || p_options.max_polygons == 0) {
		return core::make_error("nav clusters: the target area and polygon cap must be positive");
	}
	NavClusterMap out;
	out.poly_cluster.assign(n, kNoCluster);
	std::vector<std::vector<uint32_t>> members;
	std::deque<uint32_t> open;
	for (uint32_t seed = 0; seed < n; ++seed) {
		if (out.poly_cluster[seed] != kNoCluster || nav_area_material(p_graph.polys[seed].area) == kMaterialInvalid) {
			continue;
		}
		const uint32_t cluster = uint32_t(members.size());
		members.emplace_back();
		std::vector<uint32_t> &list = members.back();
		float area = 0.0f;
		open.clear();
		open.push_back(seed);
		out.poly_cluster[seed] = cluster;
		while (!open.empty() && area < p_options.target_area_m2 && list.size() < p_options.max_polygons) {
			const uint32_t poly = open.front();
			open.pop_front();
			list.push_back(poly);
			area += p_graph.polys[poly].area_m2;
			for (uint32_t l = p_graph.first_link[poly]; l < p_graph.first_link[poly + 1]; ++l) {
				const uint32_t next = p_graph.links[l].to;
				if (next >= n || out.poly_cluster[next] != kNoCluster ||
						nav_area_material(p_graph.polys[next].area) != nav_area_material(p_graph.polys[seed].area) ||
						!linked(p_graph, next, poly)) {
					continue;
				}
				out.poly_cluster[next] = cluster;
				open.push_back(next);
			}
		}
		for (const uint32_t left : open) {
			out.poly_cluster[left] = kNoCluster;
		}
	}

	const size_t clusters = members.size();
	out.centres.resize(clusters);
	out.centre_polys.resize(clusters);
	out.area_m2.assign(clusters, 0.0f);
	std::vector<Cluster> table(clusters);
	std::vector<uint32_t> polygons;
	polygons.reserve(n);
	for (size_t c = 0; c < clusters; ++c) {
		Vec3 mean{};
		float weight = 0.0f;
		for (const uint32_t poly : members[c]) {
			const sim::NavGraphPoly &p = p_graph.polys[poly];
			const float w = std::max(p.area_m2, 1e-3f);
			mean.x += p.centre.x * w;
			mean.y += p.centre.y * w;
			mean.z += p.centre.z * w;
			weight += w;
			out.area_m2[c] += p.area_m2;
		}
		mean = { mean.x / weight, mean.y / weight, mean.z / weight };
		uint32_t best = members[c].front();
		for (const uint32_t poly : members[c]) {
			if (distance_sq(p_graph.polys[poly].centre, mean) < distance_sq(p_graph.polys[best].centre, mean)) {
				best = poly;
			}
		}
		out.centres[c] = p_graph.polys[best].centre;
		out.centre_polys[c] = p_graph.polys[best].ref;
		table[c].first_polygon = uint32_t(polygons.size());
		table[c].number_of_polygons = uint32_t(members[c].size());
		table[c].material = nav_area_material(p_graph.polys[members[c].front()].area);
		polygons.insert(polygons.end(), members[c].begin(), members[c].end());
	}

	std::vector<ClusterPortal> portals;
	std::vector<float> costs;
	for (size_t c = 0; c < clusters; ++c) {
		std::map<uint32_t, std::pair<Vec3, float>> best;
		for (const uint32_t poly : members[c]) {
			for (uint32_t l = p_graph.first_link[poly]; l < p_graph.first_link[poly + 1]; ++l) {
				const sim::NavGraphLink &link = p_graph.links[l];
				if (link.to >= n) {
					continue;
				}
				const uint32_t to = out.poly_cluster[link.to];
				if (to == kNoCluster || to == c) {
					continue;
				}
				const float cost = distance(out.centres[c], link.portal) + distance(link.portal, out.centres[to]);
				const auto found = best.find(to);
				if (found == best.end() || cost < found->second.second) {
					best[to] = { link.portal, cost };
				}
			}
		}
		if (portals.size() + best.size() > std::numeric_limits<uint16_t>::max()) {
			return core::make_error("nav clusters: " + std::to_string(portals.size() + best.size()) +
					" portals exceed the cluster graph's 16-bit portal index; raise the target area");
		}
		table[c].first_portal = uint16_t(portals.size());
		table[c].number_of_portals = uint16_t(best.size());
		for (const auto &[to, entry] : best) {
			portals.push_back({ entry.first, to });
			costs.push_back(entry.second);
		}
	}
	BF3_TRY(out.graph.assign(std::move(table), std::move(polygons), std::move(portals), std::move(costs)));
	return out;
}

}
