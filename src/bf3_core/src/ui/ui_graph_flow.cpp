#include "bf3/core/ui/ui_graph.h"

#include "bf3/core/util/text.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

namespace bf3::core::ui {

namespace {

constexpr size_t kMaxDepth = 256;

std::string leaf(std::string_view p_path) {
	return ui_data_category_leaf(p_path);
}

std::optional<double> number_of(std::string_view p_text) {
	if (p_text.empty()) {
		return std::nullopt;
	}
	const std::string s(p_text);
	char *end = nullptr;
	const double d = std::strtod(s.c_str(), &end);
	if (end == nullptr || *end != '\0' || !std::isfinite(d)) {
		return std::nullopt;
	}
	return d;
}

std::string data_text(const UiDataRef &p_ref) {
	return leaf(p_ref.category) + "#" + (p_ref.name.empty() ? std::to_string(p_ref.key) : p_ref.name);
}

std::string params_text(const std::vector<std::string> &p_params) {
	std::string s;
	for (size_t i = 0; i < p_params.size(); ++i) {
		s += (i == 0 ? "" : ",") + p_params[i];
	}
	return s;
}

bool operand_true(std::string_view p_op, double p_left, double p_right) {
	if (p_op.ends_with("GreaterThanOrEqual")) {
		return p_left >= p_right;
	}
	if (p_op.ends_with("LessThanOrEqual")) {
		return p_left <= p_right;
	}
	if (p_op.ends_with("GreaterThan")) {
		return p_left > p_right;
	}
	if (p_op.ends_with("LessThan")) {
		return p_left < p_right;
	}
	if (p_op.ends_with("NotEqual")) {
		return p_left != p_right;
	}
	return p_left == p_right;
}

}

std::string ui_flow_value_text(const std::optional<std::string> &p_value) {
	return p_value ? *p_value : std::string("-");
}

bool ui_flow_truthy(std::string_view p_value) {
	if (p_value.empty()) {
		return false;
	}
	if (iequals_ascii(p_value, "true")) {
		return true;
	}
	if (iequals_ascii(p_value, "false")) {
		return false;
	}
	const std::optional<double> n = number_of(p_value);
	return n ? *n != 0.0 : true;
}

struct UiFlow::Instance {
	std::string graph;
	std::shared_ptr<const UiGraphAsset> asset;
	uint64_t screen = 0;
	bool dead = false;
	std::map<uint32_t, std::vector<uint32_t>> out_edges;
};

UiFlow::UiFlow(UiFlowHost &r_host, UiGraphProvider p_graphs, const UiActionTable *p_actions) :
		host_(r_host), graphs_(std::move(p_graphs)), actions_(p_actions) {}

UiFlow::~UiFlow() = default;

void UiFlow::note(std::string p_line) {
	if (log_.size() < kUiFlowMaxLog) {
		log_.push_back(std::move(p_line));
	}
}

void UiFlow::error(std::string p_line) {
	if (errors_.size() < kUiFlowMaxLog) {
		errors_.push_back(std::move(p_line));
	}
}

UiFlow::Instance *UiFlow::instance_of(std::string_view p_graph, bool p_create) {
	for (const std::unique_ptr<Instance> &i : instances_) {
		if (!i->dead && i->screen == 0 && i->graph == p_graph) {
			return i.get();
		}
	}
	if (!p_create) {
		return nullptr;
	}
	std::shared_ptr<const UiGraphAsset> asset = graphs_ ? graphs_(p_graph) : nullptr;
	if (asset == nullptr) {
		error("no graph " + std::string(p_graph));
		return nullptr;
	}
	auto inst = std::make_unique<Instance>();
	inst->graph = std::string(p_graph);
	inst->asset = std::move(asset);
	for (uint32_t c = 0; c < inst->asset->connections.size(); ++c) {
		const UiGraphConnection &con = inst->asset->connections[c];
		inst->out_edges[(con.source_node << 16) | con.source_port].push_back(c);
	}
	instances_.push_back(std::move(inst));
	return instances_.back().get();
}

UiFlow::Instance *UiFlow::screen_instance(uint64_t p_screen) {
	for (const std::unique_ptr<Instance> &i : instances_) {
		if (!i->dead && i->screen == p_screen) {
			return i.get();
		}
	}
	return nullptr;
}

void UiFlow::begin_call() {
	if (calls_++ == 0) {
		budget_ = kUiFlowMaxStepsPerCall;
	}
}

void UiFlow::end_call() {
	if (--calls_ == 0) {
		instances_.erase(std::remove_if(instances_.begin(), instances_.end(),
								 [](const std::unique_ptr<Instance> &i) { return i->dead; }),
				instances_.end());
	}
}

const UiFlowScreen *UiFlow::screen(uint64_t p_id) const {
	for (const UiFlowScreen &s : screens_) {
		if (s.id == p_id) {
			return &s;
		}
	}
	return nullptr;
}

const UiFlowScreen *UiFlow::find_screen(std::string_view p_partition) const {
	for (auto it = screens_.rbegin(); it != screens_.rend(); ++it) {
		if (it->partition == p_partition && !it->exiting) {
			return &*it;
		}
	}
	return nullptr;
}

std::optional<uint64_t> UiFlow::focused() const {
	return focus_;
}

bool UiFlow::graph_live(std::string_view p_graph) const {
	for (const std::unique_ptr<Instance> &i : instances_) {
		if (!i->dead && i->screen == 0 && i->graph == p_graph) {
			return true;
		}
	}
	return false;
}

std::vector<uint64_t> UiFlow::take_opened() {
	std::vector<uint64_t> out;
	out.swap(opened_);
	return out;
}

std::vector<uint64_t> UiFlow::take_closing() {
	std::vector<uint64_t> out;
	out.swap(closing_);
	return out;
}

std::optional<std::string> UiFlow::read(const UiDataRef &p_ref, const std::optional<std::string> &p_incoming) {
	if (!p_ref.defined()) {
		return p_incoming;
	}
	return host_.data_get(p_ref);
}

void UiFlow::follow(Instance &r_inst, uint32_t p_node, uint32_t p_port, const std::optional<std::string> &p_value) {
	const auto it = r_inst.out_edges.find((p_node << 16) | p_port);
	if (it == r_inst.out_edges.end()) {
		return;
	}
	const std::vector<uint32_t> edges = it->second;
	const std::shared_ptr<const UiGraphAsset> asset = r_inst.asset;
	for (const uint32_t c : edges) {
		if (r_inst.dead) {
			return;
		}
		const UiGraphConnection &con = asset->connections[c];
		if (con.pop > 0) {
			pop(con.pop);
		}
		enter(r_inst, con.target_node, con.target_port, p_value);
	}
}

void UiFlow::refocus() {
	focus_.reset();
	for (auto it = screens_.rbegin(); it != screens_.rend(); ++it) {
		if (!it->exiting && it->shown && it->focusable) {
			focus_ = it->id;
			return;
		}
	}
}

void UiFlow::begin_close(size_t p_index) {
	UiFlowScreen &s = screens_[p_index];
	if (s.exiting) {
		return;
	}
	s.exiting = true;
	note("close " + leaf(s.partition));
	closing_.push_back(s.id);
	if (Instance *si = screen_instance(s.id)) {
		si->dead = true;
	}
	if (focus_ == s.id) {
		refocus();
	}
}

void UiFlow::pop(int32_t p_count) {
	note("pop " + std::to_string(p_count));
	for (int32_t n = 0; n < p_count; ++n) {
		bool popped = false;
		for (size_t i = screens_.size(); i-- > 0;) {
			if (!screens_[i].exiting) {
				begin_close(i);
				popped = true;
				break;
			}
		}
		if (!popped) {
			return;
		}
	}
}

UiFlowScreen *UiFlow::ensure_screen(Instance &r_inst, uint32_t p_node, bool p_shown, bool p_focus) {
	const UiGraphNode &n = r_inst.asset->nodes[p_node];
	UiFlowScreen *s = nullptr;
	for (UiFlowScreen &e : screens_) {
		if (!e.exiting && e.graph == r_inst.graph && e.state_node == p_node && e.partition == n.screen) {
			s = &e;
		}
	}
	if (s == nullptr) {
		if (screens_.size() >= kUiFlowMaxScreens) {
			error("more than " + std::to_string(kUiFlowMaxScreens) + " screens");
			return nullptr;
		}
		UiFlowScreen made;
		made.id = next_screen_++;
		made.partition = n.screen;
		made.graph = r_inst.graph;
		made.state = n.name;
		made.state_node = p_node;
		made.dialog = n.kind == UiGraphNodeKind::Dialog;
		made.modal = made.dialog || r_inst.asset->modal;
		made.shown = false;
		const uint64_t id = made.id;
		screens_.push_back(std::move(made));
		opened_.push_back(id);
		note("push " + leaf(n.screen));
		if (std::shared_ptr<const UiGraphAsset> sg = graphs_ ? graphs_(n.screen) : nullptr;
				sg != nullptr && sg->is_screen()) {
			auto inst = std::make_unique<Instance>();
			inst->graph = n.screen;
			inst->asset = std::move(sg);
			inst->screen = id;
			for (uint32_t c = 0; c < inst->asset->connections.size(); ++c) {
				const UiGraphConnection &con = inst->asset->connections[c];
				inst->out_edges[(con.source_node << 16) | con.source_port].push_back(c);
			}
			if (inst->asset->modal) {
				screens_.back().modal = true;
			}
			instances_.push_back(std::move(inst));
		}
		s = &screens_.back();
	}
	if (p_shown && !s->shown) {
		s->shown = true;
		++s->shown_count;
		note("show " + leaf(s->partition));
	} else if (!p_shown && s->shown) {
		s->shown = false;
		note("hide " + leaf(s->partition));
	}
	if (p_focus) {
		s->focusable = true;
		if (focus_ != s->id) {
			focus_ = s->id;
			note("focus " + leaf(s->partition));
		}
	} else if (!p_shown && focus_ == s->id) {
		refocus();
	}
	return s;
}

bool UiFlow::state_output(uint64_t p_screen, std::string_view p_port_name, const std::optional<std::string> &p_value) {
	const UiFlowScreen *s = screen(p_screen);
	if (s == nullptr) {
		return false;
	}
	Instance *owner = instance_of(s->graph, false);
	const uint32_t node = s->state_node;
	if (owner == nullptr || node >= owner->asset->nodes.size()) {
		return false;
	}
	const std::shared_ptr<const UiGraphAsset> asset = owner->asset;
	const UiGraphNode &n = asset->nodes[node];
	bool any = false;
	for (uint32_t p = 0; p < n.ports.size(); ++p) {
		const UiGraphPort &port = n.ports[p];
		if (port.role != UiGraphPortRole::Output || port.input_action >= 0) {
			continue;
		}
		const bool match = port.instance.empty() ? port.name == p_port_name : port.instance == p_port_name;
		if (match) {
			any = true;
			follow(*owner, node, p, p_value);
		}
	}
	return any;
}

void UiFlow::enter(Instance &r_inst, uint32_t p_node, uint32_t p_port, const std::optional<std::string> &p_value) {
	if (r_inst.dead) {
		return;
	}
	if (budget_ == 0) {
		++overflows_;
		error("step budget exhausted in " + r_inst.graph);
		return;
	}
	if (depth_ >= kMaxDepth) {
		++overflows_;
		error("graph depth over " + std::to_string(kMaxDepth) + " in " + r_inst.graph);
		return;
	}
	--budget_;
	++steps_;
	++depth_;
	const std::shared_ptr<const UiGraphAsset> asset = r_inst.asset;
	const UiGraphNode &n = asset->nodes[p_node];
	const UiGraphPort *port = p_port < n.ports.size() ? &n.ports[p_port] : nullptr;
	const auto first = [&](UiGraphPortRole p_role) {
		if (const std::optional<uint32_t> o = n.port(p_role)) {
			follow(r_inst, p_node, *o, p_value);
		}
	};
	switch (n.kind) {
		case UiGraphNodeKind::InstanceInput:
			first(UiGraphPortRole::Out);
			break;
		case UiGraphNodeKind::InstanceOutput: {
			note("output " + leaf(r_inst.graph) + "." + n.name);
			if (r_inst.screen != 0) {
				state_output(r_inst.screen, n.name, p_value);
			} else {
				const std::string graph = r_inst.graph;
				if (n.destroy_graph) {
					close_graph(graph);
				}
				host_.graph_output(graph, n.name, n.output_id, n.destroy_graph);
			}
			break;
		}
		case UiGraphNodeKind::State:
		case UiGraphNodeKind::Dialog: {
			if (port == nullptr) {
				break;
			}
			if (port->role == UiGraphPortRole::Hide) {
				ensure_screen(r_inst, p_node, false, false);
			} else if (port->role == UiGraphPortRole::Show) {
				ensure_screen(r_inst, p_node, true, false);
			} else if (port->role == UiGraphPortRole::In) {
				ensure_screen(r_inst, p_node, true, true);
			} else if (port->role == UiGraphPortRole::Input) {
				UiFlowScreen *s = ensure_screen(r_inst, p_node, true, true);
				if (s == nullptr) {
					break;
				}
				const uint64_t id = s->id;
				note("event " + leaf(s->partition) + "." + port->name);
				if (Instance *si = screen_instance(id)) {
					if (const std::optional<uint32_t> in =
									si->asset->find(UiGraphNodeKind::InstanceInput, port->name)) {
						enter(*si, *in, 0, p_value);
					}
				}
			}
			break;
		}
		case UiGraphNodeKind::Widget: {
			if (port == nullptr || port->role != UiGraphPortRole::Input || r_inst.screen == 0) {
				break;
			}
			const UiFlowScreen *s = screen(r_inst.screen);
			if (s == nullptr) {
				break;
			}
			note("widget " + leaf(s->partition) + "." + n.instance + " " + port->query);
			host_.widget_event(*s, n.instance, port->query, p_value);
			break;
		}
		case UiGraphNodeKind::Action: {
			UiFlowAction a;
			a.graph = r_inst.graph;
			a.node = n.name;
			a.key = n.action_key;
			const std::optional<std::string_view> name =
					actions_ != nullptr ? actions_->name_of(n.action_key) : std::nullopt;
			a.name = name ? std::string(*name) : std::to_string(n.action_key);
			a.params = n.params;
			a.incoming = p_value;
			if (!name) {
				error("action key " + std::to_string(n.action_key) + " of " + n.name + " in " + r_inst.graph +
						" is not in the action table");
			}
			note("action " + a.name + "(" + params_text(a.params) + ")" + (p_value ? " <" + *p_value : std::string()));
			host_.action(a);
			first(UiGraphPortRole::Out);
			break;
		}
		case UiGraphNodeKind::DataSet: {
			const std::string value = !n.param.empty() || n.set_empty ? n.param : p_value.value_or(std::string());
			note("set " + data_text(n.data) + "=" + value);
			host_.data_set(n.data, value, n.force_update);
			if (const std::optional<uint32_t> o = n.port(UiGraphPortRole::Out)) {
				follow(r_inst, p_node, *o, value);
			}
			break;
		}
		case UiGraphNodeKind::DataGet: {
			const std::optional<std::string> v = host_.data_get(n.data);
			note("get " + data_text(n.data) + "=" + ui_flow_value_text(v));
			if (const std::optional<uint32_t> o = n.port(UiGraphPortRole::Out)) {
				follow(r_inst, p_node, *o, v);
			}
			break;
		}
		case UiGraphNodeKind::DataToggle: {
			const std::optional<std::string> v = host_.data_get(n.data);
			const std::string value = v && ui_flow_truthy(*v) ? "False" : "True";
			note("toggle " + data_text(n.data) + "=" + value);
			host_.data_set(n.data, value, false);
			if (const std::optional<uint32_t> o = n.port(UiGraphPortRole::Out)) {
				follow(r_inst, p_node, *o, value);
			}
			break;
		}
		case UiGraphNodeKind::BinaryLogic: {
			const std::optional<std::string> v = read(n.data, p_value);
			const bool t = v && ui_flow_truthy(*v);
			note("test " + n.name + "=" + (t ? "True" : "False"));
			first(t ? UiGraphPortRole::True : UiGraphPortRole::False);
			break;
		}
		case UiGraphNodeKind::ComparisonLogic: {
			const std::optional<std::string> v = read(n.data, p_value);
			std::string s = v.value_or(std::string());
			if (n.skip_fractionals) {
				if (const std::optional<double> d = number_of(s)) {
					s = std::to_string(int64_t(std::trunc(*d)));
				}
			}
			std::optional<uint32_t> hit;
			std::optional<uint32_t> fallback;
			for (uint32_t p = 0; p < n.ports.size(); ++p) {
				if (n.ports[p].role != UiGraphPortRole::Output) {
					continue;
				}
				if (n.ports[p].name == s && !hit) {
					hit = p;
				}
				if (n.ports[p].name == "default" && !fallback) {
					fallback = p;
				}
			}
			const std::optional<uint32_t> chosen = hit ? hit : fallback;
			note("compare " + n.name + "=" + s + " -> " + (chosen ? n.ports[*chosen].name : std::string("none")));
			if (chosen) {
				follow(r_inst, p_node, *chosen, p_value);
			}
			break;
		}
		case UiGraphNodeKind::OperandLogic: {
			const std::optional<std::string> l = read(n.data, p_value);
			const std::optional<std::string> r = n.right.defined() ? host_.data_get(n.right) : std::nullopt;
			const double lv = l ? number_of(*l).value_or(ui_flow_truthy(*l) ? 1.0 : 0.0) : 0.0;
			const double rv = r ? number_of(*r).value_or(0.0) : n.literal;
			const bool t = operand_true(n.op, lv, rv);
			note("operand " + n.name + "=" + (t ? "True" : "False"));
			first(t ? UiGraphPortRole::True : UiGraphPortRole::False);
			break;
		}
		case UiGraphNodeKind::Splitter: {
			for (uint32_t p = 0; p < n.ports.size() && !r_inst.dead; ++p) {
				if (n.ports[p].role == UiGraphPortRole::Output) {
					follow(r_inst, p_node, p, p_value);
				}
			}
			break;
		}
		case UiGraphNodeKind::Unknown:
			error("node " + n.name + " of type " + n.type + " in " + r_inst.graph + " is not run");
			break;
	}
	--depth_;
}

bool UiFlow::fire_input(std::string_view p_graph, std::string_view p_input, std::optional<std::string> p_value) {
	begin_call();
	Instance *inst = instance_of(p_graph, true);
	bool found = false;
	if (inst != nullptr) {
		if (const std::optional<uint32_t> n = inst->asset->find(UiGraphNodeKind::InstanceInput, p_input)) {
			note("input " + leaf(p_graph) + "." + std::string(p_input));
			found = true;
			enter(*inst, *n, 0, p_value);
		} else {
			error("graph " + std::string(p_graph) + " has no input " + std::string(p_input));
		}
	}
	end_call();
	return found;
}

bool UiFlow::widget_fired(
		uint64_t p_screen, std::string_view p_instance, std::string_view p_query, std::optional<std::string> p_value) {
	begin_call();
	bool found = false;
	if (Instance *si = screen_instance(p_screen)) {
		const std::shared_ptr<const UiGraphAsset> asset = si->asset;
		note("fired " + leaf(si->graph) + "." + std::string(p_instance) + " " + std::string(p_query));
		for (uint32_t i = 0; i < asset->nodes.size() && !si->dead; ++i) {
			const UiGraphNode &n = asset->nodes[i];
			if (n.kind != UiGraphNodeKind::Widget || n.instance != p_instance) {
				continue;
			}
			for (uint32_t p = 0; p < n.ports.size() && !si->dead; ++p) {
				if (n.ports[p].role == UiGraphPortRole::Output && n.ports[p].query == p_query) {
					found = true;
					follow(*si, i, p, p_value);
				}
			}
		}
	}
	end_call();
	return found;
}

bool UiFlow::input_action(int32_t p_action) {
	begin_call();
	std::vector<uint64_t> order;
	if (focus_) {
		order.push_back(*focus_);
	}
	for (auto it = screens_.rbegin(); it != screens_.rend(); ++it) {
		if (!it->exiting && it->shown && (order.empty() || it->id != order.front())) {
			order.push_back(it->id);
		}
	}
	bool found = false;
	for (const uint64_t target : order) {
		const UiFlowScreen *s = screen(target);
		if (s == nullptr || s->exiting) {
			continue;
		}
		const bool modal = s->modal;
		Instance *owner = instance_of(s->graph, false);
		if (owner != nullptr && s->state_node < owner->asset->nodes.size()) {
			const std::shared_ptr<const UiGraphAsset> asset = owner->asset;
			const uint32_t node = s->state_node;
			const UiGraphNode &n = asset->nodes[node];
			const std::string where = leaf(s->partition);
			for (uint32_t p = 0; p < n.ports.size() && !owner->dead; ++p) {
				if (n.ports[p].role == UiGraphPortRole::Output && n.ports[p].input_action == p_action) {
					if (!found) {
						note("concept " + where + " " + n.ports[p].input_event);
					}
					found = true;
					follow(*owner, node, p, std::nullopt);
				}
			}
		}
		if (found || modal) {
			break;
		}
	}
	end_call();
	return found;
}

void UiFlow::screen_initialized(uint64_t p_screen) {
	begin_call();
	for (UiFlowScreen &s : screens_) {
		if (s.id == p_screen && !s.initialized && !s.exiting) {
			s.initialized = true;
			note("initialized " + leaf(s.partition));
			state_output(p_screen, kUiScreenInitializedPort, std::nullopt);
			break;
		}
	}
	end_call();
}

void UiFlow::screen_exited(uint64_t p_screen) {
	screens_.erase(std::remove_if(screens_.begin(), screens_.end(),
						   [&](const UiFlowScreen &s) { return s.id == p_screen && s.exiting; }),
			screens_.end());
}

bool UiFlow::close_graph(std::string_view p_graph) {
	Instance *inst = instance_of(p_graph, false);
	if (inst == nullptr) {
		return false;
	}
	begin_call();
	inst->dead = true;
	for (size_t i = 0; i < screens_.size(); ++i) {
		if (screens_[i].graph == p_graph && !screens_[i].exiting) {
			begin_close(i);
		}
	}
	end_call();
	return true;
}

}
