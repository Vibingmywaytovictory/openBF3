#pragma once

#include "bf3/ai/knowledge/ai_object.h"
#include "bf3/ai/path/material.h"
#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <vector>

namespace bf3::ai {

struct Cluster {
	uint32_t first_polygon = 0;
	uint32_t number_of_polygons = 0;
	uint16_t first_portal = 0;
	uint16_t number_of_portals = 0;
	MaterialId material = kMaterialGround;
};

struct ClusterPortal {
	Vec3 position{};
	uint32_t to_cluster = 0;
};

class ClusterNodeManager {
public:
	struct OCData {
		uint32_t cluster = 0;
		float cost = 0.0f;
		int32_t parent = -1;
		bool closed = false;
		int32_t next_in_bucket = -1;
		int32_t next_free = -1;
	};

	static constexpr size_t kOCDataBuckets = 313;

	ClusterNodeManager();

	int32_t new_oc_data(uint32_t p_cluster);
	void recycle_oc_data(int32_t p_index);
	int32_t find(uint32_t p_cluster) const;
	OCData &data(int32_t p_index) { return pool_[size_t(p_index)]; }
	const OCData &data(int32_t p_index) const { return pool_[size_t(p_index)]; }
	void reset();
	size_t live_count() const { return live_; }
	size_t capacity() const { return pool_.size(); }

private:
	std::vector<OCData> pool_;
	std::array<int32_t, kOCDataBuckets> buckets_{};
	int32_t free_ = -1;
	size_t live_ = 0;
};

class ClusterGraph {
public:
	core::Status assign(std::vector<Cluster> p_clusters, std::vector<uint32_t> p_cluster_polygons,
			std::vector<ClusterPortal> p_portals, std::vector<float> p_portal_costs);
	void clean();

	uint32_t number_of_clusters() const { return uint32_t(clusters_.size()); }
	uint32_t number_of_portals() const { return uint32_t(portals_.size()); }
	const Cluster &cluster(uint32_t p_index) const { return clusters_[p_index]; }
	const ClusterPortal &cluster_portal(uint32_t p_cluster, uint32_t p_portal) const;
	float portal_cost(uint32_t p_cluster, uint32_t p_portal) const;
	const std::vector<uint32_t> &cluster_polygons() const { return cluster_polygons_; }

	float shortest_path(uint32_t p_from, uint32_t p_to, const MaterialManager &p_materials, ClusterNodeManager &r_nodes,
			std::vector<uint32_t> &r_path) const;

private:
	std::vector<Cluster> clusters_;
	std::vector<uint32_t> cluster_polygons_;
	std::vector<ClusterPortal> portals_;
	std::vector<float> portal_costs_;
};

}
