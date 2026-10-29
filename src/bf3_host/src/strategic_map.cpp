#include "strategic_map.h"

#include "tdm_bot_common.h"

#include "bf3/ai/nav/nav_clusters.h"
#include "bf3/ai/squad/squad_world.h"
#include "bf3/core/level/mode_areas.h"
#include "bf3/sim/nav.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace bf3::host {

namespace {

constexpr float kTdmAreaRadiusM = 40.0f;
constexpr uint32_t kLloydRounds = 16;

using tdm_bot::distance;

const char *area_kind_name(BotStrategicArea::Kind p_kind) {
	switch (p_kind) {
		case BotStrategicArea::Kind::Flag:
			return "flag";
		case BotStrategicArea::Kind::Hq:
			return "hq";
		case BotStrategicArea::Kind::Mcom:
			return "mcom";
		case BotStrategicArea::Kind::TdmCluster:
			return "tdm";
	}
	return "area";
}

uint32_t nearest_of(
		const std::vector<sim::Vec3> &p_points, const std::vector<uint32_t> &p_members, const sim::Vec3 &p_to) {
	uint32_t best = UINT32_MAX;
	float best_d = std::numeric_limits<float>::max();
	for (const uint32_t m : p_members) {
		const float d = ai::distance_sq(p_points[m], p_to);
		if (d < best_d) {
			best_d = d;
			best = m;
		}
	}
	return best;
}

std::vector<std::vector<uint32_t>> cluster_points(
		const std::vector<sim::Vec3> &p_points, const std::vector<uint8_t> &p_eligible, float p_radius) {
	std::vector<uint32_t> all(p_points.size());
	for (uint32_t i = 0; i < all.size(); ++i) {
		all[i] = i;
	}
	std::vector<uint32_t> eligible;
	for (uint32_t i = 0; i < p_points.size(); ++i) {
		if (p_eligible[i] != 0) {
			eligible.push_back(i);
		}
	}
	if (eligible.empty()) {
		return {};
	}
	sim::Vec3 mean{};
	for (const sim::Vec3 &p : p_points) {
		mean = { mean.x + p.x, mean.y + p.y, mean.z + p.z };
	}
	const float n = float(p_points.size());
	mean = { mean.x / n, mean.y / n, mean.z / n };
	std::vector<uint32_t> centres{ nearest_of(p_points, eligible, mean) };
	std::vector<float> reach(p_points.size(), std::numeric_limits<float>::max());
	while (true) {
		uint32_t far = UINT32_MAX;
		float far_d = -1.0f;
		for (uint32_t i = 0; i < p_points.size(); ++i) {
			reach[i] = std::min(reach[i], distance(p_points[i], p_points[centres.back()]));
			if (reach[i] > far_d) {
				far_d = reach[i];
				far = i;
			}
		}
		if (far_d <= p_radius || centres.size() >= eligible.size()) {
			break;
		}
		const uint32_t next = p_eligible[far] != 0 ? far : nearest_of(p_points, eligible, p_points[far]);
		if (std::find(centres.begin(), centres.end(), next) != centres.end()) {
			break;
		}
		centres.push_back(next);
	}
	std::vector<std::vector<uint32_t>> members;
	for (uint32_t round = 0; round < kLloydRounds; ++round) {
		members.assign(centres.size(), {});
		for (uint32_t i = 0; i < p_points.size(); ++i) {
			uint32_t best = 0;
			for (uint32_t c = 1; c < centres.size(); ++c) {
				if (ai::distance_sq(p_points[i], p_points[centres[c]]) <
						ai::distance_sq(p_points[i], p_points[centres[best]])) {
					best = c;
				}
			}
			members[best].push_back(i);
		}
		bool moved = false;
		for (size_t c = 0; c < centres.size(); ++c) {
			sim::Vec3 m{};
			for (const uint32_t i : members[c]) {
				m = { m.x + p_points[i].x, m.y + p_points[i].y, m.z + p_points[i].z };
			}
			const float k = float(std::max<size_t>(members[c].size(), 1));
			m = { m.x / k, m.y / k, m.z / k };
			std::vector<uint32_t> own_eligible;
			for (const uint32_t i : members[c]) {
				if (p_eligible[i] != 0) {
					own_eligible.push_back(i);
				}
			}
			const uint32_t next = nearest_of(p_points, own_eligible.empty() ? eligible : own_eligible, m);
			moved = moved || next != centres[c];
			centres[c] = next;
		}
		if (!moved) {
			break;
		}
	}
	for (size_t c = 0; c < members.size(); ++c) {
		members[c].insert(members[c].begin(), centres[c]);
		members[c].erase(std::unique(members[c].begin() + 1, members[c].end()), members[c].end());
		members[c].erase(std::remove(members[c].begin() + 1, members[c].end(), centres[c]), members[c].end());
	}
	members.erase(std::remove_if(
						  members.begin(), members.end(), [](const std::vector<uint32_t> &p_m) { return p_m.empty(); }),
			members.end());
	return members;
}

}

