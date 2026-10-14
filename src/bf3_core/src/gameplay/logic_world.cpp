// Blueprint scopes and the property resolver. Ported from IW4L level_bf3 (connections.rs,
// world.rs, level.rs), Apache-2.0; see the header for what is and is not carried over.
#include "bf3/core/gameplay/logic_world.h"

#include "bf3/core/ids.h"

#include <algorithm>
#include <charconv>
#include <string>

namespace bf3::core::gameplay {

namespace {

std::string_view trim(std::string_view p_text) {
	const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
	while (!p_text.empty() && space(p_text.front())) {
		p_text.remove_prefix(1);
	}
	while (!p_text.empty() && space(p_text.back())) {
		p_text.remove_suffix(1);
	}
	return p_text;
}

std::optional<int64_t> parse_int(std::string_view p_text) {
	if (!p_text.empty() && p_text.front() == '+') {
		p_text.remove_prefix(1);
	}
	int64_t v = 0;
	const auto res = std::from_chars(p_text.data(), p_text.data() + p_text.size(), v);
	if (res.ec != std::errc() || res.ptr != p_text.data() + p_text.size()) {
		return std::nullopt;
	}
	return v;
}

std::optional<double> parse_float(std::string_view p_text) {
	if (!p_text.empty() && p_text.front() == '+') {
		p_text.remove_prefix(1);
	}
	double v = 0;
	const auto res = std::from_chars(p_text.data(), p_text.data() + p_text.size(), v);
	if (res.ec != std::errc() || res.ptr != p_text.data() + p_text.size()) {
		return std::nullopt;
	}
	return v;
}

// Exactly N floats separated by ',', '(', ')' or whitespace.
template <size_t N>
std::optional<std::array<float, N>> parse_floats(std::string_view p_text) {
	std::array<float, N> out{};
	size_t count = 0;
	size_t pos = 0;
	const auto separator = [](char c) { return c == ',' || c == '(' || c == ')' || c == ' ' || c == '\t'; };
	while (pos < p_text.size()) {
		while (pos < p_text.size() && separator(p_text[pos])) {
			++pos;
		}
		size_t end = pos;
		while (end < p_text.size() && !separator(p_text[end])) {
			++end;
		}
		if (end > pos) {
			if (count == N) {
				return std::nullopt;
			}
			const std::optional<double> v = parse_float(p_text.substr(pos, end - pos));
			if (!v) {
				return std::nullopt;
			}
			out[count++] = static_cast<float>(*v);
		}
		pos = end;
	}
	if (count != N) {
		return std::nullopt;
	}
	return out;
}

std::optional<int> team_from_name(std::string_view p_name) {
	if (p_name.size() >= 4 && iequals_ascii(p_name.substr(0, 4), "Team")) {
		p_name.remove_prefix(4);
	}
	if (iequals_ascii(p_name, "Neutral")) {
		return 0;
	}
	const std::optional<int64_t> v = parse_int(p_name);
	if (!v || *v < 0 || *v > 64) {
		return std::nullopt;
	}
	return static_cast<int>(*v);
}

bool is_kind(std::string_view p_kind, std::initializer_list<std::string_view> p_names) {
	for (const std::string_view name : p_names) {
		if (p_kind == name) {
			return true;
		}
	}
	return false;
}

} // namespace

// --- PropertyValue -------------------------------------------------------------------------------

std::optional<PropertyValue> PropertyValue::parse(std::string_view p_constant) {
	const std::string_view constant = trim(p_constant);
	if (constant.empty()) {
		return std::nullopt;
	}
	PropertyValue out;
	out.kind = Kind::Constant;
	out.text = constant;
	const size_t space = constant.find(' ');
	if (space == std::string_view::npos) {
		return out;
	}
	const std::string_view kind = constant.substr(0, space);
	const std::string_view rest = trim(constant.substr(space + 1));
	if (kind == "Boolean") {
		if (iequals_ascii(rest, "true") || iequals_ascii(rest, "false")) {
			out.kind = Kind::Bool;
			out.b = iequals_ascii(rest, "true");
		}
	} else if (is_kind(kind, { "Int8", "Int16", "Int32", "Int64", "Uint8", "Uint16", "Uint32", "Uint64", "UInt8", "UInt16", "UInt32", "UInt64" })) {
		if (const std::optional<int64_t> v = parse_int(rest)) {
			out.kind = Kind::Int;
			out.i = *v;
		}
	} else if (kind == "Float32" || kind == "Float64") {
		if (const std::optional<double> v = parse_float(rest)) {
			out.kind = Kind::Float;
			out.f = *v;
		}
	} else if (kind == "CString") {
		std::string_view text = rest;
		if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
			text = text.substr(1, text.size() - 2);
		}
		out.kind = Kind::Text;
		out.text = text;
	} else if (kind == "TeamId") {
		if (const std::optional<int> team = team_from_name(rest)) {
			out.kind = Kind::Team;
			out.i = *team;
		}
	} else if (kind == "LinearTransform") {
		if (const std::optional<std::array<float, 12>> f = parse_floats<12>(rest)) {
			out.kind = Kind::Transform;
			for (size_t r = 0; r < 4; ++r) {
				out.rows[r] = { (*f)[r * 3], (*f)[r * 3 + 1], (*f)[r * 3 + 2] };
			}
		}
	} else if (kind == "Vec3") {
		if (const std::optional<std::array<float, 3>> f = parse_floats<3>(rest)) {
			out.kind = Kind::Vec3;
			out.rows[0] = *f;
		}
	}
	return out;
}

