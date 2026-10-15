#pragma once
// S4 level logic (docs/design/s4.md §5): the instantiated, flattened logic graph of one level x mode, as the server
// (or a client) sees it. Built once at level load from a LogicWorld; immutable afterwards and shared between threads
// as std::shared_ptr<const LogicGraph>. The interpreter (bf3/sim/logic/logic_runtime.h) runs on it.
//
// Instantiation (plan D12 revised option c, §3.11; gap logic §2-§4):
//  * LogicWorld walks the level with expand_root_objects on: every included sub-world (GameMode inclusion, AutoLoad),
//    every prefab some ReferenceObjectData instantiates (one scope per instance), world parts transparent, the
//    Object of every ObjectBlueprint reference emitted with that reference, spawn references kept as entities.
//  * Every object of a scope becomes one node, connected or not (Metro RushLarge0: 145 SyncedBool nodes, 125 of them on
//    server connections). Its Realm decides whether the node exists on this side; a node that does not is not created
//    and every edge touching it is dropped and counted (networked events excepted, see networked_edges()). Unknown
//    and unimplemented types are gated only on connected nodes (at least one edge of any kind after routing and
//    collapse, kLogicNodeConnected): unconnected objects of unlisted types (RoadData, LightProbeVolumeData, ... on
//    every row) cannot send or receive and are counted apart as info.
//  * Prefab interfaces are flattened. Each scope has a virtual IfaceIn and IfaceOut per interface port; the parent's
//    X.a -> Ref.p becomes X.a -> IfaceIn.p, the child's Descriptor.p -> Y.b becomes IfaceIn.p -> Y.b, Y.b ->
//    Descriptor.p becomes Y.b -> IfaceOut.p, the parent's Ref.p -> Z.c becomes IfaceOut.p -> Z.c. The virtual nodes are
//    then collapsed into direct node -> node edges (depth first, in connection array order), so the runtime never
//    sees an interface. The walk marks the virtual nodes on the current path only (set on entry, cleared on exit), so
//    an interface cycle stops and distinct paths are all kept: a diamond through interfaces delivers once per path,
//    as forwarding connection by connection would; such repeats are counted (stats.duplicate_paths). Every hop
//    charges the budget, so path explosion fails with an error instead of running. Property inputs whose interface
//    chain ends at a descriptor constant become the node's initial input value. Edges into a reference whose
//    blueprint was not instantiated (Excluded, missing, sub-world not in this mode) are dropped and counted.
//  * Edge order is stable: the order of the connection arrays in the walk order of the scopes, collapsed paths in the
//    order their first hop appears. The runtime fans an event out in this order.
//
// Hostile data: one gameplay::WorkBudget bounds the walk (LogicWorld's own charges), every connection entry scanned,
// every interface hop collapsed, every parameter read and every node and edge emitted. Node, edge, parameter and
// per-port fan-out counts have hard caps on top (LogicGraphOptions); a graph that exceeds one fails with an error.
#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/logic_types.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/ids.h"
#include "bf3/core/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::core::gameplay {

class LogicWorld;

// Bumped whenever the build rules or the layout below change; part of digest() and of any cache key.
inline constexpr uint32_t kLogicGraphVersion = 1;

inline constexpr uint32_t kNoLogicIndex = 0xffffffffu;

// A property value as the interpreter carries it. Plain data; equality is bitwise (properties propagate on change,
// and "change" must not depend on float comparison rules: NaN to NaN is no change, -0 to +0 is one).
struct LogicValue {
	enum class Kind : uint8_t { None = 0, Bool, Int, Float, Vec3, Transform, Text, Team };
	Kind kind = Kind::None;
	bool b = false;
	int64_t i = 0; // Int, Team (TeamNeutral 0), Text (index into LogicGraph::texts())
	double f = 0;
	std::array<float, 12> m{}; // Vec3 in m[0..2]; Transform rows right, up, forward, trans
	bool same_as(const LogicValue &p_other) const; // bitwise on the fields the kind uses
};
static_assert(sizeof(LogicValue) <= 80, "LogicValue is copied through the event queue");

// One configuration field of a node (DelayEntityData.Delay, SequenceEntityData.Events[3].EventTime, ...), read from
// the node's effective value at build time. `field` is ebx_name_hash of the field name; array elements repeat the
// field with element 0, 1, ...; members of an array element are flattened as their own field ids.
struct LogicParam {
	int32_t field = 0;
	uint16_t element = 0;
	LogicValue value;
};

