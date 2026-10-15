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
// Hostile data: one WorkBudget (work_budget.h) bounds the total work of the walk and of every query
// made on the world afterwards; scope count, object count, nesting depth, walk steps and resolver
// steps per query have their own caps on top. A level that exceeds any of them fails with an error
// instead of growing without limit. The cost model is written down in logic_world.cpp.

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/ids.h"
#include "bf3/core/result.h"

#include <array>
#include <deque>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
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

// Keyed with the process seed (ids.h), like every hash of data-controlled keys.
struct InstanceKeyHash {
	size_t operator()(const InstanceKey &p_key) const noexcept {
		return static_cast<size_t>(hash_keyed(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p_key.partition)), p_key.index));
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

	// Charges every entry of the three arrays (resolved or not) to r_budget before scanning it.
	static Result<LogicConnections> of(const EbxResolver &p_db, const EbxInstance &p_blueprint, WorkBudget &r_budget);

	// Indices into `property` of the connections into (target, field), in connection order (as
	// the IW4L resolver tries them). No allocation: a view into an index built by of().
	std::span<const uint32_t> property_into(const EbxInstance &p_target, int32_t p_field) const;

private:
	// Indices into `property` ordered by (target, field), within one key in connection order: a
	// stable sort, O(n log n) whatever the keys. The field ids come from the data, so a hash index
	// here could be flooded (crafted ids that cancel the target's hash bits).
	std::vector<uint32_t> by_target_;
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
	// Walk steps (a cap on top of the total budget, counted in LogicWorldStats::walk_steps): one per
	// container visited and per Objects entry examined (null and unresolvable entries included), one
	// per inclusion setting and enabled option of every sub-world reference reached, plus the
	// resolver steps of every Excluded query. World parts push no scope or object, so without a
	// bound a fan-out of nested world parts would run k^max_depth visits.
	size_t max_walk_steps = 2000000; // retail levels need at most ~7k (MP_Subway Rush)
	// Resolver steps all queries may take together, the walk's own Excluded queries included (each
	// query is also capped at kQuerySteps). The cap is checked before each query, so the total can
	// overshoot it by up to kQuerySteps. Once spent, queries return nullopt and
	// query_budget_spent() is true; a walk that spends it fails with an error.
	size_t max_query_steps = 2000000; // a retail mode read needs < 4k (fields scanned included)
	// The total work budget of the walk and of every later query on the world: walk steps, query
	// steps, connection sets built, sub-world inclusion tests, names copied. When `budget` is set
	// the world charges that one (the caller keeps it alive while it uses the world, e.g. to share
	// one budget between the walk and a reader around it); else the world owns one of max_work units.
	size_t max_work = WorkBudget::kDefaultLimit;
	WorkBudget *budget = nullptr;
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
	// The work budget the walk charged and later queries charge (shared with the caller's when
	// LogicWorldOptions::budget was set). Exhausted means some answer may be missing: report its error.
	WorkBudget &budget() const { return *budget_; }
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
	// SubWorldData by bundle; that name copy is charged to the budget, nullopt once it is spent).
	std::optional<EbxInstance> referenced_blueprint(const EbxInstance &p_reference) const;

	// Resolver steps the queries have taken so far, and whether LogicWorldOptions::max_query_steps
	// or the work budget ran out (later answers are then missing, so callers should report an error).
	size_t query_steps() const { return query_steps_; }
	bool query_budget_spent() const { return query_steps_ >= max_query_steps_ || budget_->exhausted(); }

	// Resolver steps a single query may take before it gives up (bounds hostile graphs).
	static constexpr uint32_t kResolveDepth = 32;
	static constexpr size_t kQuerySteps = 200000;

private:
	struct Frame;
	LogicWorld() = default;

	// The connection set of a blueprint, built and charged on first use; nullopt once the budget is spent.
	std::optional<uint32_t> connection_set(const EbxInstance &p_blueprint) const;
	// property() that adds the resolver steps it took to r_steps (the walker's step count).
	std::optional<PropertyValue> property_counted(uint32_t p_scope, const EbxInstance &p_target, int32_t p_field, size_t &r_steps) const;
	// One resolver step of a query, charged to the budget: false once the query has taken
	// kQuerySteps or the budget is spent.
	bool query_step(size_t &r_steps) const;
	// Whether a query loop must stop: its next step would fail.
	bool query_spent(size_t p_steps) const { return p_steps >= kQuerySteps || budget_->exhausted(); }
	std::optional<PropertyValue> property_in(const Frame &p_frame, const EbxInstance &p_target, int32_t p_field, uint32_t p_depth, size_t &r_steps) const;
	std::optional<PropertyValue> source_value(const Frame &p_frame, const EbxInstance &p_source, int32_t p_field, uint32_t p_depth, size_t &r_steps) const;
	std::optional<PropertyValue> interface_in(const Frame &p_frame, int32_t p_field, uint32_t p_depth, size_t &r_steps) const;

	const EbxResolver *db_ = nullptr;
	std::unique_ptr<WorkBudget> own_budget_; // when LogicWorldOptions::budget is null
	WorkBudget *budget_ = nullptr; // never null after build()
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
// An empty p_mode accepts every auto-loaded sub-world. Every Settings entry (null and unresolvable
// ones included) and every EnabledOptions entry is charged to r_budget before it is scanned (and each
// instance examined one unit). r_scanned, when set, gets the Settings and EnabledOptions entries
// added (the walk counts them as walk steps).
Result<bool> subworld_included(const EbxResolver &p_db, const EbxInstance &p_reference, std::string_view p_mode, WorkBudget &r_budget,
		size_t *r_scanned = nullptr);

} // namespace bf3::core::gameplay