PropertyValue PropertyValue::of_field(const EbxValue &p_value) {
	PropertyValue out;
	out.kind = Kind::Field;
	out.field = p_value;
	return out;
}

std::optional<bool> PropertyValue::as_bool() const {
	switch (kind) {
		case Kind::Bool:
			return b;
		case Kind::Field:
			return field.as_bool();
		default:
			return std::nullopt;
	}
}

std::optional<int64_t> PropertyValue::as_i64() const {
	switch (kind) {
		case Kind::Int:
		case Kind::Team:
			return i;
		case Kind::Field:
			return field.as_i64();
		default:
			return std::nullopt;
	}
}

std::optional<double> PropertyValue::as_f64() const {
	switch (kind) {
		case Kind::Float:
			return f;
		case Kind::Int:
			return static_cast<double>(i);
		case Kind::Field:
			if (const std::optional<double> v = field.as_f64()) {
				return v;
			}
			if (field.kind() != EbxValue::Kind::Enum) {
				if (const std::optional<int64_t> v = field.as_i64()) {
					return static_cast<double>(*v);
				}
				if (const std::optional<uint64_t> v = field.as_u64()) {
					return static_cast<double>(*v);
				}
			}
			return std::nullopt;
		default:
			return std::nullopt;
	}
}

std::optional<std::string_view> PropertyValue::as_str() const {
	switch (kind) {
		case Kind::Text:
		case Kind::Constant:
			return text;
		case Kind::Field:
			return field.as_str();
		default:
			return std::nullopt;
	}
}

std::optional<int> PropertyValue::as_team() const {
	switch (kind) {
		case Kind::Team:
			return static_cast<int>(i);
		case Kind::Int:
			return (i >= 0 && i <= 64) ? std::optional<int>(static_cast<int>(i)) : std::nullopt;
		case Kind::Field:
			if (const std::optional<std::string_view> name = field.enum_name()) {
				return team_from_name(*name);
			}
			if (const std::optional<int64_t> v = field.as_i64(); v && *v >= 0 && *v <= 64) {
				return static_cast<int>(*v);
			}
			return std::nullopt;
		default:
			return std::nullopt;
	}
}

std::optional<std::array<float, 3>> PropertyValue::as_vec3() const {
	switch (kind) {
		case Kind::Vec3:
			return rows[0];
		case Kind::Transform:
			return rows[3];
		case Kind::Field:
			return field.as_vec3();
		default:
			return std::nullopt;
	}
}