// The configuration fields one node type reads (docs/design/s4.md §6.2), in the order the builder reads them. Each is
// read as LogicWorld::effective (a property connection into the field or the placing reference, else the entity's
// own field) and converted to `kind` (a field that does not convert is left out). A scalar array gives one param per
// element (element 0, 1, ...); a struct array gives, per element, one param per member (field = the member's id).
// At most kMaxParamElements elements per array; a longer array fails the build.
struct LogicParamMember {
	std::string_view name;
	LogicValue::Kind kind = LogicValue::Kind::Int;
};
struct LogicParamField {
	enum class Shape : uint8_t { Scalar, Array, StructArray };
	std::string_view name;
	LogicValue::Kind kind = LogicValue::Kind::Bool; // Scalar and Array
	Shape shape = Shape::Scalar;
	LogicParamMember members[2] = {}; // StructArray (empty name: unused)
};
inline constexpr size_t kMaxParamElements = 4096;
std::span<const LogicParamField> logic_param_fields(LogicTypeId p_type);

struct PropertyValue;
// The conversion the builder applies to an effective value (scalar fields): nullopt when the value does not convert to
// p_kind. A Text value is returned with i = 0 and its text in *r_text (the builder interns it into texts()).
std::optional<LogicValue> logic_value_of(const PropertyValue &p_value, LogicValue::Kind p_kind, std::string *r_text = nullptr);
// FNV-1a 64 of a value's kind and the fields that kind uses (little-endian, bitwise floats); the trace and the digest
// hash values with it.
uint64_t logic_value_hash(const LogicValue &p_value);

// LogicNode::flags.
inline constexpr uint8_t kLogicNodeConnected = 1u << 0; // at least one edge (any kind, either end) after collapse

struct LogicNode {
	LogicTypeId type = LogicTypeId::Unknown;
	LogicRealm realm = LogicRealm::Unset;
	uint8_t flags = 0;
	uint32_t scope = 0; // index into LogicGraph::scopes()
	Guid guid; // the entity's instance GUID (shared by every instance of one prefab)
	// Stable identity across runs and machines: hash_mix chain over the scope path (reference GUIDs from the root)
	// and the entity GUID. Ghost rows, RandomDelay seeds and trace records use it.
	uint64_t key = 0;
	uint32_t type_name = kNoLogicIndex; // texts() index of the EBX type name (kept for Unknown nodes)
	uint32_t param_begin = 0, param_count = 0; // into params()
	uint32_t input_begin = 0, input_count = 0; // into initial_inputs(): constant property inputs
	uint32_t event_out_begin = 0, event_out_count = 0; // into event_edges(), sorted by source port then order
	uint32_t property_out_begin = 0, property_out_count = 0; // into property_edges()
	uint32_t link_out_begin = 0, link_out_count = 0; // into link_edges()
};

struct LogicEdge {
	uint32_t source = 0; // node index
	int32_t source_port = 0;
	uint32_t target = 0; // node index
	int32_t target_port = 0;
	uint32_t order = 0; // global stable order within the edge kind
	LogicEventTarget target_type = LogicEventTarget::ClientAndServer; // events only
	bool networked = false; // events: sent to clients (networked_to_clients)
	// Events: identity of the EBX connection of the last hop (the one into the real target, whose TargetType this
	// edge has): hash_mix chain over the owning scope's path, the blueprint partition GUID and the index in its
	// EventConnections array. The server and a client compute the same id from the same data, so it is the id a
	// networked event carries (plan §3.5) even when the target exists on one side only.
	uint64_t connection = 0;
};

// A constant property input: the value a descriptor constant (or a source the build could not keep as a node, e.g.
// a Data-class field) feeds into port `port` of a node at start.
struct LogicInput {
	int32_t port = 0;
	LogicValue value;
};

// Data identity of a node for scripts, judges and tools (never used by the interpreter): the entity GUID, plus the
// GUID of a reference that instantiated one of the node's enclosing scopes (any depth) to tell prefab instances apart;
// zero matches any scope. Example: Caspian flag f3042a9e is {reference f3042a9e, entity zero} with type CapturePoint
// (the CapturePoint sits in the nested capturepointprefabflagpole scope, shared GUID 237dadc8 on every flag).
struct LogicNodeRef {
	Guid scope_reference;
	Guid entity; // zero: any entity of the requested type
};

