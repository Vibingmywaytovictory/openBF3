#pragma once

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
		return static_cast<size_t>(hash_keyed(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p_key.partition)), p_key.index));
	}
};

struct PropertyValue {
	enum class Kind : uint8_t {
		Bool,
		Int,
		Float,
		Text,
		Team,
		Vec3,
		Transform,
		Constant,
		Field,
		Ref,
	};

	Kind kind = Kind::Constant;
	bool b = false;
	int64_t i = 0;
	double f = 0;
	std::string_view text;
	std::array<std::array<float, 3>, 4> rows{};
	EbxValue field;
	std::optional<EbxInstance> ref;

	static std::optional<PropertyValue> parse(std::string_view p_constant);
	static PropertyValue of_field(const EbxValue &p_value);

	std::optional<bool> as_bool() const;
	std::optional<int64_t> as_i64() const;

	std::optional<double> as_f64() const;
	std::optional<std::string_view> as_str() const;

	std::optional<int> as_team() const;
	std::optional<std::array<float, 3>> as_vec3() const;
	std::optional<std::string_view> enum_name() const;

	std::optional<EbxRef> as_ref() const;
};

struct LogicConnection {
	EbxInstance source;
	int32_t source_field = 0;
	EbxInstance target;
	int32_t target_field = 0;
};

struct LogicConnections {
	std::vector<LogicConnection> property;
	std::vector<LogicConnection> link;
	std::vector<LogicConnection> event;
	size_t unresolved = 0;

	static Result<LogicConnections> of(const EbxResolver &p_db, const EbxInstance &p_blueprint, WorkBudget &r_budget);

	std::span<const uint32_t> property_into(const EbxInstance &p_target, int32_t p_field) const;

private:

	std::vector<uint32_t> by_target_;
};

struct LogicScope {
	EbxInstance blueprint;
	std::optional<EbxInstance> reference;
	std::optional<uint32_t> parent;
	uint32_t depth = 0;
	uint32_t connections = 0;
};

struct LogicObject {
	EbxInstance entity;
	std::optional<EbxInstance> reference;
	uint32_t scope = 0;
};

struct LogicWorldOptions {

	std::string mode;

	bool expand_root_objects = false;
	uint32_t max_depth = 48;
	size_t max_scopes = 200000;
	size_t max_objects = 4000000;

	size_t max_walk_steps = 2000000;

	size_t max_query_steps = 2000000;

	size_t max_work = WorkBudget::kDefaultLimit;
	WorkBudget *budget = nullptr;
};

struct LogicWorldStats {
	size_t excluded = 0;
	size_t unresolved_objects = 0;
	size_t missing_blueprints = 0;
	size_t depth_limited = 0;
	size_t cycles = 0;
	size_t walk_steps = 0;

	static constexpr size_t kMaxListedSubworlds = 4096;
	std::vector<std::string> subworlds;
	std::vector<std::string> skipped_subworlds;
	std::vector<std::string> absent_subworlds;
};

class LogicWorld {
public:

	static Result<LogicWorld> build(const EbxResolver &p_db, const EbxInstance &p_root, const LogicWorldOptions &p_options);

	LogicWorld(LogicWorld &&) noexcept = default;
	LogicWorld &operator=(LogicWorld &&) noexcept = default;

	const EbxResolver &db() const { return *db_; }
	const std::vector<LogicScope> &scopes() const { return scopes_; }
	const std::vector<LogicObject> &objects() const { return objects_; }
	const LogicWorldStats &stats() const { return stats_; }
	const LogicConnections &connections(uint32_t p_scope) const { return sets_[scopes_[p_scope].connections]; }

	WorkBudget &budget() const { return *budget_; }

	std::string_view scope_name(uint32_t p_scope) const;

	std::optional<PropertyValue> property(uint32_t p_scope, const EbxInstance &p_target, int32_t p_field) const;

	std::optional<PropertyValue> interface_value(uint32_t p_scope, int32_t p_field) const;

	std::optional<PropertyValue> effective(const LogicObject &p_object, std::string_view p_field) const;
	std::optional<PropertyValue> effective(uint32_t p_scope, const EbxInstance &p_entity, std::string_view p_field) const;

	std::optional<EbxInstance> referenced_blueprint(const EbxInstance &p_reference) const;

	size_t query_steps() const { return query_steps_; }
	bool query_budget_spent() const { return query_steps_ >= max_query_steps_ || budget_->exhausted(); }

	static constexpr uint32_t kResolveDepth = 32;
	static constexpr size_t kQuerySteps = 200000;

private:
	struct Frame;
	LogicWorld() = default;

	std::optional<uint32_t> connection_set(const EbxInstance &p_blueprint) const;

	std::optional<PropertyValue> property_counted(uint32_t p_scope, const EbxInstance &p_target, int32_t p_field, size_t &r_steps) const;

	bool query_step(size_t &r_steps) const;

	bool query_spent(size_t p_steps) const { return p_steps >= kQuerySteps || budget_->exhausted(); }
	std::optional<PropertyValue> property_in(const Frame &p_frame, const EbxInstance &p_target, int32_t p_field, uint32_t p_depth, size_t &r_steps) const;
	std::optional<PropertyValue> source_value(const Frame &p_frame, const EbxInstance &p_source, int32_t p_field, uint32_t p_depth, size_t &r_steps) const;
	std::optional<PropertyValue> interface_in(const Frame &p_frame, int32_t p_field, uint32_t p_depth, size_t &r_steps) const;

	const EbxResolver *db_ = nullptr;
	std::unique_ptr<WorkBudget> own_budget_;
	WorkBudget *budget_ = nullptr;
	std::vector<LogicScope> scopes_;
	std::vector<LogicObject> objects_;
	LogicWorldStats stats_;
	size_t max_query_steps_ = 0;
	mutable size_t query_steps_ = 0;
	mutable std::deque<LogicConnections> sets_;
	mutable std::unordered_map<InstanceKey, uint32_t, InstanceKeyHash> set_by_blueprint_;

	friend class LogicWalker;
};

std::optional<EbxValue> data_field(const EbxInstance &p_object, int32_t p_field);

Result<bool> subworld_included(const EbxResolver &p_db, const EbxInstance &p_reference, std::string_view p_mode, WorkBudget &r_budget,
		size_t *r_scanned = nullptr);

}