std::optional<std::string_view> PropertyValue::enum_name() const {
	return kind == Kind::Field ? field.enum_name() : std::nullopt;
}

std::optional<EbxRef> PropertyValue::as_ref() const {
	return kind == Kind::Field ? field.as_ref() : std::nullopt;
}

// --- LogicConnections ----------------------------------------------------------------------------

LogicConnections LogicConnections::of(const EbxResolver &p_db, const EbxInstance &p_blueprint) {
	LogicConnections out;
	struct List {
		const char *name;
		const char *source_field;
		const char *target_field;
		std::vector<LogicConnection> *into;
	};
	const List lists[] = {
		{ "PropertyConnections", "SourceFieldId", "TargetFieldId", &out.property },
		{ "LinkConnections", "SourceFieldId", "TargetFieldId", &out.link },
		{ "EventConnections", "SourceEvent.Id", "TargetEvent.Id", &out.event },
	};
	const EbxPartition &partition = p_blueprint.partition();
	for (const List &list : lists) {
		const std::optional<EbxValue> array_value = p_blueprint.get(list.name);
		const OptionalEbxArray array = array_value ? array_value->as_array() : OptionalEbxArray();
		if (!array) {
			continue;
		}
		list.into->reserve(array->size());
		for (const EbxValue entry : *array) {
			const auto end = [&](const char *p_field) -> std::optional<EbxInstance> {
				const std::optional<EbxValue> v = entry.get(p_field);
				const std::optional<EbxRef> r = v ? v->as_ref() : std::nullopt;
				return r ? p_db.resolve(partition, *r) : std::nullopt;
			};
			const auto id = [&](const char *p_path) -> int32_t {
				const std::optional<EbxValue> v = entry.get_path(p_path);
				const std::optional<int32_t> i = v ? v->as_i32() : std::nullopt;
				return i.value_or(0);
			};
			const std::optional<EbxInstance> source = end("Source");
			const std::optional<EbxInstance> target = end("Target");
			if (!source || !target) {
				++out.unresolved;
				continue;
			}
			list.into->push_back({ *source, id(list.source_field), *target, id(list.target_field) });
		}
	}
	out.by_target_.reserve(out.property.size());
	for (uint32_t i = 0; i < out.property.size(); ++i) {
		out.by_target_.emplace(TargetKey{ InstanceKey::of(out.property[i].target), out.property[i].target_field }, i);
	}
	return out;
}

std::vector<uint32_t> LogicConnections::property_into(const EbxInstance &p_target, int32_t p_field) const {
	std::vector<uint32_t> out;
	const auto range = by_target_.equal_range(TargetKey{ InstanceKey::of(p_target), p_field });
	for (auto it = range.first; it != range.second; ++it) {
		out.push_back(it->second);
	}
	// Connection order, as the IW4L resolver tries them.
	std::sort(out.begin(), out.end());
	return out;
}

// --- helpers ------------------------------------------------------------------------------------

std::optional<EbxValue> data_field(const EbxInstance &p_object, int32_t p_field) {
	for (const EbxField &f : p_object.fields()) {
		if (field_id(f.name) == p_field) {
			return f.value;
		}
	}
	return std::nullopt;
}

bool subworld_included(const EbxResolver &p_db, const EbxInstance &p_reference, std::string_view p_mode) {
	size_t steps = 0;
	return subworld_included(p_db, p_reference, p_mode, steps);
}

