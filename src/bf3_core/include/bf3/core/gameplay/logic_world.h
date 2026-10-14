#pragma once
// Blueprint scopes of a level x mode and the property resolver over them: the walk that plan §3.7
// calls "interface -> PropertyConnection -> entity field". Ported from IW4L level_bf3
// (connections.rs: Connections, Property, Scopes; world.rs: the walker, without transforms and
// geometry; level.rs: includes), Apache-2.0.
//
// A scope is one instance of a blueprint: the level, an included sub-world, or a prefab some
// reference instantiated. Connections are stored on the blueprint, and world parts are
// transparent: the objects of a layer belong to the scope that holds the layer reference. A
// value fed into a field is resolved through the scope's interface into the parent scopes and
// through the output ports of child prefabs, down to an interface constant ("Int32 250") or the
// data field of the connected source object.
//
// Everything points into partitions of the EbxResolver, which must outlive the LogicWorld.
// Hostile data: scope count, object count, nesting depth and resolver steps are bounded; a level
// that exceeds them fails with an error instead of growing without limit.

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/result.h"

#include <array>
#include <deque>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bf3::core::gameplay {

// Connection field ids and interface ids: ebx_name_hash of the UpperCamel field name, signed as
// the connection records store it.
inline int32_t field_id(std::string_view p_name) {
	return static_cast<int32_t>(ebx_name_hash(p_name));
}

struct InstanceKey {
	const EbxPartition *partition = nullptr;
	uint32_t index = 0;

	static InstanceKey of(const EbxInstance &p_instance) { return { &p_instance.partition(), p_instance.index() }; }
	bool operator==(const InstanceKey &) const = default;
};

struct InstanceKeyHash {
	size_t operator()(const InstanceKey &p_key) const noexcept {
		return std::hash<const void *>()(p_key.partition) ^ (static_cast<size_t>(p_key.index) * 0x9E3779B97F4A7C15ull);
	}
};

// A value fed into a field through a PropertyConnection: an interface constant or the data field
// of the connected source object.
struct PropertyValue {
	enum class Kind : uint8_t {
		Bool,
		Int,
		Float,
		Text, // a CString constant without its quotes
		Team, // "TeamId TeamN" as N; TeamNeutral is 0
		Vec3,
		Transform, // rows right, up, forward, trans
		Constant, // a constant of a type this reader does not interpret, as written
		Field, // the data field of the source object
		Ref, // an interface field's ValueRef
	};

	Kind kind = Kind::Constant;
	bool b = false;
	int64_t i = 0;
	double f = 0;
	std::string_view text;
	std::array<std::array<float, 3>, 4> rows{};
	EbxValue field;
	std::optional<EbxInstance> ref;

	// An interface DataField.Value; empty (after trimming) means "no constant".
	static std::optional<PropertyValue> parse(std::string_view p_constant);
	static PropertyValue of_field(const EbxValue &p_value);

	std::optional<bool> as_bool() const;
	std::optional<int64_t> as_i64() const;
	// Floats, and integers converted.
	std::optional<double> as_f64() const;
	std::optional<std::string_view> as_str() const;
	// Team number: TeamId constants, integers and TeamId enum fields ("Team2" -> 2, "TeamNeutral" -> 0).
	std::optional<int> as_team() const;
	std::optional<std::array<float, 3>> as_vec3() const;
	std::optional<std::string_view> enum_name() const;
	// A reference field value or ValueRef, unresolved form (resolve with the EbxResolver).
	std::optional<EbxRef> as_ref() const;
};

struct LogicConnection {
	EbxInstance source;
	int32_t source_field = 0;
	EbxInstance target;
	int32_t target_field = 0;
};

// The PropertyConnections, LinkConnections and EventConnections of one blueprint, both ends
// resolved. For events the field ids are the event ids.
struct LogicConnections {
	std::vector<LogicConnection> property;
	std::vector<LogicConnection> link;
	std::vector<LogicConnection> event;
	size_t unresolved = 0; // connections with an end that does not resolve in the loaded partitions

