#pragma once

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

inline constexpr uint32_t kLogicGraphVersion = 1;

inline constexpr uint32_t kNoLogicIndex = 0xffffffffu;

struct LogicValue {
	enum class Kind : uint8_t { None = 0, Bool, Int, Float, Vec3, Transform, Text, Team };
	Kind kind = Kind::None;
	bool b = false;
	int64_t i = 0;
	double f = 0;
	std::array<float, 12> m{};
	bool same_as(const LogicValue &p_other) const;
};
static_assert(sizeof(LogicValue) <= 80, "LogicValue is copied through the event queue");

struct LogicParam {
	int32_t field = 0;
	uint16_t element = 0;
	LogicValue value;
};

struct LogicParamMember {
	std::string_view name;
	LogicValue::Kind kind = LogicValue::Kind::Int;
};
struct LogicParamField {
	enum class Shape : uint8_t { Scalar, Array, StructArray };
	std::string_view name;
	LogicValue::Kind kind = LogicValue::Kind::Bool;
	Shape shape = Shape::Scalar;
	LogicParamMember members[2] = {};
};
inline constexpr size_t kMaxParamElements = 4096;
std::span<const LogicParamField> logic_param_fields(LogicTypeId p_type);

struct PropertyValue;

std::optional<LogicValue> logic_value_of(const PropertyValue &p_value, LogicValue::Kind p_kind, std::string *r_text = nullptr);

uint64_t logic_value_hash(const LogicValue &p_value);

inline constexpr uint8_t kLogicNodeConnected = 1u << 0;

struct LogicNode {
	LogicTypeId type = LogicTypeId::Unknown;
	LogicRealm realm = LogicRealm::Unset;
	uint8_t flags = 0;
	uint32_t scope = 0;
	Guid guid;

	uint64_t key = 0;
	uint32_t type_name = kNoLogicIndex;
	uint32_t param_begin = 0, param_count = 0;
	uint32_t input_begin = 0, input_count = 0;
	uint32_t event_out_begin = 0, event_out_count = 0;
	uint32_t property_out_begin = 0, property_out_count = 0;
	uint32_t link_out_begin = 0, link_out_count = 0;
};

struct LogicEdge {
	uint32_t source = 0;
	int32_t source_port = 0;
	uint32_t target = 0;
	int32_t target_port = 0;
	uint32_t order = 0;
	LogicEventTarget target_type = LogicEventTarget::ClientAndServer;
	bool networked = false;

	uint64_t connection = 0;
};

struct LogicEntry {
	uint32_t scope = 0;
	int32_t port = 0;
	uint32_t target = 0;
	int32_t target_port = 0;
	LogicEventTarget target_type = LogicEventTarget::ClientAndServer;
};

struct LogicInput {
	int32_t port = 0;
	LogicValue value;
};

struct LogicNodeRef {
	Guid scope_reference;
	Guid entity;
};

struct LogicScopeInfo {
	uint32_t parent = kNoLogicIndex;
	uint32_t depth = 0;
	Guid reference;
	uint64_t path = 0;
	uint32_t name = kNoLogicIndex;
};

struct LogicGraphStats {
	size_t scopes = 0;
	size_t objects = 0;
	size_t nodes_by_class[6] = {};

	size_t unknown_nodes = 0;
	size_t unimplemented_nodes = 0;
	size_t unknown_unconnected = 0;
	size_t unimplemented_unconnected = 0;
	size_t event_edges = 0, property_edges = 0, link_edges = 0;
	size_t networked_only_edges = 0;
	size_t duplicate_paths = 0;
	size_t connections_scanned = 0;
	size_t interface_hops = 0;
	size_t dropped_realm = 0;
	size_t dropped_target_type = 0;
	size_t dropped_unbound = 0;
	size_t dropped_unbound_events = 0;
	size_t dropped_unresolved = 0;
	size_t synced_bools = 0;
	size_t work = 0;

	static constexpr size_t kMaxListedTypes = 1024;
	std::vector<std::pair<std::string, size_t>> unknown_types;
	std::vector<std::pair<std::string, size_t>> unimplemented_types;
};