bool subworld_included(const EbxResolver &p_db, const EbxInstance &p_reference, std::string_view p_mode, size_t &r_steps) {
	const std::optional<EbxValue> auto_load = p_reference.get("AutoLoad");
	if (!auto_load || !auto_load->as_bool().value_or(false)) {
		return false;
	}
	if (p_mode.empty()) {
		return true;
	}
	const std::optional<EbxInstance> settings = p_db.follow(p_reference.data(), "InclusionSettings");
	if (!settings) {
		return true;
	}
	const std::vector<EbxInstance> list = p_db.follow_all(settings->data(), "Settings");
	r_steps += list.size();
	for (const EbxInstance &setting : list) {
		const std::optional<EbxInstance> criterion = p_db.follow(setting.data(), "Criterion");
		if (criterion) {
			const std::optional<EbxValue> name = criterion->get("Name");
			const std::optional<std::string_view> text = name ? name->as_str() : std::nullopt;
			if (text && !iequals_ascii(*text, "GameMode")) {
				continue;
			}
		}
		bool enabled = false;
		const std::optional<EbxValue> options = setting.get("EnabledOptions");
		if (const OptionalEbxArray array = options ? options->as_array() : OptionalEbxArray()) {
			r_steps += array->size();
			for (const EbxValue option : *array) {
				const std::optional<std::string_view> text = option.as_str();
				if (text && iequals_ascii(*text, p_mode)) {
					enabled = true;
					break;
				}
			}
		}
		if (!enabled) {
			return false;
		}
	}
	return true;
}

// --- walker --------------------------------------------------------------------------------------

class LogicWalker {
public:
	LogicWalker(LogicWorld &r_world, const LogicWorldOptions &p_options) :
			world_(r_world), options_(p_options) {}

	// Walks p_container's Objects. Every container on the current path is on path_, so a blueprint
	// that (directly or through others) contains itself is skipped instead of being re-entered.
	Status visit(const EbxInstance &p_container, uint32_t p_scope, uint32_t p_depth) {
		if (p_depth > options_.max_depth) {
			++world_.stats_.depth_limited;
			return Status::success();
		}
		BF3_TRY(step(1));
		path_.push_back(InstanceKey::of(p_container));
		const Status status = visit_objects(p_container, p_scope, p_depth);
		path_.pop_back();
		return status;
	}

	Result<uint32_t> push(const EbxInstance &p_blueprint, std::optional<EbxInstance> p_reference, std::optional<uint32_t> p_parent) {
		if (world_.scopes_.size() >= options_.max_scopes) {
			return make_error("logic world: more than " + std::to_string(options_.max_scopes) + " scopes");
		}
		LogicScope scope;
		scope.blueprint = p_blueprint;
		scope.reference = p_reference;
		scope.parent = p_parent;
		scope.depth = p_parent ? world_.scopes_[*p_parent].depth + 1 : 0;
		scope.connections = world_.connection_set(p_blueprint);
		world_.scopes_.push_back(scope);
		return static_cast<uint32_t>(world_.scopes_.size() - 1);
	}

private:
	Status step(size_t p_count) {
		world_.stats_.walk_steps += p_count;
		if (world_.stats_.walk_steps > options_.max_walk_steps) {
			return make_error("logic world: walk exceeds " + std::to_string(options_.max_walk_steps) + " steps");
		}
		return Status::success();
	}

	// Records a sub-world bundle name in a stats list. A list keeps at most kMaxListedSubworlds
	// names (later ones are dropped), so a reference reached through a fan-out cannot grow it
	// without bound.
	static void note(std::vector<std::string> &r_list, std::string_view p_name) {
		if (r_list.size() < LogicWorldStats::kMaxListedSubworlds) {
			r_list.emplace_back(p_name);
		}
	}

	// Whether p_blueprint is a container on the current walk path (a reference cycle).
	bool on_path(const EbxInstance &p_blueprint) {
		const InstanceKey key = InstanceKey::of(p_blueprint);
		if (std::find(path_.begin(), path_.end(), key) == path_.end()) {
			return false;
		}
		++world_.stats_.cycles;
		return true;
	}