	static LogicConnections of(const EbxResolver &p_db, const EbxInstance &p_blueprint);

	// Indices into `property` of the connections into (target, field).
	std::vector<uint32_t> property_into(const EbxInstance &p_target, int32_t p_field) const;

private:
	struct TargetKey {
		InstanceKey target;
		int32_t field = 0;
		bool operator==(const TargetKey &) const = default;
	};
	struct TargetKeyHash {
		size_t operator()(const TargetKey &p_key) const noexcept { return InstanceKeyHash()(p_key.target) ^ (static_cast<size_t>(static_cast<uint32_t>(p_key.field)) << 1); }
	};
	std::unordered_multimap<TargetKey, uint32_t, TargetKeyHash> by_target_;
};

struct LogicScope {
	EbxInstance blueprint;
	std::optional<EbxInstance> reference; // the reference that instantiated the blueprint; none for the root
	std::optional<uint32_t> parent;
	uint32_t depth = 0;
	uint32_t connections = 0; // index into LogicWorld's connection sets
};

// One object of a scope: an entity of the blueprint (through world parts), a spawn reference, or
// the Object of an ObjectBlueprint some reference placed (then `reference` is that reference).
struct LogicObject {
	EbxInstance entity;
	std::optional<EbxInstance> reference;
	uint32_t scope = 0;
};

struct LogicWorldOptions {
	// The GameMode option id the sub-world inclusion settings are tested against
	// ("ConquestSmall0"); empty includes every auto-loaded sub-world.
	std::string mode;
	// Expand prefabs and ObjectBlueprint references placed directly in the root blueprint. Off for
	// a LevelData root: its own layers hold the static geometry, and mode logic lives in the
	// sub-worlds (the root still feeds their interfaces).
	bool expand_root_objects = false;
	uint32_t max_depth = 48;
	size_t max_scopes = 200000;
	size_t max_objects = 4000000;
	// Work the whole walk may do: one step per container visited and per Objects entry examined
	// (null and unresolvable entries included), one per inclusion setting and enabled option of
	// every sub-world reference reached, plus the resolver steps of every Excluded query. World parts push no scope or object, so
	// without this a fan-out of nested world parts would run k^max_depth visits.
	size_t max_walk_steps = 2000000; // retail levels need at most ~7k (MP_Subway Rush)
	// Resolver steps all queries may take together, the walk's own Excluded queries included (each
	// query is also capped at kQuerySteps). The budget is checked before each query, so the total
	// can overshoot it by up to kQuerySteps. Once spent, queries return nullopt and
	// query_budget_spent() is true; a walk that spends it fails with an error.
	size_t max_query_steps = 2000000; // a retail mode read needs < 4k (fields scanned included)
};

struct LogicWorldStats {
	size_t excluded = 0; // references skipped because they are Excluded
	size_t unresolved_objects = 0; // Objects entries that do not resolve
	size_t missing_blueprints = 0;
	size_t depth_limited = 0;
	size_t cycles = 0; // references to a blueprint already being walked (skipped)
	size_t walk_steps = 0; // against LogicWorldOptions::max_walk_steps
	// Each name list below keeps at most this many entries (far more than a level references).
	static constexpr size_t kMaxListedSubworlds = 4096;
	std::vector<std::string> subworlds; // included sub-world bundle names, in visit order
	std::vector<std::string> skipped_subworlds; // sub-worlds the mode does not include
	std::vector<std::string> absent_subworlds; // included but not in the resolver
};

class LogicWorld {
public:
	// Walks p_root (LevelData, SubWorldData or a prefab blueprint) and every included sub-world and
	// prefab below it.
	static Result<LogicWorld> build(const EbxResolver &p_db, const EbxInstance &p_root, const LogicWorldOptions &p_options);

	LogicWorld(LogicWorld &&) noexcept = default;
	LogicWorld &operator=(LogicWorld &&) noexcept = default;

