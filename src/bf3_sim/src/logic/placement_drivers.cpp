#include "bf3/sim/logic/placement_drivers.h"

#include <map>
#include <optional>
#include <utility>

namespace bf3::sim::logic {

namespace gp = core::gameplay;

std::vector<uint32_t> bind_placement_drivers(
		const LogicGraph &p_graph, std::span<const core::level::LogicDriverRef> p_drivers) {
	std::vector<uint32_t> out(p_drivers.size(), gp::kNoLogicIndex);
	const std::span<const gp::LogicScopeInfo> scopes = p_graph.scopes();
	std::optional<uint32_t> root;
	std::map<std::pair<uint32_t, core::Guid>, uint32_t> child;
	for (uint32_t s = 0; s < scopes.size(); ++s) {
		if (scopes[s].parent == gp::kNoLogicIndex) {
			if (!root) {
				root = s;
			}
			continue;
		}
		child.emplace(std::make_pair(scopes[s].parent, scopes[s].reference), s);
	}
	if (!root) {
		return out;
	}
	for (size_t d = 0; d < p_drivers.size(); ++d) {
		std::optional<uint32_t> scope = root;
		for (const core::Guid &reference : p_drivers[d].scope_path) {
			const auto found = child.find(std::make_pair(*scope, reference));
			scope = found != child.end() ? std::optional<uint32_t>(found->second) : std::nullopt;
			if (!scope) {
				break;
			}
		}
		if (!scope) {
			continue;
		}
		for (const uint32_t n : p_graph.nodes_with_guid(p_drivers[d].source)) {
			const gp::LogicNode &node = p_graph.nodes()[n];
			if (node.scope == *scope &&
					(node.type == gp::LogicTypeId::Bool || node.type == gp::LogicTypeId::SyncedBool)) {
				out[d] = n;
				break;
			}
		}
	}
	return out;
}

bool placement_driver_value(
		const LogicRuntime &p_runtime, uint32_t p_node, const core::level::LogicDriverRef &p_driver) {
	if (p_node == gp::kNoLogicIndex) {
		return p_driver.start;
	}
	const std::optional<LogicValue> v = p_runtime.output(p_node, p_driver.port);
	return v && v->kind == LogicValue::Kind::Bool ? v->b : p_driver.start;
}

}