	Status visit_objects(const EbxInstance &p_container, uint32_t p_scope, uint32_t p_depth) {
		const bool root = p_scope == 0;
		Result<std::vector<EbxInstance>> list = objects(p_container);
		if (!list) {
			return list.error();
		}
		for (const EbxInstance &object : *list) {
			if (object.is_a("WorldPartReferenceObjectData")) {
				Result<bool> skip = excluded(p_scope, object);
				if (!skip) {
					return skip.error();
				}
				if (*skip) {
					continue;
				}
				const std::optional<EbxInstance> part = world_.db().follow(object.data(), "Blueprint");
				if (!part) {
					++world_.stats_.missing_blueprints;
					continue;
				}
				if (on_path(*part)) {
					continue;
				}
				BF3_TRY(visit(*part, p_scope, p_depth + 1));
			} else if (object.is_a("SubWorldReferenceObjectData")) {
				BF3_TRY(enter_subworld(object, p_scope, p_depth));
			} else if (object.is_a("ReferenceObjectData")) {
				if (root && !options_.expand_root_objects) {
					continue;
				}
				BF3_TRY(place_reference(object, p_scope, p_depth));
			} else {
				if (root && !options_.expand_root_objects) {
					continue;
				}
				BF3_TRY(emit(object, std::nullopt, p_scope));
			}
		}
		return Status::success();
	}

	// The resolved Objects of a container. Every array entry is charged to the walk budget before
	// any is examined, null and unresolvable ones included: each costs a resolve, and a part reached
	// through a fan-out re-scans its whole array on every visit.
	Result<std::vector<EbxInstance>> objects(const EbxInstance &p_container) {
		std::vector<EbxInstance> out;
		const std::optional<EbxValue> value = p_container.get("Objects");
		const OptionalEbxArray array = value ? value->as_array() : OptionalEbxArray();
		if (!array) {
			return out;
		}
		BF3_TRY(step(array->size()));
		out.reserve(array->size());
		for (const EbxValue element : *array) {
			const std::optional<EbxRef> r = element.as_ref();
			if (!r || r->is_null()) {
				continue;
			}
			if (const std::optional<EbxInstance> object = world_.db().resolve(p_container.partition(), *r)) {
				out.push_back(*object);
			} else {
				++world_.stats_.unresolved_objects;
			}
		}
		return out;
	}

	Status enter_subworld(const EbxInstance &p_reference, uint32_t p_scope, uint32_t p_depth) {
		const std::optional<EbxValue> bundle = p_reference.get("BundleName");
		const std::string_view name = bundle ? bundle->as_str().value_or(std::string_view()) : std::string_view();
		size_t steps = 0;
		const bool included = subworld_included(world_.db(), p_reference, options_.mode, steps);
		BF3_TRY(step(steps));
		if (!included) {
			note(world_.stats_.skipped_subworlds, name);
			return Status::success();
		}
		Result<bool> skip = excluded(p_scope, p_reference);
		if (!skip) {
			return skip.error();
		}
		if (*skip) {
			return Status::success();
		}
		const std::optional<EbxInstance> data = world_.referenced_blueprint(p_reference);
		if (!data) {
			note(world_.stats_.absent_subworlds, name);
			return Status::success();
		}
		if (on_path(*data)) {
			return Status::success();
		}
		note(world_.stats_.subworlds, name);
		Result<uint32_t> child = push(*data, p_reference, p_scope);
		if (!child) {
			return child.error();
		}
		return visit(*data, *child, p_depth + 1);
	}

	Status place_reference(const EbxInstance &p_reference, uint32_t p_scope, uint32_t p_depth) {
		Result<bool> skip = excluded(p_scope, p_reference);
		if (!skip) {
			return skip.error();
		}
		if (*skip) {
			return Status::success();
		}
		if (p_reference.is_a("SpawnReferenceObjectData")) {
			return emit(p_reference, std::nullopt, p_scope);
		}
		const std::optional<EbxInstance> blueprint = world_.db().follow(p_reference.data(), "Blueprint");
		if (!blueprint) {
			++world_.stats_.missing_blueprints;
			return emit(p_reference, std::nullopt, p_scope);
		}
		if (blueprint->is_a("PrefabBlueprint")) {
			if (on_path(*blueprint)) {
				return Status::success();
			}
			Result<uint32_t> child = push(*blueprint, p_reference, p_scope);
			if (!child) {
				return child.error();
			}
			return visit(*blueprint, *child, p_depth + 1);
		}
		if (blueprint->is_a("ObjectBlueprint")) {
			if (const std::optional<EbxInstance> object = world_.db().follow(blueprint->data(), "Object")) {
				return emit(*object, p_reference, p_scope);
			}
		}
		return emit(p_reference, std::nullopt, p_scope);
	}

