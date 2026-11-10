#include "bf3/core/ui/commo_rose.h"

#include <charconv>

namespace bf3::core::ui {

namespace {

std::optional<int32_t> param_of(const UiGraphNode &p_node) {
	int32_t v = 0;
	const char *end = p_node.param.data() + p_node.param.size();
	const auto r = std::from_chars(p_node.param.data(), end, v);
	if (r.ec != std::errc() || r.ptr != end) {
		return std::nullopt;
	}
	return v;
}

std::vector<uint32_t> targets_of(const UiGraphAsset &p_graph, uint32_t p_node, std::optional<uint32_t> p_port) {
	std::vector<uint32_t> out;
	for (const UiGraphConnection &c : p_graph.connections) {
		if (c.source_node == p_node && (!p_port || c.source_port == *p_port) && c.target_node < p_graph.nodes.size()) {
			out.push_back(c.target_node);
		}
	}
	return out;
}

bool sets(const UiGraphNode &p_node, int32_t p_key) {
	return p_node.kind == UiGraphNodeKind::DataSet && p_node.data.key == p_key;
}

struct Command {
	int32_t command = kCommoRoseNoCommand;
	int32_t button = -1;
};

std::optional<Command> command_from(const UiGraphAsset &p_graph, uint32_t p_set) {
	const UiGraphNode &set = p_graph.nodes[p_set];
	const std::optional<int32_t> command = param_of(set);
	if (!sets(set, kCommoRoseCommandKey) || !command) {
		return std::nullopt;
	}
	for (const uint32_t t : targets_of(p_graph, p_set, std::nullopt)) {
		const UiGraphNode &next = p_graph.nodes[t];
		if (const std::optional<int32_t> button = param_of(next); sets(next, kCommoRoseButtonKey) && button) {
			return Command{ *command, *button };
		}
	}
	return std::nullopt;
}

}

const CommoRoseEntry *CommoRoseLayout::find(std::string_view p_instance) const {
	for (const CommoRoseEntry &e : entries) {
		if (e.instance == p_instance) {
			return &e;
		}
	}
	return nullptr;
}

Result<CommoRoseLayout> read_commo_rose(const UiGraphAsset &p_screen) {
	CommoRoseLayout out;
	for (uint32_t n = 0; n < p_screen.nodes.size(); ++n) {
		const UiGraphNode &w = p_screen.nodes[n];
		if (w.kind != UiGraphNodeKind::Widget || w.name != "Button") {
			continue;
		}
		CommoRoseEntry e;
		e.instance = w.instance;
		if (const std::optional<uint32_t> over = w.port(UiGraphPortRole::Output, "Over")) {
			for (const uint32_t t : targets_of(p_screen, n, over)) {
				const UiGraphNode &next = p_screen.nodes[t];
				if (const std::optional<Command> c = command_from(p_screen, t)) {
					e.command_on_foot = e.command_in_vehicle = c->command;
					e.button = c->button;
				} else if (next.kind == UiGraphNodeKind::BinaryLogic) {
					for (const UiGraphPortRole role : { UiGraphPortRole::True, UiGraphPortRole::False }) {
						const std::optional<uint32_t> branch = next.port(role);
						for (const uint32_t s : branch ? targets_of(p_screen, t, branch) : std::vector<uint32_t>{}) {
							if (const std::optional<Command> b = command_from(p_screen, s)) {
								(role == UiGraphPortRole::True ? e.command_in_vehicle : e.command_on_foot) = b->command;
								e.button = b->button;
							}
						}
					}
				}
			}
		}
		if (const std::optional<uint32_t> released = w.port(UiGraphPortRole::Output, "Released")) {
			for (const uint32_t t : targets_of(p_screen, n, released)) {
				const UiGraphNode &next = p_screen.nodes[t];
				e.spot_and_order = e.spot_and_order ||
						(next.kind == UiGraphNodeKind::InstanceOutput && next.name == "SpotAndOrder");
			}
		}
		if (e.button < 0) {
			return make_error(p_screen.partition + ": " + e.instance + " sets no commo rose button on Over");
		}
		out.entries.push_back(std::move(e));
	}
	if (out.entries.empty()) {
		return make_error(p_screen.partition + ": no commo rose buttons");
	}
	return out;
}

}
