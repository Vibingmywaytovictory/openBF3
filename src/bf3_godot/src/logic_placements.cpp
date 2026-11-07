#include "logic_placements.h"

#include "level_upload.h"

#include "bf3/sim/logic/placement_drivers.h"

#include <cstdio>

namespace bf3::godot_side {

void LogicPlacements::bind(const render::LevelScenePlan &p_plan, const core::gameplay::LogicGraph &p_graph) {
	plan_ = &p_plan;
	graph_ = &p_graph;
	nodes_ = sim::logic::bind_placement_drivers(p_graph, p_plan.drivers);
	values_.assign(p_plan.drivers.size(), 0xFF);
	instances_of_set_.assign(p_plan.driver_sets.size(), {});
	stats_ = LogicPlacementsStats{};
	stats_.drivers = p_plan.drivers.size();
	for (const uint32_t n : nodes_) {
		stats_.bound += n != core::gameplay::kNoLogicIndex ? 1 : 0;
	}
	for (uint32_t i = 0; i < p_plan.instances.size(); ++i) {
		const uint32_t set = p_plan.instances[i].drivers;
		if (set < instances_of_set_.size()) {
			instances_of_set_[set].push_back(i);
			++stats_.instances;
		}
	}
}

bool LogicPlacements::frame(
		const render::LevelScenePlan &p_plan, const sim::logic::LogicRuntime &p_runtime, LevelUpload &r_upload) {
	if (plan_ != &p_plan || graph_ != &p_runtime.graph()) {
		bind(p_plan, p_runtime.graph());
	}
	if (p_plan.drivers.empty()) {
		return false;
	}
	bool any = false;
	for (size_t d = 0; d < p_plan.drivers.size(); ++d) {
		const uint8_t v = sim::logic::placement_driver_value(p_runtime, nodes_[d], p_plan.drivers[d]) ? 1 : 0;
		if (v != values_[d]) {
			values_[d] = v;
			any = true;
		}
	}
	if (!any) {
		return false;
	}
	++stats_.changes;
	stats_.shown = 0;
	for (size_t s = 0; s < p_plan.driver_sets.size(); ++s) {
		bool visible = true;
		for (const uint32_t d : p_plan.driver_sets[s]) {
			visible = visible && values_[d] == 1;
		}
		for (const uint32_t i : instances_of_set_[s]) {
			r_upload.set_instance_visible(i, visible);
		}
		stats_.shown += visible ? instances_of_set_[s].size() : 0;
	}
	return true;
}

std::string LogicPlacements::describe() const {
	char line[192];
	std::snprintf(line, sizeof(line),
			"[level] logic placements: %zu drivers, %zu bound to the client logic, %zu of %zu instances shown, change "
			"%llu",
			stats_.drivers, stats_.bound, stats_.shown, stats_.instances, (unsigned long long)stats_.changes);
	return line;
}

}
