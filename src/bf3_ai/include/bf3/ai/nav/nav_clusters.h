#pragma once

#include "bf3/ai/path/cluster_graph.h"
#include "bf3/core/result.h"
#include "bf3/sim/nav.h"

#include <cstdint>
#include <vector>

namespace bf3::ai {

inline constexpr uint32_t kNoCluster = UINT32_MAX;

struct NavClusterOptions {
	float target_area_m2 = 400.0f;
	uint32_t max_polygons = 64;
};

MaterialId nav_area_material(uint8_t p_area);

struct NavClusterMap {
	ClusterGraph graph;
	std::vector<uint32_t> poly_cluster;
	std::vector<Vec3> centres;
	std::vector<sim::NavPolyRef> centre_polys;
	std::vector<float> area_m2;

	uint32_t cluster_of(const sim::NavWorld &p_world, sim::NavPolyRef p_poly) const;
	uint32_t cluster_count() const { return graph.number_of_clusters(); }
};

core::Result<NavClusterMap> build_nav_clusters(
		const sim::NavPolyGraph &p_graph, const NavClusterOptions &p_options = {});

}
