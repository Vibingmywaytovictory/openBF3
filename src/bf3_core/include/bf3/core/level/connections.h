#pragma once

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/level/budget.h"
#include "bf3/core/level/level_transform.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bf3::core::level {

int32_t field_id(std::string_view p_name);

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
	size_t unresolved = 0;

	static Result<Connections> of(const EbxResolver &p_db, const EbxInstance &p_blueprint, WorkBudget &r_budget);

	const std::vector<uint32_t> *property_into(const EbxInstance &p_target, int32_t p_field) const;

private:
	struct FieldKey {
		InstanceKey target;
		int32_t field = 0;
		bool operator==(const FieldKey &) const = default;
	};
	struct FieldKeyHash {
		size_t operator()(const FieldKey &p_key) const noexcept { return InstanceKeyHash()(p_key.target) ^ (size_t(uint32_t(p_key.field)) << 7); }
	};

	std::unordered_map<FieldKey, std::vector<uint32_t>, FieldKeyHash> property_index_;
};

struct Property {
	enum class Kind : uint8_t { Bool, Int, Float, Text, Team, Transform, Vec3, Constant, Field };
	Kind kind = Kind::Constant;
	bool b = false;
	int64_t i = 0;
	float f = 0.0f;
	std::string_view text;
	uint32_t team = 0;
	LevelTransform transform;
	level::Vec3 vec{};
	EbxValue field;

	static std::optional<Property> parse(std::string_view p_constant);

	std::optional<bool> as_bool() const;
	std::optional<int64_t> as_i64() const;
	std::optional<float> as_f32() const;

	std::optional<std::string_view> as_str() const;
	std::optional<uint32_t> as_team() const;
	std::optional<LevelTransform> as_transform() const;
	std::optional<level::Vec3> as_vec3() const;
};

std::optional<EbxValue> data_field(const EbxInstance &p_object, int32_t p_field);

struct Scope {
	EbxInstance blueprint;
	std::optional<EbxInstance> reference;
	std::optional<uint32_t> parent;
	LevelTransform transform;
	uint32_t subworld = 0;
	uint32_t variation = 0;
	uint32_t connections = 0;
};

class Scopes {
public:
	Scopes(const EbxResolver &p_db, WorkBudget &r_budget) :
			db_(&p_db), budget_(&r_budget) {}

	Result<uint32_t> push(const EbxInstance &p_blueprint, std::optional<EbxInstance> p_reference, std::optional<uint32_t> p_parent,
			const LevelTransform &p_transform, uint32_t p_subworld, uint32_t p_variation);

	const EbxResolver &db() const { return *db_; }
	size_t size() const { return scopes_.size(); }
	bool empty() const { return scopes_.empty(); }
	const Scope &get(uint32_t p_scope) const { return scopes_[p_scope]; }
	const std::vector<Scope> &all() const { return scopes_; }
	const Connections &connections(uint32_t p_scope) const { return connections_[scopes_[p_scope].connections]; }

	std::optional<uint32_t> child(uint32_t p_scope, const EbxInstance &p_reference) const;

	Result<std::optional<EbxInstance>> referenced_blueprint(const EbxInstance &p_reference) const;

	Result<std::optional<Property>> property(uint32_t p_scope, const EbxInstance &p_target, int32_t p_field) const;

	Result<std::optional<Property>> interface(uint32_t p_scope, int32_t p_field) const;

	size_t capped_resolutions() const { return capped_; }

	size_t hops() const { return hops_; }

private:
	struct Frame;

	bool hop() const;

	const Connections *connections_of(const EbxInstance &p_blueprint) const;

	std::optional<EbxValue> descriptor_field(const EbxInstance &p_descriptor, int32_t p_field) const;
	std::optional<Property> property_in(const Frame &p_frame, const EbxInstance &p_target, int32_t p_field, uint32_t p_depth) const;
	std::optional<Property> source_value(const Frame &p_frame, const EbxInstance &p_source, int32_t p_field, uint32_t p_depth) const;
	std::optional<Property> interface_in(const Frame &p_frame, int32_t p_field, uint32_t p_depth) const;

	const EbxResolver *db_;
	WorkBudget *budget_;
	std::vector<Scope> scopes_;
	std::vector<Connections> connections_;
	std::unordered_map<InstanceKey, uint32_t, InstanceKeyHash> by_blueprint_;

	mutable std::unordered_map<InstanceKey, Connections, InstanceKeyHash> extra_;

	mutable std::unordered_map<InstanceKey, std::unordered_map<int32_t, uint32_t>, InstanceKeyHash> descriptor_fields_;
	struct ChildKey {
		uint32_t scope;
		InstanceKey reference;
		bool operator==(const ChildKey &) const = default;
	};
	struct ChildKeyHash {
		size_t operator()(const ChildKey &p_key) const noexcept { return InstanceKeyHash()(p_key.reference) ^ (size_t(p_key.scope) << 1); }
	};
	std::unordered_map<ChildKey, uint32_t, ChildKeyHash> children_;

	mutable uint32_t resolve_left_ = 0;
	mutable size_t capped_ = 0;
	mutable size_t hops_ = 0;
};

}
