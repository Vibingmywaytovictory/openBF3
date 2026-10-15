// S4 level logic: the instantiated, flattened logic graph (docs/design/s4.md §5). See logic_graph.h for the rules.
//
// --- Cost model (hostile input; the rule of logic_world.cpp) -----------------------------------------------------
// One WorkBudget covers the walk (LogicWorld's own charges) and this build. Every loop below charges before it runs:
//   scopes                1 unit each, plus the scope name copied (by length) when it is first interned
//   descriptors           1 unit per scope (the blueprint's Descriptor lookup)
//   objects -> nodes      1 unit per object examined (type, Realm), the type name interned (by length, once per name)
//   params                every effective() query charges its own steps (LogicWorld); every array element and
//                         struct member read 1 unit; arrays above kMaxParamElements fail the build
//   connection arrays     1 unit per blueprint, then the raw size of each of its three arrays before the scan
//   index lookups         binary searches over sorted vectors keyed by (scope, partition address, instance index)
//                         and std::map lookups (O(log n), never a hash of data-controlled keys)
//   collapse              1 unit per hop taken; on-path marks make interface cycles stop; a start edge that yields
//                         more than max_fanout edges, or a chain deeper than max_interface_depth, fails the build
//   constants             1 unit per descriptor field scanned (once per scope), the constant's length to parse it
//   emitted               1 unit per node, param, input and edge
// finalize() sorts (n log n) what was charged and validates every index.
#include "bf3/core/gameplay/logic_graph.h"

#include "bf3/core/gameplay/logic_world.h"
#include "gameplay_read.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <tuple>