enum class LogicSide : uint8_t { Server, Client };

struct LogicGraphOptions {
	std::string mode;
	LogicSide side = LogicSide::Server;

	size_t max_nodes = 1u << 20;
	size_t max_edges = 1u << 22;
	size_t max_params = 1u << 22;
	size_t max_fanout = 1u << 16;
	uint32_t max_interface_depth = 48;

	uint32_t max_depth = 48;
	size_t max_scopes = 200000;
	size_t max_objects = 4000000;
	size_t max_walk_steps = 2000000;
	size_t max_query_steps = 2000000;

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

	std::span<const LogicEdge> networked_edges() const { return networked_edges_; }
	std::span<const LogicEntry> entries() const { return entries_; }
	std::span<const LogicEntry> entries_of(uint32_t p_scope, int32_t p_port) const;
	std::optional<uint32_t> scope_named(std::string_view p_name) const;
	std::span<const LogicParam> params() const { return params_; }
	std::span<const LogicInput> initial_inputs() const { return inputs_; }
	std::span<const LogicScopeInfo> scopes() const { return scopes_; }
	std::span<const std::string> texts() const { return texts_; }
	const LogicGraphStats &stats() const { return stats_; }
	LogicSide side() const { return side_; }

	std::span<const LogicParam> params_of(uint32_t p_node) const;

	std::optional<LogicValue> param(uint32_t p_node, int32_t p_field, uint16_t p_element = 0) const;
	std::span<const LogicEdge> events_from(uint32_t p_node) const;
	std::span<const LogicEdge> properties_from(uint32_t p_node) const;
	std::span<const LogicEdge> links_from(uint32_t p_node) const;

	std::span<const uint32_t> nodes_with_guid(const Guid &p_guid) const;

	std::span<const uint32_t> nodes_of_type(LogicTypeId p_type) const;

	std::vector<uint32_t> find_nodes(const LogicNodeRef &p_ref, LogicTypeId p_type = LogicTypeId::Unknown) const;

	bool scope_within(uint32_t p_scope, uint32_t p_ancestor) const;
	std::string_view text(uint32_t p_index) const;

	uint64_t digest() const;

	class Builder {
	public:
		explicit Builder(LogicSide p_side = LogicSide::Server);
		uint32_t add_scope(uint32_t p_parent, const Guid &p_reference, std::string_view p_name);
		uint32_t add_node(LogicTypeId p_type, LogicRealm p_realm, uint32_t p_scope, const Guid &p_guid);
		void add_param(uint32_t p_node, int32_t p_field, const LogicValue &p_value, uint16_t p_element = 0);
		void add_input(uint32_t p_node, int32_t p_port, const LogicValue &p_value);

		uint32_t add_text(std::string_view p_text);
		void add_event(uint32_t p_source, int32_t p_source_port, uint32_t p_target, int32_t p_target_port,
				LogicEventTarget p_target_type = LogicEventTarget::Server, uint64_t p_connection = 0);

		void add_networked_only(uint32_t p_source, int32_t p_source_port, uint32_t p_target, int32_t p_target_port, LogicEventTarget p_target_type,
				uint64_t p_connection);
		void add_property(uint32_t p_source, int32_t p_source_port, uint32_t p_target, int32_t p_target_port);
		void add_link(uint32_t p_source, int32_t p_source_port, uint32_t p_target, int32_t p_target_port);
		void add_entry(uint32_t p_scope, int32_t p_port, uint32_t p_target, int32_t p_target_port,
				LogicEventTarget p_target_type);
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
	std::vector<LogicEntry> entries_;
	std::vector<LogicParam> params_;
	std::vector<LogicInput> inputs_;
	std::vector<LogicScopeInfo> scopes_;
	std::vector<std::string> texts_;
	std::vector<uint32_t> by_guid_;
	std::vector<uint32_t> by_type_;
	LogicGraphStats stats_;
};

Result<LogicGraph> build_logic_graph(const EbxResolver &p_db, const EbxInstance &p_root, const LogicGraphOptions &p_options);

Result<LogicGraph> build_logic_graph(const LogicWorld &p_world, const LogicGraphOptions &p_options);

}