struct LogicScopeInfo {
	uint32_t parent = kNoLogicIndex;
	uint32_t depth = 0;
	Guid reference; // the instantiating reference's GUID; zero for the root
	uint64_t path = 0; // hash_mix chain of reference GUIDs from the root
	uint32_t name = kNoLogicIndex; // texts() index of the blueprint partition name
};

struct LogicGraphStats {
	size_t scopes = 0;
	size_t objects = 0; // LogicWorld objects examined
	size_t nodes_by_class[6] = {}; // LogicTypeClass order
	// The gated counts (G1, G5) are over connected nodes only (kLogicNodeConnected); the unconnected ones are info.
	size_t unknown_nodes = 0;
	size_t unimplemented_nodes = 0; // Primitive and Native rows with s4 false, on this side
	size_t unknown_unconnected = 0; // [Metro + Caspian 229-235 per row, 13 types; 16 types over 230 rows]
	size_t unimplemented_unconnected = 0;
	size_t event_edges = 0, property_edges = 0, link_edges = 0;
	size_t networked_only_edges = 0; // networked_edges(): fired for clients, not delivered on this side
	size_t duplicate_paths = 0; // collapsed edges equal in (source, port, target, port) to an earlier one (kept)
	size_t connections_scanned = 0;
	size_t interface_hops = 0; // virtual interface edges collapsed
	size_t dropped_realm = 0; // an end does not exist on this side
	size_t dropped_target_type = 0; // event TargetType not delivered on this side
	size_t dropped_unbound = 0; // an end is a reference that was not instantiated, or not an object of the scope
	size_t dropped_unresolved = 0; // an end does not resolve in the loaded partitions
	size_t synced_bools = 0;
	size_t work = 0; // budget units used by the walk and the build
	// Per EBX type name: nodes of Unknown type and nodes of unimplemented rows (at most kMaxListedTypes entries each).
	static constexpr size_t kMaxListedTypes = 1024;
	std::vector<std::pair<std::string, size_t>> unknown_types;
	std::vector<std::pair<std::string, size_t>> unimplemented_types;
};

enum class LogicSide : uint8_t { Server, Client };

struct LogicGraphOptions {
	std::string mode; // GameMode option id ("RushLarge0"); empty includes every auto-loaded sub-world
	LogicSide side = LogicSide::Server;
	// Caps on top of the budget (retail maximum in brackets, census 2026-10-15).
	size_t max_nodes = 1u << 20; // [8,360 objects, MP_003 RushLarge0]
	size_t max_edges = 1u << 22; // all kinds together [~9.6k connections per row]
	size_t max_params = 1u << 22;
	size_t max_fanout = 1u << 16; // edges one collapsed start edge may produce (paths through interfaces)
	uint32_t max_interface_depth = 48; // hops through nested interfaces (LogicWorld max_depth)
	// LogicWorld caps (see LogicWorldOptions).
	uint32_t max_depth = 48;
	size_t max_scopes = 200000;
	size_t max_objects = 4000000;
	size_t max_walk_steps = 2000000;
	size_t max_query_steps = 2000000;
	// Total work of walk plus build; 1M units like every gameplay reader [walk alone 34,118 max, MP_003 RushLarge0].
	size_t max_work = WorkBudget::kDefaultLimit;
	WorkBudget *budget = nullptr;
};

class LogicGraph {
public:
	LogicGraph(LogicGraph &&) noexcept = default;
	LogicGraph &operator=(LogicGraph &&) noexcept = default;

	std::span<const LogicNode> nodes() const { return nodes_; }
	std::span<const LogicEdge> event_edges() const { return event_edges_; }
	std::span<const LogicEdge> property_edges() const { return property_edges_; }
	std::span<const LogicEdge> link_edges() const { return link_edges_; }
	// Networked events this side fires but does not deliver: every NetworkedClient edge, and NetworkedClientAndServer
	// edges whose target does not exist on this side (target kNoLogicIndex then). The server fires them for clients
	// (e.g. Metro park Delay 456fc3ec.Out -> Sound 5a962122, NetworkedClient). Sorted like event_edges.
	std::span<const LogicEdge> networked_edges() const { return networked_edges_; }
	std::span<const LogicParam> params() const { return params_; }
	std::span<const LogicInput> initial_inputs() const { return inputs_; }
	std::span<const LogicScopeInfo> scopes() const { return scopes_; }
	std::span<const std::string> texts() const { return texts_; }
	const LogicGraphStats &stats() const { return stats_; }
	LogicSide side() const { return side_; }