namespace bf3::core::gameplay {

namespace {

using K = LogicValue::Kind;
using Shape = LogicParamField::Shape;

constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001b3ull;
constexpr uint64_t kRootPath = 0x53344c6f67696331ull; // "S4Logic1"

struct Fnv {
	uint64_t h = kFnvOffset;
	void byte(uint8_t p_b) {
		h ^= p_b;
		h *= kFnvPrime;
	}
	void u32(uint32_t p_v) {
		for (int i = 0; i < 4; ++i) {
			byte(static_cast<uint8_t>(p_v >> (8 * i)));
		}
	}
	void u64(uint64_t p_v) {
		for (int i = 0; i < 8; ++i) {
			byte(static_cast<uint8_t>(p_v >> (8 * i)));
		}
	}
	void i32(int32_t p_v) { u32(static_cast<uint32_t>(p_v)); }
	void f64(double p_v) {
		uint64_t bits = 0;
		std::memcpy(&bits, &p_v, 8);
		u64(bits);
	}
	void f32(float p_v) {
		uint32_t bits = 0;
		std::memcpy(&bits, &p_v, 4);
		u32(bits);
	}
	void str(std::string_view p_text) {
		u64(p_text.size());
		for (const char c : p_text) {
			byte(static_cast<uint8_t>(c));
		}
	}
};

uint64_t le64(const uint8_t *p_bytes) {
	uint64_t v = 0;
	for (int i = 7; i >= 0; --i) {
		v = (v << 8) | p_bytes[i];
	}
	return v;
}

uint64_t guid_word(const Guid &p_guid) {
	return hash_mix(le64(p_guid.bytes.data()) ^ hash_mix(le64(p_guid.bytes.data() + 8) ^ 0x9E3779B97F4A7C15ull));
}

// One step of a stable, unkeyed hash chain (never hash_keyed: its seed differs per process).
uint64_t chain(uint64_t p_prev, uint64_t p_word) {
	return hash_mix(hash_mix(p_prev ^ 0x9E3779B97F4A7C15ull) ^ p_word);
}

// --- param field table (docs/design/s4.md §6.2) -------------------------------------------------------------------

constexpr LogicParamField kDelayF[] = { { "Delay", K::Float }, { "AutoStart", K::Bool }, { "RunOnce", K::Bool }, { "RemoveDuplicateEvents", K::Bool } };
constexpr LogicParamField kRandomDelayF[] = { { "MinDelay", K::Float }, { "MaxDelay", K::Float }, { "AutoStart", K::Bool }, { "RunOnce", K::Bool } };
constexpr LogicParamField kDelayTriggerF[] = { { "Delay", K::Float }, { "Enabled", K::Bool }, { "RunOnce", K::Bool }, { "AutoTriggerEvent", K::Bool } };
constexpr LogicParamField kMultipleTriggerF[] = { { "TriggerAmount", K::Int }, { "Delay", K::Float }, { "RunOnce", K::Bool }, { "Enabled", K::Bool } };
constexpr LogicParamField kEventGateF[] = { { "Default", K::Bool } };
constexpr LogicParamField kEventSplitterF[] = { { "RunOnce", K::Bool } };
constexpr LogicParamField kEventSwitchF[] = { { "OutEvents", K::Int }, { "AutoIncrement", K::Bool } };
constexpr LogicParamField kRandomMultiEventF[] = { { "RandomEventWeight", K::Float, Shape::Array }, { "UniformDistribution", K::Bool },
	{ "DisableOutputOnTrigger", K::Bool }, { "ResetOutputsWhenAllHasTriggered", K::Bool } };
constexpr LogicParamField kSequenceF[] = { { "SequenceLength", K::Int }, { "SequenceStartTime", K::Int }, { "Looping", K::Bool }, { "PlayInReverse", K::Bool },
	{ "AutoStart", K::Bool }, { "ExternalTime", K::Float }, { "Events", K::Int, Shape::StructArray, { { "EventTime", K::Int }, { "Id", K::Int } } } };
constexpr LogicParamField kTeamFilterF[] = { { "Team", K::Team }, { "InvertFilter", K::Bool }, { "GenerateEventForEveryMatchingTeamMember", K::Bool } };
constexpr LogicParamField kPlayerFilterF[] = { { "InvertFilter", K::Bool } };
constexpr LogicParamField kBoolF[] = { { "DefaultValue", K::Bool } };
constexpr LogicParamField kSyncedBoolF[] = { { "In", K::Bool } };
constexpr LogicParamField kIntF[] = { { "DefaultValue", K::Int }, { "IncDecValue", K::Int } };
constexpr LogicParamField kFloatF[] = { { "DefaultValue", K::Float }, { "IncDecValue", K::Float } };
constexpr LogicParamField kIntHubF[] = { { "InputCount", K::Int }, { "InputSelect", K::Int } };
constexpr LogicParamField kBoolToEventF[] = { { "Value", K::Bool }, { "InitalEvent", K::Bool } };
constexpr LogicParamField kNotF[] = { { "In", K::Bool } };
constexpr LogicParamField kAndOrF[] = { { "In1", K::Bool }, { "In2", K::Bool } };
constexpr LogicParamField kOr4F[] = { { "In1", K::Bool }, { "In2", K::Bool }, { "In3", K::Bool }, { "In4", K::Bool } };
constexpr LogicParamField kCompareF[] = { { "Operator", K::Int }, { "IntIn0", K::Int }, { "IntIn1", K::Int }, { "FloatIn0", K::Float }, { "FloatIn1", K::Float } };
constexpr LogicParamField kCompareIntF[] = { { "A", K::Int }, { "B", K::Int }, { "TriggerOnPropertyChange", K::Bool }, { "TriggerOnStart", K::Bool } };
constexpr LogicParamField kCompareFloatF[] = { { "A", K::Float }, { "B", K::Float }, { "TriggerOnPropertyChange", K::Bool }, { "TriggerOnStart", K::Bool } };
constexpr LogicParamField kCompareBoolF[] = { { "Bool", K::Bool }, { "TriggerOnPropertyChange", K::Bool } };
constexpr LogicParamField kMathOpF[] = { { "Operators", K::Int, Shape::Array } };
constexpr LogicParamField kPropertyCastF[] = { { "IntValue", K::Int }, { "FloatValue", K::Float }, { "BoolValue", K::Bool } };
constexpr LogicParamField kPropertyGateF[] = { { "Default", K::Bool }, { "BoolIn", K::Bool }, { "FloatIn", K::Float }, { "IntIn", K::Int },
	{ "TransformIn", K::Transform }, { "Vec3In", K::Vec3 } };
constexpr LogicParamField kSwitchPropertyStringF[] = { { "StringProperties", K::Text, Shape::Array } };
constexpr LogicParamField kSettingF[] = { { "BoolSettingName", K::Text }, { "IntSettingName", K::Text }, { "FloatSettingName", K::Text } };
constexpr LogicParamField kLocatorF[] = { { "Transform", K::Transform } };
constexpr LogicParamField kConsoleCommandF[] = { { "CommandName", K::Text } };
constexpr LogicParamField kPrintDebugTextF[] = { { "Text", K::Text }, { "Enabled", K::Bool } };
// Natives: the state their stubs start from (logic_native.h).
constexpr LogicParamField kSpawnerF[] = { { "Enabled", K::Bool }, { "Team", K::Team } };
constexpr LogicParamField kCapturePointF[] = { { "Enabled", K::Bool }, { "InitialOwnerTeam", K::Team } };
constexpr LogicParamField kTicketCounterF[] = { { "Enabled", K::Bool }, { "TeamId", K::Team }, { "InitialTicketCount", K::Int } };
constexpr LogicParamField kLifeCounterF[] = { { "Enabled", K::Bool }, { "TeamId", K::Team }, { "StartingLifeCount", K::Int }, { "BaseCount", K::Int },
	{ "FallbackTime", K::Float }, { "Attacker", K::Bool } };
constexpr LogicParamField kTeamNativeF[] = { { "Enabled", K::Bool }, { "Team", K::Team } };
constexpr LogicParamField kEnabledF[] = { { "Enabled", K::Bool } };

template <size_t N>
std::span<const LogicParamField> fields(const LogicParamField (&p_table)[N]) {
	return std::span<const LogicParamField>(p_table, N);
}

} // namespace

std::span<const LogicParamField> logic_param_fields(LogicTypeId p_type) {
	using T = LogicTypeId;
	switch (p_type) {
		case T::Delay:
			return fields(kDelayF);
		case T::RandomDelay:
			return fields(kRandomDelayF);
		case T::DelayTrigger:
			return fields(kDelayTriggerF);
		case T::MultipleTrigger:
			return fields(kMultipleTriggerF);
		case T::EventGate:
			return fields(kEventGateF);
		case T::EventSplitter:
			return fields(kEventSplitterF);
		case T::EventSwitch:
			return fields(kEventSwitchF);
		case T::RandomMultiEvent:
			return fields(kRandomMultiEventF);
		case T::Sequence:
		case T::SyncedSequence:
			return fields(kSequenceF);
		case T::TeamFilter:
			return fields(kTeamFilterF);
		case T::PlayerFilter:
			return fields(kPlayerFilterF);
		case T::Bool:
			return fields(kBoolF);
		case T::SyncedBool:
			return fields(kSyncedBoolF);
		case T::Int:
			return fields(kIntF);
		case T::Float:
			return fields(kFloatF);
		case T::IntHub:
			return fields(kIntHubF);
		case T::BoolToEvent:
			return fields(kBoolToEventF);
		case T::Not:
			return fields(kNotF);
		case T::And:
		case T::Or:
			return fields(kAndOrF);
		case T::Or4:
			return fields(kOr4F);
		case T::Compare:
			return fields(kCompareF);
		case T::CompareInt:
			return fields(kCompareIntF);
		case T::CompareFloat:
			return fields(kCompareFloatF);
		case T::CompareBool:
			return fields(kCompareBoolF);
		case T::MathOp:
			return fields(kMathOpF);
		case T::PropertyCast:
			return fields(kPropertyCastF);
		case T::PropertyGate:
			return fields(kPropertyGateF);
		case T::SwitchPropertyString:
			return fields(kSwitchPropertyStringF);
		case T::Setting:
			return fields(kSettingF);
		case T::Locator:
			return fields(kLocatorF);
		case T::ConsoleCommandTrigger:
			return fields(kConsoleCommandF);
		case T::PrintDebugText:
			return fields(kPrintDebugTextF);
		case T::CharacterSpawn:
		case T::VehicleSpawn:
		case T::AlternateSpawn:
			return fields(kSpawnerF);
		case T::CapturePoint:
			return fields(kCapturePointF);
		case T::TicketCounter:
			return fields(kTicketCounterF);
		case T::LifeCounter:
			return fields(kLifeCounterF);
		case T::CombatAreaTrigger:
		case T::RoundOver:
			return fields(kTeamNativeF);
		default:
			break;
	}
	if (logic_type_info(p_type).cls == LogicTypeClass::Native) {
		return fields(kEnabledF);
	}
	return {};
}

// --- LogicValue ----------------------------------------------------------------------------------------------------

bool LogicValue::same_as(const LogicValue &p_other) const {
	if (kind != p_other.kind) {
		return false;
	}
	switch (kind) {
		case Kind::None:
			return true;
		case Kind::Bool:
			return b == p_other.b;
		case Kind::Int:
		case Kind::Team:
		case Kind::Text:
			return i == p_other.i;
		case Kind::Float:
			return std::memcmp(&f, &p_other.f, sizeof(f)) == 0;
		case Kind::Vec3:
			return std::memcmp(m.data(), p_other.m.data(), 3 * sizeof(float)) == 0;
		case Kind::Transform:
			return std::memcmp(m.data(), p_other.m.data(), 12 * sizeof(float)) == 0;
	}
	return false;
}

uint64_t logic_value_hash(const LogicValue &p_value) {
	Fnv h;
	h.byte(static_cast<uint8_t>(p_value.kind));
	switch (p_value.kind) {
		case K::None:
			break;
		case K::Bool:
			h.byte(p_value.b ? 1 : 0);
			break;
		case K::Int:
		case K::Team:
		case K::Text:
			h.u64(static_cast<uint64_t>(p_value.i));
			break;
		case K::Float:
			h.f64(p_value.f);
			break;
		case K::Vec3:
			for (size_t k = 0; k < 3; ++k) {
				h.f32(p_value.m[k]);
			}
			break;
		case K::Transform:
			for (size_t k = 0; k < 12; ++k) {
				h.f32(p_value.m[k]);
			}
			break;
	}
	return h.h;
}

namespace {

LogicValue make_bool(bool p_b) {
	LogicValue v;
	v.kind = K::Bool;
	v.b = p_b;
	return v;
}

LogicValue make_int(K p_kind, int64_t p_i) {
	LogicValue v;
	v.kind = p_kind;
	v.i = p_i;
	return v;
}

LogicValue make_float(double p_f) {
	LogicValue v;
	v.kind = K::Float;
	v.f = p_f;
	return v;
}

LogicValue make_rows(K p_kind, const std::array<std::array<float, 3>, 4> &p_rows) {
	LogicValue v;
	v.kind = p_kind;
	for (size_t r = 0; r < 4; ++r) {
		for (size_t c = 0; c < 3; ++c) {
			v.m[r * 3 + c] = p_rows[r][c];
		}
	}
	return v;
}

LogicValue make_vec3(const std::array<float, 3> &p_v) {
	LogicValue v;
	v.kind = K::Vec3;
	v.m[0] = p_v[0];
	v.m[1] = p_v[1];
	v.m[2] = p_v[2];
	return v;
}

// An element or member value (no property connection reaches array contents).
std::optional<LogicValue> value_of_field(const EbxValue &p_value, K p_kind, std::string *r_text) {
	return logic_value_of(PropertyValue::of_field(p_value), p_kind, r_text);
}

} // namespace

std::optional<LogicValue> logic_value_of(const PropertyValue &p_value, LogicValue::Kind p_kind, std::string *r_text) {
	switch (p_kind) {
		case K::None:
			return std::nullopt;
		case K::Bool:
			if (const std::optional<bool> b = p_value.as_bool()) {
				return make_bool(*b);
			}
			return std::nullopt;
		case K::Int:
			if (const std::optional<int64_t> i = p_value.as_i64()) {
				return make_int(K::Int, *i);
			}
			return std::nullopt;
		case K::Float:
			if (p_value.kind == PropertyValue::Kind::Field) {
				if (const std::optional<double> d = detail::number(p_value.field)) {
					return make_float(*d);
				}
				return std::nullopt;
			}
			if (const std::optional<double> d = p_value.as_f64()) {
				return make_float(*d);
			}
			return std::nullopt;
		case K::Team:
			if (const std::optional<int> t = p_value.as_team()) {
				return make_int(K::Team, *t);
			}
			return std::nullopt;
		case K::Text: {
			std::optional<std::string_view> s = p_value.as_str();
			if (!s && p_value.kind == PropertyValue::Kind::Field) {
				s = p_value.field.enum_name();
			}
			if (!s) {
				return std::nullopt;
			}
			if (r_text != nullptr) {
				*r_text = std::string(*s);
			}
			return make_int(K::Text, 0);
		}
		case K::Vec3:
			if (const std::optional<std::array<float, 3>> v = p_value.as_vec3()) {
				return make_vec3(*v);
			}
			return std::nullopt;
		case K::Transform:
			if (p_value.kind == PropertyValue::Kind::Transform) {
				return make_rows(K::Transform, p_value.rows);
			}
			if (p_value.kind == PropertyValue::Kind::Field) {
				if (const std::optional<std::array<std::array<float, 3>, 4>> t = p_value.field.as_transform()) {
					return make_rows(K::Transform, *t);
				}
			}
			return std::nullopt;
	}
	return std::nullopt;
}

// --- parts and finalize --------------------------------------------------------------------------------------------

namespace {

struct GraphParts {
	LogicSide side = LogicSide::Server;
	std::vector<LogicScopeInfo> scopes;
	std::vector<LogicNode> nodes;
	std::vector<std::pair<uint32_t, LogicParam>> params;
	std::vector<std::pair<uint32_t, LogicInput>> inputs;
	std::vector<LogicEdge> events, properties, links, networked;
	std::vector<std::string> texts;
	std::map<std::string, uint32_t, std::less<>> text_index;
	LogicGraphStats stats;
	std::string bad; // first invalid Builder call, if any

