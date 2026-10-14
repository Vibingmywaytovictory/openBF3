#pragma once
// Blueprint connections and the property resolver of a level traversal. Ported from IW4L level_bf3
// (connections.rs), Apache-2.0.
//
// A scope is one instance of a blueprint in the placed level: the level itself, a sub-world, or a
// prefab some reference instantiated. Connections are stored on the blueprint; world parts (layers)
// are transparent, so objects of a layer belong to the scope holding the layer reference.
// A PropertyConnection feeds a field of an object from an interface constant
// ('LinearTransform ((r),(u),(f),(t))', 'Boolean True', ...) or from a data field of the source;
// the resolver follows the scope's interface into the parent scopes and the output ports of child
// prefabs (handoff sections/03 §2.1: Metro's TICKET_HALL flagpole moves 18.4 m this way).
// Field ids are ebx_name_hash of the field name, as i32.

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/level/level_transform.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bf3::core::level {

int32_t field_id(std::string_view p_name);

// Identity of an instance inside one database: its partition and index.
struct InstanceKey {
	const EbxPartition *partition = nullptr;
	uint32_t index = 0;

	static InstanceKey of(const EbxInstance &p_instance) { return { &p_instance.partition(), p_instance.index() }; }
	bool operator==(const InstanceKey &) const = default;
};

struct InstanceKeyHash {
	size_t operator()(const InstanceKey &p_key) const noexcept {
		return std::hash<const void *>()(p_key.partition) ^ (size_t(p_key.index) * 0x9E3779B97F4A7C15ull);
	}
};

// One PropertyConnection, LinkConnection or EventConnection with both ends resolved. For events the
// field ids are the event ids.
struct Connection {
	EbxInstance source;
	int32_t source_field = 0;
	EbxInstance target;
	int32_t target_field = 0;
};

struct Connections {
	std::vector<Connection> property;
	std::vector<Connection> link;
	std::vector<Connection> event;
	size_t unresolved = 0; // connections with an end that does not resolve in the loaded partitions

	static Connections of(const EbxResolver &p_db, const EbxInstance &p_blueprint);
};

// A value fed into a field through a PropertyConnection: an interface constant or the data field
// of the connected source object.
struct Property {
	enum class Kind : uint8_t { Bool, Int, Float, Text, Team, Transform, Vec3, Constant, Field };
	Kind kind = Kind::Constant;
	bool b = false;
	int64_t i = 0;
	float f = 0.0f;
	std::string_view text; // Text (a CString without its quotes) or Constant (as written)
	uint32_t team = 0; // `TeamId TeamN` as N; TeamNeutral is 0
	LevelTransform transform;
	level::Vec3 vec{};
	EbxValue field;

	// An interface DataField.Value; empty means "no constant".
	static std::optional<Property> parse(std::string_view p_constant);

	std::optional<bool> as_bool() const;
	std::optional<int64_t> as_i64() const;
	std::optional<float> as_f32() const;
	std::optional<std::string_view> as_str() const;
	std::optional<uint32_t> as_team() const;
	std::optional<LevelTransform> as_transform() const;
	std::optional<level::Vec3> as_vec3() const;
};

// The data field of p_object whose name hashes to p_field (base types first, first match).
std::optional<EbxValue> data_field(const EbxInstance &p_object, int32_t p_field);

struct Scope {
	EbxInstance blueprint;
	std::optional<EbxInstance> reference; // the reference that instantiated it; none for the level
	std::optional<uint32_t> parent;
	LevelTransform transform; // where the blueprint's own frame sits in the world
	uint32_t subworld = 0;
	uint32_t variation = 0; // the ObjectVariation name hash the instance passes to its objects
	uint32_t connections = 0;
};

// Every scope of a traversal, and the property resolver over them. The const resolvers keep
// mutable state (the step budget, the child-prefab connection cache), so a Scopes must not be used
// from several threads at once.
class Scopes {
public:
	explicit Scopes(const EbxResolver &p_db) :
			db_(&p_db) {}

	uint32_t push(const EbxInstance &p_blueprint, std::optional<EbxInstance> p_reference, std::optional<uint32_t> p_parent,
			const LevelTransform &p_transform, uint32_t p_subworld, uint32_t p_variation);

	const EbxResolver &db() const { return *db_; }
	size_t size() const { return scopes_.size(); }
	bool empty() const { return scopes_.empty(); }
	const Scope &get(uint32_t p_scope) const { return scopes_[p_scope]; }
	const std::vector<Scope> &all() const { return scopes_; }
	const Connections &connections(uint32_t p_scope) const { return connections_[scopes_[p_scope].connections]; }
	// The scope that p_reference, an object of p_scope, instantiated.
	std::optional<uint32_t> child(uint32_t p_scope, const EbxInstance &p_reference) const;
	// The prefab or sub-world blueprint a reference instantiates (a sub-world reference names its
	// SubWorldData by bundle, not by pointer).
	std::optional<EbxInstance> referenced_blueprint(const EbxInstance &p_reference) const;
	// The value a PropertyConnection of p_scope feeds into p_field of p_target, an object of that
	// scope, resolved down to a constant or a data field.
	std::optional<Property> property(uint32_t p_scope, const EbxInstance &p_target, int32_t p_field) const;
	// Interface field p_field of the scope's blueprint for this instance: what the parent connects
	// into the instantiating reference, else the descriptor's constant.
	std::optional<Property> interface(uint32_t p_scope, int32_t p_field) const;
	// Resolutions cut short by the step budget (hostile connection graphs; 0 on retail levels).
	size_t budget_exhausted() const { return exhausted_; }

private:
	struct Frame;

	const Connections &connections_of(const EbxInstance &p_blueprint) const;
	std::vector<std::pair<EbxInstance, int32_t>> sources_into(const EbxInstance &p_blueprint, const EbxInstance &p_target, int32_t p_field) const;
	std::optional<Property> property_in(const Frame &p_frame, const EbxInstance &p_target, int32_t p_field, uint32_t p_depth) const;
	std::optional<Property> source_value(const Frame &p_frame, const EbxInstance &p_source, int32_t p_field, uint32_t p_depth) const;
	std::optional<Property> interface_in(const Frame &p_frame, int32_t p_field, uint32_t p_depth) const;

	const EbxResolver *db_;
	std::vector<Scope> scopes_;
	std::vector<Connections> connections_;
	std::unordered_map<InstanceKey, uint32_t, InstanceKeyHash> by_blueprint_;
	// Connections of blueprints no scope instantiates (child prefabs seen through output ports).
	mutable std::unordered_map<InstanceKey, Connections, InstanceKeyHash> extra_;
	struct ChildKey {
		uint32_t scope;
		InstanceKey reference;
		bool operator==(const ChildKey &) const = default;
	};
	struct ChildKeyHash {
		size_t operator()(const ChildKey &p_key) const noexcept { return InstanceKeyHash()(p_key.reference) ^ (size_t(p_key.scope) << 1); }
	};
	std::unordered_map<ChildKey, uint32_t, ChildKeyHash> children_;
	// Steps left in the current resolution: each connection hop costs one. The depth limit alone
	// bounds the chain length, not the fan-out, which a hostile blueprint can make exponential.
	mutable uint32_t budget_ = 0;
	mutable size_t exhausted_ = 0;
};

} // namespace bf3::core::level
