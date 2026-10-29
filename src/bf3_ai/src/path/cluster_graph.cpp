#include "bf3/ai/path/cluster_graph.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <string>
#include <utility>

namespace bf3::ai {

ClusterNodeManager::ClusterNodeManager() {
	buckets_.fill(-1);
}

int32_t ClusterNodeManager::new_oc_data(uint32_t p_cluster) {
	int32_t index = free_;
	if (index != -1) {
		free_ = pool_[size_t(index)].next_free;
	} else {
		index = int32_t(pool_.size());
		pool_.emplace_back();
	}
	OCData &data = pool_[size_t(index)];
	data = OCData{};
	data.cluster = p_cluster;
	const size_t bucket = p_cluster % kOCDataBuckets;
	data.next_in_bucket = buckets_[bucket];
	buckets_[bucket] = index;
	++live_;
	return index;
}

void ClusterNodeManager::recycle_oc_data(int32_t p_index) {
	OCData &data = pool_[size_t(p_index)];
	const size_t bucket = data.cluster % kOCDataBuckets;
	for (int32_t *link = &buckets_[bucket]; *link != -1; link = &pool_[size_t(*link)].next_in_bucket) {
		if (*link == p_index) {
			*link = data.next_in_bucket;
			break;
		}
	}
	data.next_free = free_;
	free_ = p_index;
	--live_;
}

int32_t ClusterNodeManager::find(uint32_t p_cluster) const {
	for (int32_t index = buckets_[p_cluster % kOCDataBuckets]; index != -1;
			index = pool_[size_t(index)].next_in_bucket) {
		if (pool_[size_t(index)].cluster == p_cluster) {
			return index;
		}
	}
	return -1;
}

void ClusterNodeManager::reset() {
	free_ = -1;
	for (size_t index = pool_.size(); index > 0; --index) {
		pool_[index - 1].next_free = free_;
		free_ = int32_t(index - 1);
	}
	buckets_.fill(-1);
	live_ = 0;
}

core::Status ClusterGraph::assign(std::vector<Cluster> p_clusters, std::vector<uint32_t> p_cluster_polygons,
		std::vector<ClusterPortal> p_portals, std::vector<float> p_portal_costs) {
	if (p_portal_costs.size() != p_portals.size()) {
		return core::make_error("cluster graph: " + std::to_string(p_portal_costs.size()) + " portal costs for " +
				std::to_string(p_portals.size()) + " portals");
	}
	for (size_t index = 0; index < p_clusters.size(); ++index) {
		const Cluster &cluster = p_clusters[index];
		if (size_t(cluster.first_portal) + cluster.number_of_portals > p_portals.size()) {
			return core::make_error("cluster graph: cluster " + std::to_string(index) + " portals out of range");
		}
		if (size_t(cluster.first_polygon) + cluster.number_of_polygons > p_cluster_polygons.size()) {
			return core::make_error("cluster graph: cluster " + std::to_string(index) + " polygons out of range");
		}
	}
	for (size_t index = 0; index < p_portals.size(); ++index) {
		if (p_portals[index].to_cluster >= p_clusters.size()) {
			return core::make_error("cluster graph: portal " + std::to_string(index) + " leads to no cluster");
		}
		if (!(p_portal_costs[index] >= 0.0f)) {
			return core::make_error("cluster graph: portal " + std::to_string(index) + " has a negative cost");
		}
	}
	clusters_ = std::move(p_clusters);
	cluster_polygons_ = std::move(p_cluster_polygons);
	portals_ = std::move(p_portals);
	portal_costs_ = std::move(p_portal_costs);
	return core::Status::success();
}

void ClusterGraph::clean() {
	clusters_.clear();
	cluster_polygons_.clear();
	portals_.clear();
	portal_costs_.clear();
}

const ClusterPortal &ClusterGraph::cluster_portal(uint32_t p_cluster, uint32_t p_portal) const {
	return portals_[size_t(clusters_[p_cluster].first_portal) + p_portal];
}

float ClusterGraph::portal_cost(uint32_t p_cluster, uint32_t p_portal) const {
	return portal_costs_[size_t(clusters_[p_cluster].first_portal) + p_portal];
}

float ClusterGraph::shortest_path(uint32_t p_from, uint32_t p_to, const MaterialManager &p_materials,
		ClusterNodeManager &r_nodes, std::vector<uint32_t> &r_path) const {
	r_path.clear();
	r_nodes.reset();
	if (p_from >= clusters_.size() || p_to >= clusters_.size() ||
			!p_materials.is_valid_material(clusters_[p_from].material) ||
			!p_materials.is_valid_material(clusters_[p_to].material)) {
		return -1.0f;
	}
	using Entry = std::pair<float, int32_t>;
	std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
	open.push({ 0.0f, r_nodes.new_oc_data(p_from) });
	int32_t goal = -1;
	while (!open.empty()) {
		const auto [cost, index] = open.top();
		open.pop();
		if (r_nodes.data(index).closed || cost > r_nodes.data(index).cost) {
			continue;
		}
		r_nodes.data(index).closed = true;
		const uint32_t cluster = r_nodes.data(index).cluster;
		if (cluster == p_to) {
			goal = index;
			break;
		}
		for (uint32_t portal = 0; portal < clusters_[cluster].number_of_portals; ++portal) {
			const uint32_t next = cluster_portal(cluster, portal).to_cluster;
			const MaterialId material = clusters_[next].material;
			if (!p_materials.is_valid_material(material)) {
				continue;
			}
			const float step = cost + portal_cost(cluster, portal) * p_materials.material_cost(material);
			int32_t node = r_nodes.find(next);
			if (node == -1) {
				node = r_nodes.new_oc_data(next);
			} else if (r_nodes.data(node).closed || r_nodes.data(node).cost <= step) {
				continue;
			}
			r_nodes.data(node).cost = step;
			r_nodes.data(node).parent = index;
			open.push({ step, node });
		}
	}
	if (goal == -1) {
		return -1.0f;
	}
	for (int32_t index = goal; index != -1; index = r_nodes.data(index).parent) {
		r_path.push_back(r_nodes.data(index).cluster);
	}
	std::reverse(r_path.begin(), r_path.end());
	return r_nodes.data(goal).cost;
}

}