	const EbxResolver &db() const { return *db_; }
	const std::vector<LogicScope> &scopes() const { return scopes_; }
	const std::vector<LogicObject> &objects() const { return objects_; }
	const LogicWorldStats &stats() const { return stats_; }
	const LogicConnections &connections(uint32_t p_scope) const { return sets_[scopes_[p_scope].connections]; }
	// The partition name of a scope's blueprint (lowercase), empty when unknown.
	std::string_view scope_name(uint32_t p_scope) const;

	// The value a PropertyConnection of p_scope feeds into p_field of p_target, an object of that
	// scope. Not thread-safe (it caches connection sets of prefabs it passes through): every query
	// method may modify that cache, so a LogicWorld must not be shared between threads.
	std::optional<PropertyValue> property(uint32_t p_scope, const EbxInstance &p_target, int32_t p_field) const;
	// Interface field p_field of the scope's blueprint for this instance: what the parent connects
	// into the instantiating reference, else the descriptor's constant.
	std::optional<PropertyValue> interface_value(uint32_t p_scope, int32_t p_field) const;
	// The effective value of a field of an object: a connection into the entity or the reference
	// that placed it, else the entity's own field.
	std::optional<PropertyValue> effective(const LogicObject &p_object, std::string_view p_field) const;
	std::optional<PropertyValue> effective(uint32_t p_scope, const EbxInstance &p_entity, std::string_view p_field) const;

	// The prefab or sub-world blueprint a reference instantiates (a sub-world reference names its
	// SubWorldData by bundle).
	std::optional<EbxInstance> referenced_blueprint(const EbxInstance &p_reference) const;

	// Resolver steps the queries have taken so far, and whether LogicWorldOptions::max_query_steps
	// ran out (later answers are then missing, so callers should report an error).
	size_t query_steps() const { return query_steps_; }
	bool query_budget_spent() const { return query_steps_ >= max_query_steps_; }

	// Resolver steps a single query may take before it gives up (bounds hostile graphs).
	static constexpr uint32_t kResolveDepth = 32;
	static constexpr size_t kQuerySteps = 200000;

private:
	struct Frame;
	LogicWorld() = default;

	uint32_t connection_set(const EbxInstance &p_blueprint) const;
	// property() that adds the resolver steps it took to r_steps (the walker's budget).
	std::optional<PropertyValue> property_counted(uint32_t p_scope, const EbxInstance &p_target, int32_t p_field, size_t &r_steps) const;
	std::optional<PropertyValue> property_in(const Frame &p_frame, const EbxInstance &p_target, int32_t p_field, uint32_t p_depth, size_t &r_steps) const;
	std::optional<PropertyValue> source_value(const Frame &p_frame, const EbxInstance &p_source, int32_t p_field, uint32_t p_depth, size_t &r_steps) const;
	std::optional<PropertyValue> interface_in(const Frame &p_frame, int32_t p_field, uint32_t p_depth, size_t &r_steps) const;

	const EbxResolver *db_ = nullptr;
	std::vector<LogicScope> scopes_;
	std::vector<LogicObject> objects_;
	LogicWorldStats stats_;
	size_t max_query_steps_ = 0;
	mutable size_t query_steps_ = 0;
	mutable std::deque<LogicConnections> sets_; // deque: references stay valid while prefabs are added
	mutable std::unordered_map<InstanceKey, uint32_t, InstanceKeyHash> set_by_blueprint_;

	friend class LogicWalker;
};

// The data field of p_object whose name hashes to p_field.
std::optional<EbxValue> data_field(const EbxInstance &p_object, int32_t p_field);

// Whether a SubWorldReferenceObjectData loads in p_mode: it must auto-load, and every GameMode
// inclusion setting must enable the mode. A reference without settings is always included; a
// setting whose criterion does not resolve counts as a GameMode one, the only criterion levels use.
// An empty p_mode accepts every auto-loaded sub-world.
bool subworld_included(const EbxResolver &p_db, const EbxInstance &p_reference, std::string_view p_mode);
// The same, adding one step per inclusion setting and per enabled option scanned to r_steps.
bool subworld_included(const EbxResolver &p_db, const EbxInstance &p_reference, std::string_view p_mode, size_t &r_steps);

} // namespace bf3::core::gameplay