	uint32_t intern(std::string_view p_text) {
		const auto it = text_index.find(p_text);
		if (it != text_index.end()) {
			return it->second;
		}
		const uint32_t at = static_cast<uint32_t>(texts.size());
		texts.emplace_back(p_text);
		text_index.emplace(std::string(p_text), at);
		return at;
	}

	// The cost of interning p_text: a lookup, plus its copy when new.
	size_t intern_cost(std::string_view p_text) const { return 1 + p_text.size() / WorkBudget::kBytesPerUnit; }

	uint32_t add_scope(uint32_t p_parent, const Guid &p_reference, std::string_view p_name) {
		LogicScopeInfo s;
		s.parent = p_parent;
		s.reference = p_reference;
		if (p_parent != kNoLogicIndex && p_parent < scopes.size()) {
			s.depth = scopes[p_parent].depth + 1;
			s.path = chain(scopes[p_parent].path, guid_word(p_reference));
		} else {
			if (p_parent != kNoLogicIndex && bad.empty()) {
				bad = "scope parent " + std::to_string(p_parent) + " out of range";
			}
			s.path = chain(kRootPath, guid_word(p_reference));
		}
		s.name = intern(p_name);
		scopes.push_back(s);
		return static_cast<uint32_t>(scopes.size() - 1);
	}

	uint32_t add_node(LogicTypeId p_type, LogicRealm p_realm, uint32_t p_scope, const Guid &p_guid, std::string_view p_type_name) {
		LogicNode n;
		n.type = p_type;
		n.realm = p_realm;
		n.scope = p_scope;
		n.guid = p_guid;
		if (p_scope < scopes.size()) {
			n.key = chain(scopes[p_scope].path, guid_word(p_guid));
		} else if (bad.empty()) {
			bad = "node scope " + std::to_string(p_scope) + " out of range";
		}
		n.type_name = intern(p_type_name);
		nodes.push_back(n);
		return static_cast<uint32_t>(nodes.size() - 1);
	}