	Status emit(const EbxInstance &p_entity, std::optional<EbxInstance> p_reference, uint32_t p_scope) {
		if (world_.objects_.size() >= options_.max_objects) {
			return make_error("logic world: more than " + std::to_string(options_.max_objects) + " objects");
		}
		world_.objects_.push_back({ p_entity, p_reference, p_scope });
		return Status::success();
	}

	// Whether a reference is Excluded (connected value, else its own field); the query's resolver
	// steps count against the walk budget.
	Result<bool> excluded(uint32_t p_scope, const EbxInstance &p_reference) {
		bool value = false;
		size_t steps = 0;
		const std::optional<PropertyValue> v = world_.property_counted(p_scope, p_reference, field_id("Excluded"), steps);
		BF3_TRY(step(steps));
		// With the query budget spent the connected value is unknown; the stored field below could
		// then be wrong, so the walk fails instead.
		if (world_.query_budget_spent()) {
			return make_error("logic world: query budget of " + std::to_string(world_.max_query_steps_) + " steps spent during the walk");
		}
		if (v && v->as_bool()) {
			value = *v->as_bool();
		} else if (const std::optional<EbxValue> stored = p_reference.get("Excluded")) {
			value = stored->as_bool().value_or(false);
		}
		if (value) {
			++world_.stats_.excluded;
		}
		return value;
	}

	LogicWorld &world_;
	const LogicWorldOptions &options_;
	std::vector<InstanceKey> path_; // containers being walked, outermost first (at most max_depth + 1)
};

// --- LogicWorld ----------------------------------------------------------------------------------

Result<LogicWorld> LogicWorld::build(const EbxResolver &p_db, const EbxInstance &p_root, const LogicWorldOptions &p_options) {
	LogicWorld world;
	world.db_ = &p_db;
	world.max_query_steps_ = p_options.max_query_steps;
	LogicWalker walker(world, p_options);
	Result<uint32_t> root = walker.push(p_root, std::nullopt, std::nullopt);
	if (!root) {
		return root.error();
	}
	BF3_TRY(walker.visit(p_root, *root, 0));
	return world;
}

uint32_t LogicWorld::connection_set(const EbxInstance &p_blueprint) const {
	const InstanceKey key = InstanceKey::of(p_blueprint);
	const auto it = set_by_blueprint_.find(key);
	if (it != set_by_blueprint_.end()) {
		return it->second;
	}
	const uint32_t at = static_cast<uint32_t>(sets_.size());
	sets_.push_back(LogicConnections::of(*db_, p_blueprint));
	set_by_blueprint_.emplace(key, at);
	return at;
}

std::string_view LogicWorld::scope_name(uint32_t p_scope) const {
	if (p_scope >= scopes_.size()) {
		return {};
	}
	return db_->name_of(scopes_[p_scope].blueprint.partition().guid()).value_or(std::string_view());
}

std::optional<EbxInstance> LogicWorld::referenced_blueprint(const EbxInstance &p_reference) const {
	if (p_reference.is_a("SubWorldReferenceObjectData")) {
		const std::optional<EbxValue> bundle = p_reference.get("BundleName");
		const std::optional<std::string_view> text = bundle ? bundle->as_str() : std::nullopt;
		if (!text || text->empty()) {
			return std::nullopt;
		}
		std::string name = to_lower_ascii(*text);
		std::replace(name.begin(), name.end(), '\\', '/');
		const EbxPartition *partition = db_->partition_by_name(name);
		return partition ? std::optional<EbxInstance>(partition->primary()) : std::nullopt;
	}
	if (p_reference.is_a("ReferenceObjectData")) {
		return db_->follow(p_reference.data(), "Blueprint");
	}
	return std::nullopt;
}

