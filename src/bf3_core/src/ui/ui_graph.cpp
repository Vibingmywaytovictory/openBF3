#include "bf3/core/ui/ui_graph.h"

#include "ui_ebx.h"

#include "bf3/core/ebx.h"

#include <algorithm>
#include <map>
#include <unordered_map>

namespace bf3::core::ui {

namespace {

using detail::text_of;

bool flag_of(const EbxInstance &p_i, std::string_view p_field) {
	const std::optional<EbxValue> v = p_i.get(p_field);
	return v && v->as_bool().value_or(false);
}

int32_t i32_of(const EbxInstance &p_i, std::string_view p_field) {
	const std::optional<EbxValue> v = p_i.get(p_field);
	return v ? v->as_i32().value_or(0) : 0;
}

UiDataRef data_of(const EbxResolver &p_db, const EbxPartition &p_part, const std::optional<EbxValue> &p_value) {
	UiDataRef out;
	const std::optional<EbxStruct> s = p_value ? p_value->as_struct() : std::nullopt;
	if (!s) {
		return out;
	}
	out.name = text_of(s->get("DataName"));
	const std::optional<EbxValue> key = s->get("DataKey");
	out.key = key ? key->as_i32().value_or(0) : 0;
	if (const std::optional<EbxRef> c = detail::ref(*s, "DataCategory")) {
		out.category = detail::ref_partition(p_db, p_part, *c).value_or(std::string());
	}
	return out;
}

UiGraphNodeKind kind_of(const EbxInstance &p_i) {
	struct Entry {
		std::string_view type;
		UiGraphNodeKind kind;
	};
	static constexpr Entry kKinds[] = {
		{ "InstanceInputNode", UiGraphNodeKind::InstanceInput },
		{ "InstanceOutputNode", UiGraphNodeKind::InstanceOutput },
		{ "DialogNode", UiGraphNodeKind::Dialog },
		{ "StateNode", UiGraphNodeKind::State },
		{ "WidgetNode", UiGraphNodeKind::Widget },
		{ "ActionNode", UiGraphNodeKind::Action },
		{ "DataSetNode", UiGraphNodeKind::DataSet },
		{ "DataGetNode", UiGraphNodeKind::DataGet },
		{ "DataToggleNode", UiGraphNodeKind::DataToggle },
		{ "DataIncrementNode", UiGraphNodeKind::DataIncrement },
		{ "DataStepNode", UiGraphNodeKind::DataStep },
		{ "BinaryLogicNode", UiGraphNodeKind::BinaryLogic },
		{ "ComparisonLogicNode", UiGraphNodeKind::ComparisonLogic },
		{ "OperandLogicNode", UiGraphNodeKind::OperandLogic },
		{ "SplitterNode", UiGraphNodeKind::Splitter },
	};
	for (const Entry &e : kKinds) {
		if (p_i.is_a(e.type)) {
			return e.kind;
		}
	}
	return UiGraphNodeKind::Unknown;
}

struct PortReader {
	const EbxPartition &part;
	UiGraphNode &node;
	std::unordered_map<uint32_t, uint32_t> &ports;
	uint32_t node_index;
	std::string error;