	void add_edge(std::vector<LogicEdge> &r_list, uint32_t p_source, int32_t p_source_port, uint32_t p_target, int32_t p_target_port,
			LogicEventTarget p_target_type, bool p_networked, uint64_t p_connection) {
		LogicEdge e;
		e.source = p_source;
		e.source_port = p_source_port;
		e.target = p_target;
		e.target_port = p_target_port;
		e.order = static_cast<uint32_t>(r_list.size());
		e.target_type = p_target_type;
		e.networked = p_networked;
		e.connection = p_connection;
		r_list.push_back(e);
	}
};

} // namespace

class LogicGraphBuild {
public:
	static Result<LogicGraph> finalize(GraphParts &&r_parts, const LogicGraphOptions &p_caps);
	static Result<LogicGraph> from_world(const LogicWorld &p_world, const LogicGraphOptions &p_options);
};

Result<LogicGraph> LogicGraphBuild::finalize(GraphParts &&r_parts, const LogicGraphOptions &p_caps) {
	if (!r_parts.bad.empty()) {
		return make_error("logic graph: " + r_parts.bad);
	}
	const size_t n = r_parts.nodes.size();
	const size_t edges = r_parts.events.size() + r_parts.properties.size() + r_parts.links.size() + r_parts.networked.size();
	if (n > p_caps.max_nodes) {
		return make_error("logic graph: " + std::to_string(n) + " nodes exceed the cap of " + std::to_string(p_caps.max_nodes));
	}
	if (edges > p_caps.max_edges) {
		return make_error("logic graph: " + std::to_string(edges) + " edges exceed the cap of " + std::to_string(p_caps.max_edges));
	}
	if (r_parts.params.size() + r_parts.inputs.size() > p_caps.max_params) {
		return make_error("logic graph: " + std::to_string(r_parts.params.size() + r_parts.inputs.size()) + " params exceed the cap of " +
				std::to_string(p_caps.max_params));
	}
	for (size_t i = 0; i < r_parts.scopes.size(); ++i) {
		const uint32_t parent = r_parts.scopes[i].parent;
		if (parent != kNoLogicIndex && parent >= i) {
			return make_error("logic graph: scope " + std::to_string(i) + " has parent " + std::to_string(parent) + " (parents come first)");
		}
	}
	for (size_t i = 0; i < n; ++i) {
		if (r_parts.nodes[i].scope >= r_parts.scopes.size()) {
			return make_error("logic graph: node " + std::to_string(i) + " scope out of range");
		}
		if (static_cast<size_t>(r_parts.nodes[i].type) >= static_cast<size_t>(LogicTypeId::Count)) {
			return make_error("logic graph: node " + std::to_string(i) + " type out of range");
		}
	}
	for (const auto &[node, param] : r_parts.params) {
		if (node >= n) {
			return make_error("logic graph: param node " + std::to_string(node) + " out of range");
		}
	}
	for (const auto &[node, input] : r_parts.inputs) {
		if (node >= n) {
			return make_error("logic graph: input node " + std::to_string(node) + " out of range");
		}
	}
	const auto check_edges = [&](const std::vector<LogicEdge> &p_list, const char *p_kind, bool p_target_optional) -> Status {
		for (const LogicEdge &e : p_list) {
			if (e.source >= n || (e.target >= n && !(p_target_optional && e.target == kNoLogicIndex))) {
				return make_error(std::string("logic graph: ") + p_kind + " edge end out of range");
			}
		}
		return Status::success();
	};
	BF3_TRY(check_edges(r_parts.events, "event", false));
	BF3_TRY(check_edges(r_parts.properties, "property", false));
	BF3_TRY(check_edges(r_parts.links, "link", false));
	BF3_TRY(check_edges(r_parts.networked, "networked", true));

	LogicGraph g;
	g.side_ = r_parts.side;
	g.scopes_ = std::move(r_parts.scopes);
	g.nodes_ = std::move(r_parts.nodes);
	g.texts_ = std::move(r_parts.texts);
	for (LogicNode &node : g.nodes_) {
		node.flags = 0;
		node.param_begin = node.param_count = 0;
		node.input_begin = node.input_count = 0;
		node.event_out_begin = node.event_out_count = 0;
		node.property_out_begin = node.property_out_count = 0;
		node.link_out_begin = node.link_out_count = 0;
	}
	// Params and inputs grouped by node, each node's in the order they were added.
	std::stable_sort(r_parts.params.begin(), r_parts.params.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
	std::stable_sort(r_parts.inputs.begin(), r_parts.inputs.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
	g.params_.reserve(r_parts.params.size());
	for (size_t i = 0; i < r_parts.params.size(); ++i) {
		LogicNode &node = g.nodes_[r_parts.params[i].first];
		if (node.param_count == 0) {
			node.param_begin = static_cast<uint32_t>(i);
		}
		++node.param_count;
		g.params_.push_back(r_parts.params[i].second);
	}
	g.inputs_.reserve(r_parts.inputs.size());
	for (size_t i = 0; i < r_parts.inputs.size(); ++i) {
		LogicNode &node = g.nodes_[r_parts.inputs[i].first];
		if (node.input_count == 0) {
			node.input_begin = static_cast<uint32_t>(i);
		}
		++node.input_count;
		g.inputs_.push_back(r_parts.inputs[i].second);
	}
	const auto edge_less = [](const LogicEdge &a, const LogicEdge &b) {
		return std::tie(a.source, a.source_port, a.order) < std::tie(b.source, b.source_port, b.order);
	};
	const auto sort_edges = [&](std::vector<LogicEdge> &r_list) { std::sort(r_list.begin(), r_list.end(), edge_less); };
	sort_edges(r_parts.events);
	sort_edges(r_parts.properties);
	sort_edges(r_parts.links);
	sort_edges(r_parts.networked);
	const auto csr = [&](const std::vector<LogicEdge> &p_list, uint32_t LogicNode::*p_begin, uint32_t LogicNode::*p_count) {
		for (size_t i = 0; i < p_list.size(); ++i) {
			LogicNode &node = g.nodes_[p_list[i].source];
			if (node.*p_count == 0) {
				node.*p_begin = static_cast<uint32_t>(i);
			}
			++(node.*p_count);
		}
	};
	csr(r_parts.events, &LogicNode::event_out_begin, &LogicNode::event_out_count);
	csr(r_parts.properties, &LogicNode::property_out_begin, &LogicNode::property_out_count);
	csr(r_parts.links, &LogicNode::link_out_begin, &LogicNode::link_out_count);
	for (const std::vector<LogicEdge> *list : { &r_parts.events, &r_parts.properties, &r_parts.links, &r_parts.networked }) {
		for (const LogicEdge &e : *list) {
			g.nodes_[e.source].flags |= kLogicNodeConnected;
			if (e.target != kNoLogicIndex) {
				g.nodes_[e.target].flags |= kLogicNodeConnected;
			}
		}
	}
	// Duplicate paths: equal (source, port, target, port) within a kind, counted after the first.
	size_t duplicates = 0;
	for (const std::vector<LogicEdge> *list : { &r_parts.events, &r_parts.properties, &r_parts.links }) {
		std::vector<std::tuple<uint32_t, int32_t, uint32_t, int32_t>> keys;
		keys.reserve(list->size());
		for (const LogicEdge &e : *list) {
			keys.emplace_back(e.source, e.source_port, e.target, e.target_port);
		}
		std::sort(keys.begin(), keys.end());
		for (size_t i = 1; i < keys.size(); ++i) {
			duplicates += keys[i] == keys[i - 1];
		}
	}
	g.event_edges_ = std::move(r_parts.events);
	g.property_edges_ = std::move(r_parts.properties);
	g.link_edges_ = std::move(r_parts.links);
	g.networked_edges_ = std::move(r_parts.networked);

	g.by_guid_.resize(n);
	g.by_type_.resize(n);
	for (uint32_t i = 0; i < n; ++i) {
		g.by_guid_[i] = i;
		g.by_type_[i] = i;
	}
	std::sort(g.by_guid_.begin(), g.by_guid_.end(), [&](uint32_t a, uint32_t b) { return std::tie(g.nodes_[a].guid, a) < std::tie(g.nodes_[b].guid, b); });
	std::sort(g.by_type_.begin(), g.by_type_.end(), [&](uint32_t a, uint32_t b) { return std::tie(g.nodes_[a].type, a) < std::tie(g.nodes_[b].type, b); });

	LogicGraphStats &st = r_parts.stats;
	std::fill(std::begin(st.nodes_by_class), std::end(st.nodes_by_class), size_t(0));
	st.unknown_nodes = st.unimplemented_nodes = st.unknown_unconnected = st.unimplemented_unconnected = 0;
	st.synced_bools = 0;
	std::map<std::string, size_t, std::less<>> unknown, unimplemented;
	for (const LogicNode &node : g.nodes_) {
		const LogicTypeInfo &info = logic_type_info(node.type);
		++st.nodes_by_class[static_cast<size_t>(info.cls)];
		const bool connected = (node.flags & kLogicNodeConnected) != 0;
		const bool is_unknown = info.cls == LogicTypeClass::Unknown;
		const bool is_unimplemented = (info.cls == LogicTypeClass::Primitive || info.cls == LogicTypeClass::Native) && !info.s4;
		const std::string_view name = node.type_name < g.texts_.size() ? std::string_view(g.texts_[node.type_name]) : std::string_view();
		if (is_unknown) {
			if (connected) {
				++st.unknown_nodes;
				++unknown[std::string(name)];
			} else {
				++st.unknown_unconnected;
			}
		}
		if (is_unimplemented) {
			if (connected) {
				++st.unimplemented_nodes;
				++unimplemented[std::string(name)];
			} else {
				++st.unimplemented_unconnected;
			}
		}
		st.synced_bools += node.type == LogicTypeId::SyncedBool;
	}
	st.unknown_types.clear();
	st.unimplemented_types.clear();
	for (const auto &[name, count] : unknown) {
		if (st.unknown_types.size() < LogicGraphStats::kMaxListedTypes) {
			st.unknown_types.emplace_back(name, count);
		}
	}
	for (const auto &[name, count] : unimplemented) {
		if (st.unimplemented_types.size() < LogicGraphStats::kMaxListedTypes) {
			st.unimplemented_types.emplace_back(name, count);
		}
	}
	st.event_edges = g.event_edges_.size();
	st.property_edges = g.property_edges_.size();
	st.link_edges = g.link_edges_.size();
	st.networked_only_edges = g.networked_edges_.size();
	st.duplicate_paths = duplicates;
	g.stats_ = std::move(st);
	return g;
}

// --- Builder -------------------------------------------------------------------------------------------------------

struct LogicGraph::Builder::Parts : GraphParts {};

LogicGraph::Builder::Builder(LogicSide p_side) :
		parts_(std::make_shared<Parts>()) {
	parts_->side = p_side;
}

uint32_t LogicGraph::Builder::add_scope(uint32_t p_parent, const Guid &p_reference, std::string_view p_name) {
	return parts_->add_scope(p_parent, p_reference, p_name);
}

uint32_t LogicGraph::Builder::add_node(LogicTypeId p_type, LogicRealm p_realm, uint32_t p_scope, const Guid &p_guid) {
	if (static_cast<size_t>(p_type) >= static_cast<size_t>(LogicTypeId::Count)) {
		if (parts_->bad.empty()) {
			parts_->bad = "node type out of range";
		}
		p_type = LogicTypeId::Unknown;
	}
	return parts_->add_node(p_type, p_realm, p_scope, p_guid, logic_type_info(p_type).ebx_type);
}

void LogicGraph::Builder::add_param(uint32_t p_node, int32_t p_field, const LogicValue &p_value, uint16_t p_element) {
	LogicParam p;
	p.field = p_field;
	p.element = p_element;
	p.value = p_value;
	parts_->params.emplace_back(p_node, p);
}

void LogicGraph::Builder::add_input(uint32_t p_node, int32_t p_port, const LogicValue &p_value) {
	LogicInput in;
	in.port = p_port;
	in.value = p_value;
	parts_->inputs.emplace_back(p_node, in);
}

void LogicGraph::Builder::add_event(uint32_t p_source, int32_t p_source_port, uint32_t p_target, int32_t p_target_port, LogicEventTarget p_target_type,
		uint64_t p_connection) {
	parts_->add_edge(parts_->events, p_source, p_source_port, p_target, p_target_port, p_target_type, networked_to_clients(p_target_type), p_connection);
}

void LogicGraph::Builder::add_networked_only(uint32_t p_source, int32_t p_source_port, uint32_t p_target, int32_t p_target_port,
		LogicEventTarget p_target_type, uint64_t p_connection) {
	parts_->add_edge(parts_->networked, p_source, p_source_port, p_target, p_target_port, p_target_type, true, p_connection);
}

void LogicGraph::Builder::add_property(uint32_t p_source, int32_t p_source_port, uint32_t p_target, int32_t p_target_port) {
	parts_->add_edge(parts_->properties, p_source, p_source_port, p_target, p_target_port, LogicEventTarget::Server, false, 0);
}

void LogicGraph::Builder::add_link(uint32_t p_source, int32_t p_source_port, uint32_t p_target, int32_t p_target_port) {
	parts_->add_edge(parts_->links, p_source, p_source_port, p_target, p_target_port, LogicEventTarget::Server, false, 0);
}

Result<LogicGraph> LogicGraph::Builder::finish(const LogicGraphOptions &p_caps) {
	if (!parts_) {
		return make_error("logic graph: builder already finished");
	}
	std::shared_ptr<Parts> parts = std::move(parts_);
	parts->stats.scopes = parts->scopes.size();
	parts->stats.objects = parts->nodes.size();
	return LogicGraphBuild::finalize(std::move(*parts), p_caps);
}

// --- accessors -----------------------------------------------------------------------------------------------------

std::span<const LogicParam> LogicGraph::params_of(uint32_t p_node) const {
	if (p_node >= nodes_.size()) {
		return {};
	}
	return std::span<const LogicParam>(params_).subspan(nodes_[p_node].param_begin, nodes_[p_node].param_count);
}

std::optional<LogicValue> LogicGraph::param(uint32_t p_node, int32_t p_field, uint16_t p_element) const {
	for (const LogicParam &p : params_of(p_node)) {
		if (p.field == p_field && p.element == p_element) {
			return p.value;
		}
	}
	return std::nullopt;
}

std::span<const LogicEdge> LogicGraph::events_from(uint32_t p_node) const {
	if (p_node >= nodes_.size()) {
		return {};
	}
	return std::span<const LogicEdge>(event_edges_).subspan(nodes_[p_node].event_out_begin, nodes_[p_node].event_out_count);
}

std::span<const LogicEdge> LogicGraph::properties_from(uint32_t p_node) const {
	if (p_node >= nodes_.size()) {
		return {};
	}
	return std::span<const LogicEdge>(property_edges_).subspan(nodes_[p_node].property_out_begin, nodes_[p_node].property_out_count);
}

std::span<const LogicEdge> LogicGraph::links_from(uint32_t p_node) const {
	if (p_node >= nodes_.size()) {
		return {};
	}
	return std::span<const LogicEdge>(link_edges_).subspan(nodes_[p_node].link_out_begin, nodes_[p_node].link_out_count);
}

std::span<const uint32_t> LogicGraph::nodes_with_guid(const Guid &p_guid) const {
	const auto lo = std::lower_bound(by_guid_.begin(), by_guid_.end(), p_guid, [&](uint32_t a, const Guid &g) { return nodes_[a].guid < g; });
	const auto hi = std::upper_bound(lo, by_guid_.end(), p_guid, [&](const Guid &g, uint32_t a) { return g < nodes_[a].guid; });
	return std::span<const uint32_t>(lo, hi);
}

std::span<const uint32_t> LogicGraph::nodes_of_type(LogicTypeId p_type) const {
	const auto lo = std::lower_bound(by_type_.begin(), by_type_.end(), p_type, [&](uint32_t a, LogicTypeId t) { return nodes_[a].type < t; });
	const auto hi = std::upper_bound(lo, by_type_.end(), p_type, [&](LogicTypeId t, uint32_t a) { return t < nodes_[a].type; });
	return std::span<const uint32_t>(lo, hi);
}

std::vector<uint32_t> LogicGraph::find_nodes(const LogicNodeRef &p_ref, LogicTypeId p_type) const {
	std::vector<uint32_t> out;
	const auto matches = [&](uint32_t p_node) {
		const LogicNode &node = nodes_[p_node];
		if (!p_ref.entity.is_zero() && node.guid != p_ref.entity) {
			return false;
		}
		if (p_ref.scope_reference.is_zero()) {
			return true;
		}
		uint32_t s = node.scope;
		for (size_t depth = 0; s != kNoLogicIndex && s < scopes_.size() && depth <= scopes_.size(); ++depth) {
			if (scopes_[s].reference == p_ref.scope_reference) {
				return true;
			}
			s = scopes_[s].parent;
		}
		return false;
	};
	if (p_type == LogicTypeId::Unknown) {
		if (!p_ref.entity.is_zero()) {
			for (const uint32_t i : nodes_with_guid(p_ref.entity)) {
				if (matches(i)) {
					out.push_back(i);
				}
			}
			std::sort(out.begin(), out.end());
			return out;
		}
		for (uint32_t i = 0; i < nodes_.size(); ++i) {
			if (matches(i)) {
				out.push_back(i);
			}
		}
		return out;
	}
	for (const uint32_t i : nodes_of_type(p_type)) {
		if (matches(i)) {
			out.push_back(i);
		}
	}
	return out;
}

bool LogicGraph::scope_within(uint32_t p_scope, uint32_t p_ancestor) const {
	uint32_t s = p_scope;
	for (size_t depth = 0; s != kNoLogicIndex && s < scopes_.size() && depth <= scopes_.size(); ++depth) {
		if (s == p_ancestor) {
			return true;
		}
		s = scopes_[s].parent;
	}
	return false;
}

std::string_view LogicGraph::text(uint32_t p_index) const {
	return p_index < texts_.size() ? std::string_view(texts_[p_index]) : std::string_view();
}

uint64_t LogicGraph::digest() const {
	Fnv h;
	h.u32(kLogicGraphVersion);
	h.u32(kLogicTypesVersion);
	h.byte(static_cast<uint8_t>(side_));
	h.u64(scopes_.size());
	for (const LogicScopeInfo &s : scopes_) {
		h.u32(s.parent);
		h.u64(s.path);
		h.str(text(s.name));
	}
	h.u64(nodes_.size());
	for (const LogicNode &node : nodes_) {
		h.str(text(node.type_name));
		h.str(logic_type_info(node.type).ebx_type);
		h.byte(static_cast<uint8_t>(node.realm));
		h.u32(node.scope);
		h.u64(node.key);
		h.u32(node.param_count);
		h.u32(node.input_count);
	}
	for (const LogicParam &p : params_) {
		h.i32(p.field);
		h.u32(p.element);
		if (p.value.kind == K::Text) {
			h.str(text(static_cast<uint32_t>(p.value.i)));
		} else {
			h.u64(logic_value_hash(p.value));
		}
	}
	for (const LogicInput &in : inputs_) {
		h.i32(in.port);
		if (in.value.kind == K::Text) {
			h.str(text(static_cast<uint32_t>(in.value.i)));
		} else {
			h.u64(logic_value_hash(in.value));
		}
	}
	for (const std::vector<LogicEdge> *list : { &event_edges_, &property_edges_, &link_edges_, &networked_edges_ }) {
		h.u64(list->size());
		for (const LogicEdge &e : *list) {
			h.u32(e.source);
			h.i32(e.source_port);
			h.u32(e.target);
			h.i32(e.target_port);
			h.byte(static_cast<uint8_t>(e.target_type));
			h.byte(e.networked ? 1 : 0);
			h.u64(e.connection);
		}
	}
	return h.h;
}

// --- build from a LogicWorld ---------------------------------------------------------------------------------------

namespace {

// (scope, instance) -> value, sorted for binary search (data-controlled keys never hashed).
struct ScopedKey {
	uint32_t scope = 0;
	uintptr_t partition = 0;
	uint32_t index = 0;
	uint32_t value = 0;
	auto tie() const { return std::tie(scope, partition, index); }
};

class ScopedIndex {
public:
	void add(uint32_t p_scope, const EbxInstance &p_instance, uint32_t p_value) {
		entries_.push_back({ p_scope, reinterpret_cast<uintptr_t>(&p_instance.partition()), p_instance.index(), p_value });
	}
	void seal() {
		std::stable_sort(entries_.begin(), entries_.end(), [](const ScopedKey &a, const ScopedKey &b) { return a.tie() < b.tie(); });
	}
	std::optional<uint32_t> find(uint32_t p_scope, const EbxInstance &p_instance) const {
		const ScopedKey key{ p_scope, reinterpret_cast<uintptr_t>(&p_instance.partition()), p_instance.index(), 0 };
		const auto it = std::lower_bound(entries_.begin(), entries_.end(), key, [](const ScopedKey &a, const ScopedKey &b) { return a.tie() < b.tie(); });
		if (it == entries_.end() || it->tie() != key.tie()) {
			return std::nullopt;
		}
		return it->value;
	}

private:
	std::vector<ScopedKey> entries_;
};

constexpr uint32_t kAbsent = 0x80000000u; // object value flag: the object does not exist on this side

enum class EdgeKind : uint8_t { Event = 0, Property = 1, Link = 2 };
enum class EndKind : uint8_t { Real, Absent, Virtual };

struct End {
	EndKind kind = EndKind::Real;
	uint32_t index = 0; // node index (Real), virtual id (Virtual)
};

struct Hop {
	End source;
	int32_t source_port = 0;
	End target;
	int32_t target_port = 0;
	LogicEventTarget target_type = LogicEventTarget::Server;
	uint64_t connection = 0;
};

struct Virtual {
	uint32_t scope = 0;
	int32_t port = 0;
	bool in = false; // IfaceIn (else IfaceOut)
	uint32_t incoming = 0;
	uint32_t out_begin = 0, out_count = 0; // into the kind's hops sorted by source virtual
};

struct KindGraph {
	std::vector<Hop> hops; // record order
	std::vector<Virtual> virtuals;
	std::map<std::tuple<uint32_t, int32_t, bool>, uint32_t> virtual_index; // (scope, port, in)
	std::vector<uint32_t> out_hops; // hop indices grouped by source virtual, record order within one
	std::vector<uint8_t> on_path;

	uint32_t virtual_of(uint32_t p_scope, int32_t p_port, bool p_in) {
		const auto key = std::make_tuple(p_scope, p_port, p_in);
		const auto it = virtual_index.find(key);
		if (it != virtual_index.end()) {
			return it->second;
		}
		const uint32_t at = static_cast<uint32_t>(virtuals.size());
		Virtual v;
		v.scope = p_scope;
		v.port = p_port;
		v.in = p_in;
		virtuals.push_back(v);
		virtual_index.emplace(key, at);
		return at;
	}

	void seal() {
		std::vector<uint32_t> counts(virtuals.size(), 0);
		for (const Hop &h : hops) {
			if (h.source.kind == EndKind::Virtual) {
				++counts[h.source.index];
			}
			if (h.target.kind == EndKind::Virtual) {
				++virtuals[h.target.index].incoming;
			}
		}
		uint32_t at = 0;
		for (size_t v = 0; v < virtuals.size(); ++v) {
			virtuals[v].out_begin = at;
			at += counts[v];
			counts[v] = virtuals[v].out_begin;
		}
		out_hops.assign(at, 0);
		for (uint32_t i = 0; i < hops.size(); ++i) {
			if (hops[i].source.kind == EndKind::Virtual) {
				const uint32_t v = hops[i].source.index;
				out_hops[counts[v]++] = i;
				++virtuals[v].out_count;
			}
		}
		on_path.assign(virtuals.size(), 0);
	}
};

class GraphBuilder {
public:
	GraphBuilder(const LogicWorld &p_world, const LogicGraphOptions &p_options) :
			world_(p_world), db_(p_world.db()), options_(p_options), budget_(p_world.budget()) {
		parts_.side = p_options.side;
	}

	Result<LogicGraph> run() {
		BF3_TRY(budget_.status());
		BF3_TRY(scopes());
		BF3_TRY(nodes());
		BF3_TRY(connections());
		for (int k = 0; k < 3; ++k) {
			BF3_TRY(collapse(static_cast<EdgeKind>(k)));
		}
		BF3_TRY(constants());
		BF3_TRY(budget_.status());
		LogicGraphStats &st = parts_.stats;
		st.scopes = world_.scopes().size();
		st.objects = world_.objects().size();
		st.work = budget_.used();
		return LogicGraphBuild::finalize(std::move(parts_), options_);
	}

private:
	bool on_side(LogicRealm p_realm) const { return options_.side == LogicSide::Server ? realm_on_server(p_realm) : realm_on_client(p_realm); }

	Status emit_charge(size_t p_units = 1) { return budget_.charge(p_units); }

	uint32_t intern(std::string_view p_text) {
		const bool known = parts_.text_index.find(p_text) != parts_.text_index.end();
		if (!known && !budget_.take(parts_.intern_cost(p_text))) {
			return kNoLogicIndex;
		}
		return parts_.intern(p_text);
	}

	Status scopes() {
		const std::vector<LogicScope> &list = world_.scopes();
		descriptors_.resize(list.size());
		for (uint32_t s = 0; s < list.size(); ++s) {
			BF3_TRY(budget_.charge(2)); // the scope and its descriptor lookup
			const LogicScope &scope = list[s];
			const std::string_view name = world_.scope_name(s);
			BF3_TRY(budget_.charge(parts_.intern_cost(name)));
			const uint32_t parent = scope.parent ? *scope.parent : kNoLogicIndex;
			parts_.add_scope(parent, scope.reference ? scope.reference->guid() : Guid{}, name);
			descriptors_[s] = db_.follow(scope.blueprint.data(), "Descriptor");
			if (scope.parent && scope.reference) {
				children_.add(*scope.parent, *scope.reference, s);
			}
		}
		children_.seal();
		return Status::success();
	}

	Status nodes() {
		const std::vector<LogicObject> &list = world_.objects();
		if (list.size() > options_.max_nodes) {
			return make_error("logic graph: " + std::to_string(list.size()) + " objects exceed the node cap of " + std::to_string(options_.max_nodes));
		}
		for (uint32_t i = 0; i < list.size(); ++i) {
			const LogicObject &object = list[i];
			BF3_TRY(budget_.charge_type(object.entity.data()));
			const std::string_view type_name = object.entity.type_name();
			LogicTypeId type = logic_type_of(type_name);
			if (type == LogicTypeId::Unknown && object.entity.is_a("ReferenceObjectData")) {
				type = object.entity.is_a("SubWorldReferenceObjectData")	   ? LogicTypeId::SubWorldReference
						: object.entity.is_a("WorldPartReferenceObjectData") ? LogicTypeId::WorldPartReference
																			 : LogicTypeId::ReferenceObject;
			}
			LogicRealm realm = LogicRealm::Unset;
			if (const std::optional<EbxValue> v = object.entity.get("Realm")) {
				const std::optional<std::string_view> name = v->enum_name();
				const std::optional<LogicRealm> parsed = name ? logic_realm_from_name(*name) : std::nullopt;
				if (parsed) {
					realm = *parsed;
				} else if (const std::optional<int32_t> raw = v->enum_value(); raw && *raw >= 0 && *raw <= 4) {
					realm = static_cast<LogicRealm>(*raw);
				} else {
					realm = LogicRealm::None;
				}
			}
			if (!on_side(realm)) {
				objects_.add(object.scope, object.entity, kAbsent | i);
				if (object.reference) {
					objects_.add(object.scope, *object.reference, kAbsent | i);
				}
				continue;
			}
			BF3_TRY(emit_charge());
			BF3_TRY(budget_.charge(parts_.intern_cost(type_name)));
			const uint32_t node = parts_.add_node(type, realm, object.scope, object.entity.guid(), type_name);
			objects_.add(object.scope, object.entity, node);
			if (object.reference) {
				objects_.add(object.scope, *object.reference, node);
			}
			BF3_TRY(params(object, type, node));
		}
		objects_.seal();
		return Status::success();
	}

	Status add_param(uint32_t p_node, int32_t p_field, uint16_t p_element, const std::optional<LogicValue> &p_value, const std::string &p_text) {
		if (!p_value) {
			return Status::success();
		}
		BF3_TRY(emit_charge());
		LogicParam p;
		p.field = p_field;
		p.element = p_element;
		p.value = *p_value;
		if (p.value.kind == K::Text) {
			const uint32_t at = intern(p_text);
			BF3_TRY(budget_.status());
			p.value.i = at;
		}
		parts_.params.emplace_back(p_node, p);
		return Status::success();
	}

	Status params(const LogicObject &p_object, LogicTypeId p_type, uint32_t p_node) {
		for (const LogicParamField &f : logic_param_fields(p_type)) {
			const std::optional<PropertyValue> value = world_.effective(p_object, f.name);
			BF3_TRY(budget_.status());
			if (!value) {
				continue;
			}
			const int32_t id = field_id(f.name);
			if (f.shape == Shape::Scalar) {
				std::string text;
				BF3_TRY(add_param(p_node, id, 0, logic_value_of(*value, f.kind, &text), text));
				continue;
			}
			if (value->kind != PropertyValue::Kind::Field) {
				continue;
			}
			const OptionalEbxArray array = value->field.as_array();
			if (!array) {
				continue;
			}
			BF3_TRY(budget_.charge(detail::array_cost(*array)));
			if (array->size() > kMaxParamElements) {
				return make_error("logic graph: " + std::string(f.name) + " has " + std::to_string(array->size()) + " elements (more than " +
						std::to_string(kMaxParamElements) + ")");
			}
			uint16_t element = 0;
			for (const EbxValue entry : *array) {
				if (f.shape == Shape::Array) {
					std::string text;
					BF3_TRY(add_param(p_node, id, element, value_of_field(entry, f.kind, &text), text));
				} else {
					// A struct element (inline), or a reference to an instance holding the members.
					std::optional<EbxStruct> data = entry.as_struct();
					if (!data) {
						if (const std::optional<EbxRef> r = entry.as_ref(); r && !r->is_null()) {
							if (const std::optional<EbxInstance> target = db_.resolve(p_object.entity.partition(), *r)) {
								data = target->data();
							}
						}
					}
					if (data) {
						for (const LogicParamMember &member : f.members) {
							if (member.name.empty()) {
								continue;
							}
							BF3_TRY(budget_.charge(1));
							const std::optional<EbxValue> v = data->get(member.name);
							if (v) {
								std::string text;
								BF3_TRY(add_param(p_node, field_id(member.name), element, value_of_field(*v, member.kind, &text), text));
							}
						}
					}
				}
				++element;
			}
		}
		return Status::success();
	}

	// One end of a connection of scope p_scope, as a hop end. nullopt: the end is unbound (counted by the caller).
	std::optional<End> end_of(uint32_t p_scope, const EbxInstance &p_instance, int32_t p_port, bool p_source, KindGraph &r_kind) {
		if (descriptors_[p_scope] && *descriptors_[p_scope] == p_instance) {
			// The scope's own interface: its input as a source, its output as a target.
			return End{ EndKind::Virtual, r_kind.virtual_of(p_scope, p_port, p_source) };
		}
		if (const std::optional<uint32_t> child = children_.find(p_scope, p_instance)) {
			// A child prefab or sub-world: its output as a source, its input as a target.
			return End{ EndKind::Virtual, r_kind.virtual_of(*child, p_port, !p_source) };
		}
		if (const std::optional<uint32_t> object = objects_.find(p_scope, p_instance)) {
			if ((*object & kAbsent) != 0) {
				return End{ EndKind::Absent, *object & ~kAbsent };
			}
			const LogicTypeClass cls = logic_type_info(parts_.nodes[*object].type).cls;
			if (cls == LogicTypeClass::Structural) {
				return std::nullopt; // a reference whose blueprint was not instantiated
			}
			return End{ EndKind::Real, *object };
		}
		return std::nullopt;
	}

	Status connections() {
		struct List {
			const char *name;
			const char *source_field;
			const char *target_field;
			EdgeKind kind;
		};
		static constexpr List kLists[] = {
			{ "EventConnections", "SourceEvent.Id", "TargetEvent.Id", EdgeKind::Event },
			{ "PropertyConnections", "SourceFieldId", "TargetFieldId", EdgeKind::Property },
			{ "LinkConnections", "SourceFieldId", "TargetFieldId", EdgeKind::Link },
		};
		LogicGraphStats &st = parts_.stats;
		for (uint32_t s = 0; s < world_.scopes().size(); ++s) {
			const EbxInstance &blueprint = world_.scopes()[s].blueprint;
			BF3_TRY(budget_.charge_type(blueprint.data()));
			const uint64_t blueprint_word = chain(parts_.scopes[s].path, guid_word(blueprint.partition().guid()));
			for (const List &list : kLists) {
				const std::optional<EbxValue> array_value = blueprint.get(list.name);
				const OptionalEbxArray array = array_value ? array_value->as_array() : OptionalEbxArray();
				if (!array) {
					continue;
				}
				BF3_TRY(budget_.charge(detail::array_cost(*array)));
				KindGraph &kind = kinds_[static_cast<size_t>(list.kind)];
				uint32_t index = 0;
				for (const EbxValue entry : *array) {
					const uint32_t at = index++;
					++st.connections_scanned;
					const auto resolve_end = [&](const char *p_field) -> std::optional<EbxInstance> {
						const std::optional<EbxValue> v = entry.get(p_field);
						const std::optional<EbxRef> r = v ? v->as_ref() : std::nullopt;
						return r ? db_.resolve(blueprint.partition(), *r) : std::nullopt;
					};
					const auto port = [&](const char *p_path) -> int32_t {
						const std::optional<EbxValue> v = entry.get_path(p_path);
						return v ? v->as_i32().value_or(0) : 0;
					};
					const std::optional<EbxInstance> source = resolve_end("Source");
					const std::optional<EbxInstance> target = resolve_end("Target");
					if (!source || !target) {
						++st.dropped_unresolved;
						continue;
					}
					Hop hop;
					hop.source_port = port(list.source_field);
					hop.target_port = port(list.target_field);
					if (list.kind == EdgeKind::Event) {
						hop.target_type = LogicEventTarget::Invalid;
						if (const std::optional<EbxValue> tt = entry.get("TargetType")) {
							const std::optional<std::string_view> name = tt->enum_name();
							const std::optional<LogicEventTarget> parsed = name ? logic_event_target_from_name(*name) : std::nullopt;
							if (parsed) {
								hop.target_type = *parsed;
							} else if (const std::optional<int32_t> raw = tt->enum_value(); raw && *raw >= 0 && *raw <= 5) {
								hop.target_type = static_cast<LogicEventTarget>(*raw);
							}
						}
						hop.connection = chain(blueprint_word, at);
					}
					const std::optional<End> from = end_of(s, *source, hop.source_port, true, kind);
					const std::optional<End> to = end_of(s, *target, hop.target_port, false, kind);
					if (!from || !to) {
						++st.dropped_unbound;
						continue;
					}
					if (from->kind == EndKind::Absent) {
						++st.dropped_realm; // nothing on this side fires it
						continue;
					}
					hop.source = *from;
					hop.target = *to;
					kind.hops.push_back(hop);
				}
			}
		}
		for (KindGraph &k : kinds_) {
			k.seal();
		}
		return Status::success();
	}

	Status emit(EdgeKind p_kind, const Hop &p_first, const Hop &p_last) {
		BF3_TRY(emit_charge());
		LogicGraphStats &st = parts_.stats;
		const uint32_t source = p_first.source.index;
		const bool present = p_last.target.kind == EndKind::Real;
		const uint32_t target = present ? p_last.target.index : kNoLogicIndex;
		if (p_kind != EdgeKind::Event) {
			if (!present) {
				++st.dropped_realm;
				return Status::success();
			}
			parts_.add_edge(p_kind == EdgeKind::Property ? parts_.properties : parts_.links, source, p_first.source_port, target, p_last.target_port,
					LogicEventTarget::Server, false, 0);
			return Status::success();
		}
		const LogicEventTarget tt = p_last.target_type;
		const bool networked = networked_to_clients(tt);
		bool delivered = false;
		if (options_.side == LogicSide::Server) {
			delivered = delivered_on_server(tt);
		} else {
			delivered = tt == LogicEventTarget::Client || tt == LogicEventTarget::ClientAndServer;
		}
		// The networked edges the side fires without delivering: every NetworkedClient edge on the server, a
		// NetworkedClientAndServer edge whose target is absent, and on a client every networked edge (delivered on receipt).
		const bool fire_only = networked && (options_.side == LogicSide::Client || tt == LogicEventTarget::NetworkedClient || !present);
		if (fire_only) {
			parts_.add_edge(parts_.networked, source, p_first.source_port, target, p_last.target_port, tt, true, p_last.connection);
			return Status::success();
		}
		if (!delivered) {
			++st.dropped_target_type;
			return Status::success();
		}
		if (!present) {
			++st.dropped_realm;
			return Status::success();
		}
		parts_.add_edge(parts_.events, source, p_first.source_port, target, p_last.target_port, tt, networked, p_last.connection);
		return Status::success();
	}

	// Depth-first through virtual node p_virtual, emitting every real end reached (record order per hop).
	Status walk(EdgeKind p_kind, KindGraph &r_kind, const Hop &p_first, uint32_t p_virtual, uint32_t p_depth, size_t &r_fanout,
			const std::function<Status(const Hop &)> &p_sink) {
		if (p_depth > options_.max_interface_depth) {
			return make_error("logic graph: interface chain deeper than " + std::to_string(options_.max_interface_depth));
		}
		r_kind.on_path[p_virtual] = 1;
		const Virtual &v = r_kind.virtuals[p_virtual];
		for (uint32_t k = 0; k < v.out_count; ++k) {
			const Hop &hop = r_kind.hops[r_kind.out_hops[v.out_begin + k]];
			BF3_TRY(budget_.charge(1));
			++parts_.stats.interface_hops;
			if (hop.target.kind == EndKind::Virtual) {
				if (r_kind.on_path[hop.target.index] == 0) {
					const Status status = walk(p_kind, r_kind, p_first, hop.target.index, p_depth + 1, r_fanout, p_sink);
					if (!status.ok()) {
						r_kind.on_path[p_virtual] = 0;
						return status;
					}
				}
				continue;
			}
			if (++r_fanout > options_.max_fanout) {
				r_kind.on_path[p_virtual] = 0;
				return make_error("logic graph: one connection expands to more than " + std::to_string(options_.max_fanout) + " edges through interfaces");
			}
			const Status status = p_sink(hop);
			if (!status.ok()) {
				r_kind.on_path[p_virtual] = 0;
				return status;
			}
		}
		r_kind.on_path[p_virtual] = 0;
		(void)p_first;
		return Status::success();
	}

	Status collapse(EdgeKind p_kind) {
		KindGraph &kind = kinds_[static_cast<size_t>(p_kind)];
		for (const Hop &hop : kind.hops) {
			if (hop.source.kind != EndKind::Real) {
				continue;
			}
			BF3_TRY(budget_.charge(1));
			if (hop.target.kind != EndKind::Virtual) {
				BF3_TRY(emit(p_kind, hop, hop));
				continue;
			}
			size_t fanout = 0;
			BF3_TRY(walk(p_kind, kind, hop, hop.target.index, 1, fanout, [&](const Hop &p_last) { return emit(p_kind, hop, p_last); }));
		}
		return Status::success();
	}

	// The descriptor constants of scope p_scope by field id, read once (each field scanned charged).
	Result<const std::vector<std::pair<int32_t, std::string_view>> *> constants_of(uint32_t p_scope) {
		auto it = scope_constants_.find(p_scope);
		if (it != scope_constants_.end()) {
			return &it->second;
		}
		std::vector<std::pair<int32_t, std::string_view>> list;
		if (descriptors_[p_scope]) {
			BF3_TRY(budget_.charge_type(descriptors_[p_scope]->data()));
			const std::optional<EbxValue> fields = descriptors_[p_scope]->get("Fields");
			const OptionalEbxArray array = fields ? fields->as_array() : OptionalEbxArray();
			if (array) {
				BF3_TRY(budget_.charge(detail::array_cost(*array)));
				for (const EbxValue entry : *array) {
					const std::optional<EbxValue> id = entry.get("Id");
					const std::optional<EbxValue> value = entry.get("Value");
					const std::optional<std::string_view> text = value ? value->as_str() : std::nullopt;
					if (id && id->as_i32() && text) {
						list.emplace_back(*id->as_i32(), *text);
					}
				}
			}
		}
		std::stable_sort(list.begin(), list.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
		return &scope_constants_.emplace(p_scope, std::move(list)).first->second;
	}

	Status constants() {
		KindGraph &kind = kinds_[static_cast<size_t>(EdgeKind::Property)];
		for (uint32_t v = 0; v < kind.virtuals.size(); ++v) {
			const Virtual &vn = kind.virtuals[v];
			if (!vn.in || vn.incoming != 0 || vn.out_count == 0) {
				continue;
			}
			BF3_TRY(budget_.charge(1));
			Result<const std::vector<std::pair<int32_t, std::string_view>> *> list = constants_of(vn.scope);
			if (!list) {
				return list.error();
			}
			const auto found = std::lower_bound((*list)->begin(), (*list)->end(), vn.port, [](const auto &a, int32_t p) { return a.first < p; });
			if (found == (*list)->end() || found->first != vn.port) {
				continue;
			}
			BF3_TRY(budget_.charge(1 + found->second.size() / WorkBudget::kBytesPerUnit));
			const std::optional<PropertyValue> parsed = PropertyValue::parse(found->second);
			if (!parsed) {
				continue;
			}
			K value_kind = K::None;
			switch (parsed->kind) {
				case PropertyValue::Kind::Bool:
					value_kind = K::Bool;
					break;
				case PropertyValue::Kind::Int:
					value_kind = K::Int;
					break;
				case PropertyValue::Kind::Float:
					value_kind = K::Float;
					break;
				case PropertyValue::Kind::Text:
					value_kind = K::Text;
					break;
				case PropertyValue::Kind::Team:
					value_kind = K::Team;
					break;
				case PropertyValue::Kind::Vec3:
					value_kind = K::Vec3;
					break;
				case PropertyValue::Kind::Transform:
					value_kind = K::Transform;
					break;
				default:
					break;
			}
			if (value_kind == K::None) {
				continue;
			}
			std::string text;
			std::optional<LogicValue> value = logic_value_of(*parsed, value_kind, &text);
			if (!value) {
				continue;
			}
			if (value->kind == K::Text) {
				value->i = intern(text);
				BF3_TRY(budget_.status());
			}
			Hop first;
			size_t fanout = 0;
			const LogicValue constant = *value;
			BF3_TRY(walk(EdgeKind::Property, kind, first, v, 1, fanout, [&](const Hop &p_last) -> Status {
				if (p_last.target.kind != EndKind::Real) {
					return Status::success();
				}
				BF3_TRY(emit_charge());
				LogicInput in;
				in.port = p_last.target_port;
				in.value = constant;
				parts_.inputs.emplace_back(p_last.target.index, in);
				return Status::success();
			}));
		}
		return Status::success();
	}

	const LogicWorld &world_;
	const EbxResolver &db_;
	const LogicGraphOptions &options_;
	WorkBudget &budget_;
	GraphParts parts_;
	std::vector<std::optional<EbxInstance>> descriptors_;
	ScopedIndex children_; // (parent scope, reference) -> child scope
	ScopedIndex objects_; // (scope, entity or placing reference) -> node, or kAbsent | object
	KindGraph kinds_[3];
	std::map<uint32_t, std::vector<std::pair<int32_t, std::string_view>>> scope_constants_;
};

} // namespace

Result<LogicGraph> LogicGraphBuild::from_world(const LogicWorld &p_world, const LogicGraphOptions &p_options) {
	GraphBuilder builder(p_world, p_options);
	return builder.run();
}

Result<LogicGraph> build_logic_graph(const LogicWorld &p_world, const LogicGraphOptions &p_options) {
	return LogicGraphBuild::from_world(p_world, p_options);
}

Result<LogicGraph> build_logic_graph(const EbxResolver &p_db, const EbxInstance &p_root, const LogicGraphOptions &p_options) {
	LogicWorldOptions wo;
	wo.mode = p_options.mode;
	wo.expand_root_objects = true;
	wo.max_depth = p_options.max_depth;
	wo.max_scopes = p_options.max_scopes;
	wo.max_objects = p_options.max_objects;
	wo.max_walk_steps = p_options.max_walk_steps;
	wo.max_query_steps = p_options.max_query_steps;
	wo.max_work = p_options.max_work;
	wo.budget = p_options.budget;
	Result<LogicWorld> world = LogicWorld::build(p_db, p_root, wo);
	if (!world) {
		return error_at("logic graph walk", world.error());
	}
	return build_logic_graph(*world, p_options);
}

} // namespace bf3::core::gameplay