// A scope of the world, or a prefab passed through while following an output port.
struct LogicWorld::Frame {
	const Frame *parent_frame = nullptr; // child frames only
	uint32_t scope = 0; // scope frames only
	bool child = false;
	EbxInstance reference; // child frames only
	EbxInstance blueprint; // child frames only

	static Frame of_scope(uint32_t p_scope) {
		Frame f;
		f.scope = p_scope;
		return f;
	}
};

std::optional<PropertyValue> LogicWorld::property(uint32_t p_scope, const EbxInstance &p_target, int32_t p_field) const {
	size_t steps = 0;
	return property_counted(p_scope, p_target, p_field, steps);
}

std::optional<PropertyValue> LogicWorld::property_counted(uint32_t p_scope, const EbxInstance &p_target, int32_t p_field, size_t &r_steps) const {
	if (p_scope >= scopes_.size() || query_budget_spent()) {
		return std::nullopt;
	}
	size_t steps = 0;
	std::optional<PropertyValue> out = property_in(Frame::of_scope(p_scope), p_target, p_field, kResolveDepth, steps);
	r_steps += steps;
	query_steps_ += steps;
	return out;
}

std::optional<PropertyValue> LogicWorld::interface_value(uint32_t p_scope, int32_t p_field) const {
	if (p_scope >= scopes_.size() || query_budget_spent()) {
		return std::nullopt;
	}
	size_t steps = 0;
	std::optional<PropertyValue> out = interface_in(Frame::of_scope(p_scope), p_field, kResolveDepth, steps);
	query_steps_ += steps;
	return out;
}

std::optional<PropertyValue> LogicWorld::effective(uint32_t p_scope, const EbxInstance &p_entity, std::string_view p_field) const {
	if (std::optional<PropertyValue> v = property(p_scope, p_entity, field_id(p_field))) {
		return v;
	}
	const std::optional<EbxValue> stored = p_entity.get(p_field);
	return stored ? std::optional<PropertyValue>(PropertyValue::of_field(*stored)) : std::nullopt;
}

std::optional<PropertyValue> LogicWorld::effective(const LogicObject &p_object, std::string_view p_field) const {
	const int32_t id = field_id(p_field);
	if (std::optional<PropertyValue> v = property(p_object.scope, p_object.entity, id)) {
		return v;
	}
	if (p_object.reference) {
		if (std::optional<PropertyValue> v = property(p_object.scope, *p_object.reference, id)) {
			return v;
		}
	}
	const std::optional<EbxValue> stored = p_object.entity.get(p_field);
	return stored ? std::optional<PropertyValue>(PropertyValue::of_field(*stored)) : std::nullopt;
}

std::optional<PropertyValue> LogicWorld::property_in(const Frame &p_frame, const EbxInstance &p_target, int32_t p_field, uint32_t p_depth, size_t &r_steps) const {
	if (p_depth == 0 || ++r_steps > kQuerySteps) {
		return std::nullopt;
	}
	const uint32_t depth = p_depth - 1;
	const EbxInstance blueprint = p_frame.child ? p_frame.blueprint : scopes_[p_frame.scope].blueprint;
	const LogicConnections &set = sets_[connection_set(blueprint)];
	for (const uint32_t at : set.property_into(p_target, p_field)) {
		// Copy: source_value may add connection sets (a deque keeps `set` valid, the copy is cheap).
		const LogicConnection connection = set.property[at];
		if (std::optional<PropertyValue> v = source_value(p_frame, connection.source, connection.source_field, depth, r_steps)) {
			return v;
		}
	}
	return std::nullopt;
}

