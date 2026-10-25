#include "bf3/core/gameplay/death_timeline.h"

#include "bf3/core/gameplay/logic_types.h"

#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace bf3::core::gameplay {

namespace {

constexpr int32_t kDefaultValue = logic_port_id("DefaultValue");
constexpr int32_t kInputSelect = logic_port_id("InputSelect");
constexpr size_t kMaxGateHops = 8;
constexpr size_t kMaxPropertyHops = 8;

std::optional<std::string_view> graph_asset_of(const EbxResolver &p_db, const LogicGraph &p_graph, uint32_t p_node) {
	const LogicNode &n = p_graph.nodes()[p_node];
	if (n.scope >= p_graph.scopes().size()) {
		return std::nullopt;
	}
	const EbxPartition *part = p_db.partition_by_name(p_graph.text(p_graph.scopes()[n.scope].name));
	const std::optional<EbxInstance> inst = part != nullptr ? part->instance_by_guid(n.guid) : std::nullopt;
	const std::optional<EbxInstance> asset = inst ? p_db.follow(inst->data(), "GraphAsset") : std::nullopt;
	return asset ? p_db.name_of(asset->partition().guid()) : std::nullopt;
}

const LogicEdge *property_into(const LogicGraph &p_graph, uint32_t p_node, int32_t p_port) {
	for (const LogicEdge &e : p_graph.property_edges()) {
		if (e.target == p_node && e.target_port == p_port) {
			return &e;
		}
	}
	return nullptr;
}

std::optional<double> start_value(const LogicGraph &p_graph, uint32_t p_node, int32_t p_port, size_t p_hops) {
	if (p_hops > kMaxPropertyHops) {
		return std::nullopt;
	}
	if (const LogicEdge *in = property_into(p_graph, p_node, p_port)) {
		const LogicNode &src = p_graph.nodes()[in->source];
		switch (src.type) {
			case LogicTypeId::Float:
			case LogicTypeId::Int:
				return start_value(p_graph, in->source, kDefaultValue, p_hops + 1);
			case LogicTypeId::FloatHub: {
				const std::optional<double> sel = start_value(p_graph, in->source, kInputSelect, p_hops + 1);
				if (!sel || *sel < 0.0 || *sel > 15.0) {
					return std::nullopt;
				}
				const std::string port = "In" + std::to_string(int(*sel));
				return start_value(p_graph, in->source, logic_port_id(port), p_hops + 1);
			}
			default:
				return std::nullopt;
		}
	}
	const std::optional<LogicValue> v = p_graph.param(p_node, p_port);
	if (!v) {
		return std::nullopt;
	}
	switch (v->kind) {
		case LogicValue::Kind::Float:
			return v->f;
		case LogicValue::Kind::Int:
			return double(v->i);
		default:
			return std::nullopt;
	}
}

bool reaches(const LogicGraph &p_graph, uint32_t p_from, uint32_t p_target, int32_t p_port, size_t p_hops) {
	if (p_hops > kMaxGateHops) {
		return false;
	}
	for (const LogicEdge &e : p_graph.events_from(p_from)) {
		if (e.source_port != logic_port::kOut) {
			continue;
		}
		if (e.target == p_target && e.target_port == p_port) {
			return true;
		}
		if (p_graph.nodes()[e.target].type == LogicTypeId::EventGate && e.target_port == logic_port::kIn &&
				reaches(p_graph, e.target, p_target, p_port, p_hops + 1)) {
			return true;
		}
	}
	return false;
}

}

Result<DeathTimeline> read_death_timeline(const EbxResolver &p_db, const LogicGraph &p_client_graph) {
	const std::string where = "death timeline " + std::string(kDeathLogicScope);
	const std::optional<uint32_t> scope = p_client_graph.scope_named(kDeathLogicScope);
	if (!scope) {
		return make_error(where + ": no such scope in the client logic");
	}
	std::vector<uint32_t> killed;
	for (const uint32_t n : p_client_graph.nodes_of_type(LogicTypeId::UIGraph)) {
		if (p_client_graph.scope_within(p_client_graph.nodes()[n].scope, *scope) &&
				graph_asset_of(p_db, p_client_graph, n) == kKilledGraphAsset) {
			killed.push_back(n);
		}
	}
	if (killed.size() != 1) {
		return make_error(
				where + ": " + std::to_string(killed.size()) + " UIGraphs of " + std::string(kKilledGraphAsset));
	}
	const auto delay_into = [&](std::string_view p_input) -> Result<float> {
		const int32_t port = logic_port_id(p_input);
		std::optional<double> found;
		size_t hits = 0;
		for (const uint32_t d : p_client_graph.nodes_of_type(LogicTypeId::Delay)) {
			if (!p_client_graph.scope_within(p_client_graph.nodes()[d].scope, *scope) ||
					!reaches(p_client_graph, d, killed.front(), port, 0)) {
				continue;
			}
			++hits;
			found = start_value(p_client_graph, d, logic_port::kDelay, 0);
		}
		if (hits != 1 || !found || !std::isfinite(*found) || *found < 0.0 || *found > 3600.0) {
			return make_error(where + ": " + std::to_string(hits) + " Delays into the killed graph's " +
					std::string(p_input) + (hits == 1 ? " with no start value" : ""));
		}
		return float(*found);
	};
	const Result<float> card = delay_into(kKilledGraphDeathInput);
	if (!card) {
		return card.error();
	}
	const Result<float> spawn = delay_into(kKilledGraphDefragInput);
	if (!spawn) {
		return spawn.error();
	}
	DeathTimeline out;
	out.kill_card_s = *card;
	out.spawn_screens_s = *spawn;
	if (!(out.spawn_screens_s > out.kill_card_s)) {
		return make_error(where + ": the spawn screens do not come after the kill card");
	}
	return out;
}

}