core::Status build_strategic_map(BotLevel &r_level, std::span<const core::level::ModeObjectiveDef> p_objectives) {
	if (r_level.nav == nullptr) {
		return core::make_error("strategic map: no nav world");
	}
	core::Result<ai::NavClusterMap> clusters = ai::build_nav_clusters(sim::nav_poly_graph(*r_level.nav));
	if (!clusters) {
		return core::error_at("strategic map", clusters.error());
	}
	r_level.clusters = std::make_shared<const ai::NavClusterMap>(std::move(*clusters));
	r_level.areas.clear();
	for (const BotPointRow &row : r_level.points) {
		if (row.kind == BotPointRow::Kind::Spawn) {
			continue;
		}
		BotStrategicArea area;
		area.kind = row.kind == BotPointRow::Kind::Hq ? BotStrategicArea::Kind::Hq
				: row.kind == BotPointRow::Kind::Mcom ? BotStrategicArea::Kind::Mcom
													  : BotStrategicArea::Kind::Flag;
		area.objective = row.index;
		area.centre = row.on_mesh ? row.check.nearest : row.point;
		area.poly = row.check.poly;
		area.radius = bot_point_radius(row.kind);
		area.team = row.index < p_objectives.size() ? p_objectives[row.index].team : 0;
		area.takeable = area.kind == BotStrategicArea::Kind::Flag;
		r_level.areas.push_back(std::move(area));
	}
	if (r_level.areas.empty()) {
		std::vector<sim::Vec3> points;
		std::vector<uint8_t> eligible;
		std::vector<const BotPointRow *> rows;
		for (const BotPointRow &row : r_level.points) {
			if (row.kind != BotPointRow::Kind::Spawn) {
				continue;
			}
			rows.push_back(&row);
			points.push_back(row.point);
			eligible.push_back(row.on_mesh && row.in_main ? 1 : 0);
		}
		for (const std::vector<uint32_t> &members : cluster_points(points, eligible, kTdmAreaRadiusM)) {
			const BotPointRow &centre = *rows[members.front()];
			BotStrategicArea area;
			area.kind = BotStrategicArea::Kind::TdmCluster;
			area.centre = centre.check.nearest;
			area.poly = centre.check.poly;
			for (const uint32_t m : members) {
				area.spawns.push_back(rows[m]->index);
				area.radius = std::max(area.radius, distance(points[m], centre.point));
			}
			area.radius = std::max(area.radius, bot_point_radius(BotPointRow::Kind::Spawn));
			std::sort(area.spawns.begin(), area.spawns.end());
			r_level.areas.push_back(std::move(area));
		}
	}
	for (size_t i = 0; i < r_level.areas.size(); ++i) {
		BotStrategicArea &area = r_level.areas[i];
		area.name = std::string(area_kind_name(area.kind)) + "_" + std::to_string(i);
		area.cluster = area.poly != 0 ? r_level.clusters->cluster_of(*r_level.nav, area.poly) : ai::kNoCluster;
	}
	return core::Status::success();
}

}