std::optional<PropertyValue> LogicWorld::source_value(const Frame &p_frame, const EbxInstance &p_source, int32_t p_field, uint32_t p_depth, size_t &r_steps) const {
	if (p_depth == 0 || ++r_steps > kQuerySteps) {
		return std::nullopt;
	}
	const uint32_t depth = p_depth - 1;
	const EbxInstance blueprint = p_frame.child ? p_frame.blueprint : scopes_[p_frame.scope].blueprint;
	if (const std::optional<EbxInstance> descriptor = db_->follow(blueprint.data(), "Descriptor"); descriptor && *descriptor == p_source) {
		return interface_in(p_frame, p_field, depth, r_steps);
	}
	// A child prefab's or sub-world's output port: continue inside it from its interface.
	{
		const std::optional<EbxInstance> inner = referenced_blueprint(p_source);
		const std::optional<EbxInstance> descriptor = inner ? db_->follow(inner->data(), "Descriptor") : std::nullopt;
		if (descriptor) {
			Frame child;
			child.parent_frame = &p_frame;
			child.child = true;
			child.reference = p_source;
			child.blueprint = *inner;
			const LogicConnections &set = sets_[connection_set(*inner)];
			for (const uint32_t at : set.property_into(*descriptor, p_field)) {
				const LogicConnection connection = set.property[at];
				if (std::optional<PropertyValue> v = source_value(child, connection.source, connection.source_field, depth, r_steps)) {
					return v;
				}
			}
		}
	}
	if (std::optional<PropertyValue> v = property_in(p_frame, p_source, p_field, depth, r_steps)) {
		return v;
	}
	const std::optional<EbxValue> field = data_field(p_source, p_field);
	return field ? std::optional<PropertyValue>(PropertyValue::of_field(*field)) : std::nullopt;
}

std::optional<PropertyValue> LogicWorld::interface_in(const Frame &p_frame, int32_t p_field, uint32_t p_depth, size_t &r_steps) const {
	if (p_depth == 0 || ++r_steps > kQuerySteps) {
		return std::nullopt;
	}
	// What the parent connects into the instantiating reference.
	if (p_frame.child) {
		if (std::optional<PropertyValue> v = property_in(*p_frame.parent_frame, p_frame.reference, p_field, p_depth, r_steps)) {
			return v;
		}
	} else {
		const LogicScope &scope = scopes_[p_frame.scope];
		if (scope.reference && scope.parent) {
			if (std::optional<PropertyValue> v = property_in(Frame::of_scope(*scope.parent), *scope.reference, p_field, p_depth, r_steps)) {
				return v;
			}
		}
	}
	// Else the descriptor's constant.
	const EbxInstance blueprint = p_frame.child ? p_frame.blueprint : scopes_[p_frame.scope].blueprint;
	const std::optional<EbxInstance> descriptor = db_->follow(blueprint.data(), "Descriptor");
	if (!descriptor) {
		return std::nullopt;
	}
	const std::optional<EbxValue> fields = descriptor->get("Fields");
	const OptionalEbxArray array = fields ? fields->as_array() : OptionalEbxArray();
	if (!array) {
		return std::nullopt;
	}
	for (const EbxValue entry : *array) {
		// Each field scanned is a step, so a huge Fields array cannot multiply the query cost.
		if (++r_steps > kQuerySteps) {
			return std::nullopt;
		}
		const std::optional<EbxValue> id = entry.get("Id");
		if (!id || id->as_i32() != p_field) {
			continue;
		}
		const std::optional<EbxValue> value = entry.get("Value");
		if (const std::optional<std::string_view> text = value ? value->as_str() : std::nullopt) {
			if (std::optional<PropertyValue> parsed = PropertyValue::parse(*text)) {
				return parsed;
			}
		}
		const std::optional<EbxValue> value_ref = entry.get("ValueRef");
		const std::optional<EbxRef> r = value_ref ? value_ref->as_ref() : std::nullopt;
		if (r && !r->is_null()) {
			if (const std::optional<EbxInstance> target = db_->resolve(descriptor->partition(), *r)) {
				PropertyValue out;
				out.kind = PropertyValue::Kind::Ref;
				out.ref = *target;
				return out;
			}
		}
		return std::nullopt;
	}
	return std::nullopt;
}

} // namespace bf3::core::gameplay