	std::span<const LogicParam> params_of(uint32_t p_node) const;
	// First param with that field (and element), nullopt when absent.
	std::optional<LogicValue> param(uint32_t p_node, int32_t p_field, uint16_t p_element = 0) const;
	std::span<const LogicEdge> events_from(uint32_t p_node) const;
	std::span<const LogicEdge> properties_from(uint32_t p_node) const;
	std::span<const LogicEdge> links_from(uint32_t p_node) const;
	// Nodes with that entity GUID, in node order (one per prefab instance). No allocation.
	std::span<const uint32_t> nodes_with_guid(const Guid &p_guid) const;
	// Nodes of one type, in node order. No allocation.
	std::span<const uint32_t> nodes_of_type(LogicTypeId p_type) const;
	// Nodes of type p_type (any type when Unknown) matching p_ref, in node order. Linear in the nodes of that type
	// times the scope depth (<= max_depth); for scripts and tools, not for the tick.
	std::vector<uint32_t> find_nodes(const LogicNodeRef &p_ref, LogicTypeId p_type = LogicTypeId::Unknown) const;
	// Whether scope p_scope is p_ancestor or lies below it.
	bool scope_within(uint32_t p_scope, uint32_t p_ancestor) const;
	std::string_view text(uint32_t p_index) const;

	// FNV-1a 64 over the canonical content (versions, type names, keys, params, inputs, edges in order). Equal for
	// equal data on any machine; the plan §2.7 sim_digest includes it.
	uint64_t digest() const;

	// Assembles a graph from parts (unit tests, fuzz, synthetic scripts). finish() sorts edges by (source, port, order),
	// builds the indices and validates every index and count; bad input is an error, never a crash.
	class Builder {
	public:
		explicit Builder(LogicSide p_side = LogicSide::Server);
		uint32_t add_scope(uint32_t p_parent, const Guid &p_reference, std::string_view p_name);
		uint32_t add_node(LogicTypeId p_type, LogicRealm p_realm, uint32_t p_scope, const Guid &p_guid);
		void add_param(uint32_t p_node, int32_t p_field, const LogicValue &p_value, uint16_t p_element = 0);
		void add_input(uint32_t p_node, int32_t p_port, const LogicValue &p_value);
		void add_event(uint32_t p_source, int32_t p_source_port, uint32_t p_target, int32_t p_target_port,
				LogicEventTarget p_target_type = LogicEventTarget::Server, uint64_t p_connection = 0);
		// A networked event this side fires but does not deliver (see networked_edges()); p_target may be kNoLogicIndex.
		void add_networked_only(uint32_t p_source, int32_t p_source_port, uint32_t p_target, int32_t p_target_port, LogicEventTarget p_target_type,
				uint64_t p_connection);
		void add_property(uint32_t p_source, int32_t p_source_port, uint32_t p_target, int32_t p_target_port);
		void add_link(uint32_t p_source, int32_t p_source_port, uint32_t p_target, int32_t p_target_port);
		Result<LogicGraph> finish(const LogicGraphOptions &p_caps = {});

	private:
		struct Parts;
		std::shared_ptr<Parts> parts_;
	};

private:
	LogicGraph() = default;
	friend class LogicGraphBuild;

	LogicSide side_ = LogicSide::Server;
	std::vector<LogicNode> nodes_;
	std::vector<LogicEdge> event_edges_, property_edges_, link_edges_, networked_edges_;
	std::vector<LogicParam> params_;
	std::vector<LogicInput> inputs_;
	std::vector<LogicScopeInfo> scopes_;
	std::vector<std::string> texts_;
	std::vector<uint32_t> by_guid_; // node indices sorted by (guid, index)
	std::vector<uint32_t> by_type_; // node indices sorted by (type, index)
	LogicGraphStats stats_;
};

// Walks p_root (a LevelData) for p_options.mode and builds the graph. The resolver must outlive the call only (the
// graph copies everything it keeps).
Result<LogicGraph> build_logic_graph(const EbxResolver &p_db, const EbxInstance &p_root, const LogicGraphOptions &p_options);
// The same over a LogicWorld the caller already built (it must have been built with expand_root_objects on); the
// world's budget is charged.
Result<LogicGraph> build_logic_graph(const LogicWorld &p_world, const LogicGraphOptions &p_options);

} // namespace bf3::core::gameplay