	void add(const EbxRef &p_ref, UiGraphPortRole p_role) {
		if (p_ref.kind != EbxRef::Kind::Internal) {
			return;
		}
		const std::optional<EbxInstance> p = part.resolve(p_ref);
		if (!p) {
			return;
		}
		if (node.ports.size() >= kUiGraphMaxPortsPerNode) {
			error = "node " + node.name + " has more than " + std::to_string(kUiGraphMaxPortsPerNode) + " ports";
			return;
		}
		UiGraphPort port;
		port.role = p_role;
		port.name = text_of(p->get("Name"));
		port.instance = text_of(p->get("InstanceName"));
		port.query = text_of(p->get("Query"));
		if (port.query == "UIWidgetEventID_None") {
			port.query.clear();
		}
		if (const std::optional<EbxValue> ev = p->get("InputEventType")) {
			port.input_event = text_of(ev);
			port.input_action = ev->enum_value().value_or(-1);
		}
		ports[p->index()] = (node_index << 16) | uint32_t(node.ports.size());
		node.ports.push_back(std::move(port));
	}
	void one(const EbxInstance &p_i, std::string_view p_field, UiGraphPortRole p_role) {
		if (const std::optional<EbxRef> r = detail::ref(p_i.data(), p_field)) {
			add(*r, p_role);
		}
	}
	void many(const EbxInstance &p_i, std::string_view p_field, UiGraphPortRole p_role) {
		const std::optional<EbxValue> v = p_i.get(p_field);
		const OptionalEbxArray arr = v ? v->as_array() : OptionalEbxArray();
		if (!arr) {
			return;
		}
		for (const EbxValue &e : *arr) {
			if (const std::optional<EbxRef> r = e.as_ref()) {
				add(*r, p_role);
			}
		}
	}
};

}

const char *ui_graph_node_kind_name(UiGraphNodeKind p_kind) {
	switch (p_kind) {
		case UiGraphNodeKind::InstanceInput:
			return "InstanceInput";
		case UiGraphNodeKind::InstanceOutput:
			return "InstanceOutput";
		case UiGraphNodeKind::State:
			return "State";
		case UiGraphNodeKind::Dialog:
			return "Dialog";
		case UiGraphNodeKind::Widget:
			return "Widget";
		case UiGraphNodeKind::Action:
			return "Action";
		case UiGraphNodeKind::DataSet:
			return "DataSet";
		case UiGraphNodeKind::DataGet:
			return "DataGet";
		case UiGraphNodeKind::DataToggle:
			return "DataToggle";
		case UiGraphNodeKind::DataIncrement:
			return "DataIncrement";
		case UiGraphNodeKind::DataStep:
			return "DataStep";
		case UiGraphNodeKind::BinaryLogic:
			return "BinaryLogic";
		case UiGraphNodeKind::ComparisonLogic:
			return "ComparisonLogic";
		case UiGraphNodeKind::OperandLogic:
			return "OperandLogic";
		case UiGraphNodeKind::Splitter:
			return "Splitter";
		case UiGraphNodeKind::Unknown:
			break;
	}
	return "Unknown";
}

std::string ui_data_category_leaf(std::string_view p_category) {
	const size_t slash = p_category.rfind('/');
	return std::string(slash == std::string_view::npos ? p_category : p_category.substr(slash + 1));
}

std::optional<uint32_t> UiGraphNode::port(UiGraphPortRole p_role, std::string_view p_name) const {
	for (uint32_t i = 0; i < ports.size(); ++i) {
		if (ports[i].role == p_role && (p_name.empty() || ports[i].name == p_name)) {
			return i;
		}
	}
	return std::nullopt;
}

std::optional<uint32_t> UiGraphAsset::find(UiGraphNodeKind p_kind, std::string_view p_name) const {
	for (uint32_t i = 0; i < nodes.size(); ++i) {
		if (nodes[i].kind == p_kind && nodes[i].name == p_name) {
			return i;
		}
	}
	return std::nullopt;
}

Result<UiGraphAsset> read_ui_graph_asset(const EbxResolver &p_db, std::string_view p_partition) {
	const EbxPartition *part = p_db.partition_by_name(p_partition);
	if (part == nullptr) {
		return make_error("ui graph: no partition " + std::string(p_partition));
	}
	const EbxInstance root = part->primary();
	if (!root.is_a("UIGraphAsset") && !root.is_a("UIScreenAsset")) {
		return make_error("ui graph: " + std::string(p_partition) + " is a " + std::string(root.type_name()) +
				", not a UIGraphAsset or UIScreenAsset");
	}
	UiGraphAsset out;
	out.partition = std::string(p_partition);
	out.type = root.is_a("UIScreenAsset") ? "UIScreenAsset" : "UIGraphAsset";
	out.name = text_of(root.get("Name"));
	out.modal = flag_of(root, "Modal");
	out.protect_screens = flag_of(root, "ProtectScreens");
	std::unordered_map<uint32_t, uint32_t> node_of;
	std::unordered_map<uint32_t, uint32_t> ports;
	const std::optional<EbxValue> nodes = root.get("Nodes");
	const OptionalEbxArray node_refs = nodes ? nodes->as_array() : OptionalEbxArray();
	if (node_refs) {
		if (node_refs->size() > kUiGraphMaxNodes) {
			return make_error(
					"ui graph: " + std::string(p_partition) + " has " + std::to_string(node_refs->size()) + " nodes");
		}
		for (const EbxValue &v : *node_refs) {
			const std::optional<EbxRef> r = v.as_ref();
			if (!r || r->kind != EbxRef::Kind::Internal) {
				continue;
			}
			const std::optional<EbxInstance> n = part->resolve(*r);
			if (!n) {
				continue;
			}
			UiGraphNode node;
			node.kind = kind_of(*n);
			node.type = std::string(n->type_name());
			node.name = text_of(n->get("Name"));
			node.instance = text_of(n->get("InstanceName"));
			node.root = flag_of(*n, "IsRootNode");
			const uint32_t index = uint32_t(out.nodes.size());
			PortReader pr{ *part, node, ports, index, {} };
			pr.one(*n, "In", UiGraphPortRole::In);
			pr.one(*n, "Out", UiGraphPortRole::Out);
			pr.one(*n, "Show", UiGraphPortRole::Show);
			pr.one(*n, "Hide", UiGraphPortRole::Hide);
			pr.one(*n, "True", UiGraphPortRole::True);
			pr.one(*n, "False", UiGraphPortRole::False);
			pr.many(*n, "Inputs", UiGraphPortRole::Input);
			pr.many(*n, "Outputs", UiGraphPortRole::Output);
			if (!pr.error.empty()) {
				return make_error("ui graph: " + std::string(p_partition) + ": " + pr.error);
			}
			if (const std::optional<EbxRef> s = detail::ref(n->data(), "Screen")) {
				node.screen = detail::ref_partition(p_db, *part, *s).value_or(std::string());
			}
			node.action_key = i32_of(*n, "ActionKey");
			if (const std::optional<EbxValue> params = n->get("Params")) {
				if (const OptionalEbxArray arr = params->as_array()) {
					if (arr->size() > kUiGraphMaxParams) {
						return make_error("ui graph: " + std::string(p_partition) + ": node " + node.name + " has " +
								std::to_string(arr->size()) + " params");
					}
					for (const EbxValue &p : *arr) {
						node.params.emplace_back(p.as_str().value_or(std::string_view()));
					}
				}
			}
			node.append_params = flag_of(*n, "AppendIncomingParams");
			if (const std::optional<EbxValue> inputs = n->get("DataInputs")) {
				if (const OptionalEbxArray arr = inputs->as_array()) {
					for (const EbxValue &d : *arr) {
						node.data_inputs.push_back(data_of(p_db, *part, d));
					}
				}
			}
			for (const std::string_view f : { "DataSource", "DataSourceInfo", "LeftDataSourceInfo" }) {
				if (const std::optional<EbxValue> d = n->get(f)) {
					node.data = data_of(p_db, *part, d);
				}
			}
			node.right = data_of(p_db, *part, n->get("RightDataSourceInfo"));
			node.param = text_of(n->get("Param"));
			node.force_update = flag_of(*n, "ForceUpdate");
			node.set_empty = flag_of(*n, "SetToEmptyString");
			node.skip_fractionals = flag_of(*n, "SkipFractionals");
			node.op = text_of(n->get("Operator"));
			if (const std::optional<EbxValue> lit = n->get("RightLiteralOperand")) {
				node.literal = detail::number(lit).value_or(0.0);
			}
			node.current_step = data_of(p_db, *part, n->get("CurrentStepDataSource"));
			node.increment = detail::number(n->get("IncrementSize")).value_or(0.0);
			node.minimum = detail::number(n->get("Min")).value_or(0.0);
			node.maximum = detail::number(n->get("Max")).value_or(0.0);
			if (const std::optional<EbxValue> steps = n->get("Steps")) {
				if (const OptionalEbxArray arr = steps->as_array()) {
					if (arr->size() > kUiGraphMaxSteps) {
						return make_error("ui graph: " + std::string(p_partition) + ": node " + node.name + " has " +
								std::to_string(arr->size()) + " steps");
					}
					for (const EbxValue &step : *arr) {
						node.steps.push_back(step.as_f32().value_or(0.0f));
					}
				}
			}
			node.output_id = i32_of(*n, "Id");
			node.destroy_graph = flag_of(*n, "DestroyGraph");
			node.dialog_title = text_of(n->get("DialogTitle"));
			node.dialog_text = text_of(n->get("DialogText"));
			if (const std::optional<EbxValue> buttons = n->get("Buttons")) {
				if (const OptionalEbxArray arr = buttons->as_array()) {
					if (arr->size() > kUiGraphMaxParams) {
						return make_error("ui graph: " + std::string(p_partition) + ": dialog " + node.name + " has " +
								std::to_string(arr->size()) + " buttons");
					}
					for (const EbxValue &b : *arr) {
						const std::optional<EbxStruct> s = b.as_struct();
						if (!s) {
							continue;
						}
						const std::optional<EbxValue> c = s->get("InputConcept");
						node.dialog_buttons.push_back(
								UiDialogButton{ text_of(s->get("Label")), c ? c->enum_value().value_or(-1) : -1 });
					}
				}
			}
			node_of[n->index()] = index;
			out.nodes.push_back(std::move(node));
		}
	}
	const std::optional<EbxValue> conns = root.get("Connections");
	const OptionalEbxArray conn_refs = conns ? conns->as_array() : OptionalEbxArray();
	if (conn_refs) {
		if (conn_refs->size() > kUiGraphMaxConnections) {
			return make_error("ui graph: " + std::string(p_partition) + " has " + std::to_string(conn_refs->size()) +
					" connections");
		}
		for (const EbxValue &v : *conn_refs) {
			const std::optional<EbxRef> r = v.as_ref();
			const std::optional<EbxInstance> c =
					r && r->kind == EbxRef::Kind::Internal ? part->resolve(*r) : std::nullopt;
			if (!c) {
				continue;
			}
			const std::optional<EbxRef> sn = detail::ref(c->data(), "SourceNode");
			const std::optional<EbxRef> sp = detail::ref(c->data(), "SourcePort");
			const std::optional<EbxRef> tn = detail::ref(c->data(), "TargetNode");
			const std::optional<EbxRef> tp = detail::ref(c->data(), "TargetPort");
			if (!sn || !sp || !tn || !tp || sn->kind != EbxRef::Kind::Internal || sp->kind != EbxRef::Kind::Internal ||
					tn->kind != EbxRef::Kind::Internal || tp->kind != EbxRef::Kind::Internal) {
				continue;
			}
			const auto s_node = node_of.find(sn->index);
			const auto t_node = node_of.find(tn->index);
			const auto s_port = ports.find(sp->index);
			const auto t_port = ports.find(tp->index);
			if (s_node == node_of.end() || t_node == node_of.end()) {
				return make_error(
						"ui graph: " + std::string(p_partition) + " has a connection to a node outside its node list");
			}
			const auto port_index = [&](const std::unordered_map<uint32_t, uint32_t>::const_iterator &p_it,
											const EbxRef &p_ref, uint32_t p_node, bool p_source,
											bool &r_by_name) -> std::optional<uint32_t> {
				if (p_it != ports.end() && (p_it->second >> 16) == p_node) {
					return p_it->second & 0xffffu;
				}
				const std::optional<EbxInstance> pi = part->resolve(p_ref);
				if (!pi) {
					return std::nullopt;
				}
				const std::string name = text_of(pi->get("Name"));
				const UiGraphNode &n = out.nodes[p_node];
				for (uint32_t i = 0; i < n.ports.size(); ++i) {
					const UiGraphPortRole role = n.ports[i].role;
					const bool source_role = role == UiGraphPortRole::Out || role == UiGraphPortRole::Output ||
							role == UiGraphPortRole::True || role == UiGraphPortRole::False;
					if (source_role == p_source && n.ports[i].name == name) {
						r_by_name = true;
						return i;
					}
				}
				return std::nullopt;
			};
			bool by_name = false;
			const std::optional<uint32_t> source_port = port_index(s_port, *sp, s_node->second, true, by_name);
			const std::optional<uint32_t> target_port = port_index(t_port, *tp, t_node->second, false, by_name);
			if (!source_port || !target_port) {
				++out.unlisted_port_connections;
				continue;
			}
			UiGraphConnection con;
			con.source_node = s_node->second;
			con.source_port = *source_port;
			con.target_node = t_node->second;
			con.target_port = *target_port;
			con.pop = i32_of(*c, "NumScreensToPop");
			if (std::find(out.connections.begin(), out.connections.end(), con) != out.connections.end()) {
				++out.duplicate_connections;
				continue;
			}
			out.named_port_connections += by_name ? 1u : 0u;
			out.connections.push_back(con);
		}
	}
	return out;
}

int32_t ui_action_key(std::string_view p_name) {
	return int32_t(ebx_name_hash(p_name));
}

UiActionTable UiActionTable::of(std::vector<std::string> p_names) {
	UiActionTable t;
	t.names_ = std::move(p_names);
	for (uint32_t i = 0; i < t.names_.size(); ++i) {
		t.by_key_.emplace_back(ui_action_key(t.names_[i]), i);
	}
	std::sort(t.by_key_.begin(), t.by_key_.end());
	return t;
}

Result<UiActionTable> UiActionTable::read(const EbxResolver &p_db, std::string_view p_partition) {
	const EbxPartition *part = p_db.partition_by_name(p_partition);
	if (part == nullptr) {
		return make_error("ui actions: no partition " + std::string(p_partition));
	}
	const EbxInstance root = part->primary();
	if (!root.is_a("UIActionData")) {
		return make_error("ui actions: " + std::string(p_partition) + " is a " + std::string(root.type_name()) +
				", not a UIActionData");
	}
	const std::optional<EbxValue> keys = root.get("ActionKeys");
	const OptionalEbxArray arr = keys ? keys->as_array() : OptionalEbxArray();
	if (!arr) {
		return make_error("ui actions: " + std::string(p_partition) + " has no ActionKeys");
	}
	if (arr->size() > kUiActionMaxKeys) {
		return make_error("ui actions: " + std::string(p_partition) + " has " + std::to_string(arr->size()) + " keys");
	}
	std::vector<std::string> names;
	for (const EbxValue &v : *arr) {
		names.emplace_back(v.as_str().value_or(std::string_view()));
	}
	return of(std::move(names));
}

std::optional<std::string_view> UiActionTable::name_of(int32_t p_key) const {
	const auto it = std::lower_bound(by_key_.begin(), by_key_.end(), std::pair<int32_t, uint32_t>(p_key, 0));
	if (it == by_key_.end() || it->first != p_key) {
		return std::nullopt;
	}
	return std::string_view(names_[it->second]);
}

}
