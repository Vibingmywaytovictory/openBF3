// Connections and the property resolver: port of IW4L level_bf3 connections.rs (Apache-2.0).
#include "bf3/core/level/connections.h"

#include <array>
#include <charconv>
#include <string>

namespace bf3::core::level {

namespace {

// Resolution depth: each hop through a connection or an interface costs one.
constexpr uint32_t kResolveDepth = 32;
// Hops one resolution may take (see Scopes::resolve_left_); retail resolutions take a few dozen.
// The traversal's WorkBudget bounds the sum over all resolutions.
constexpr uint32_t kResolveCap = 1u << 16;
// Longest sub-world BundleName looked up (retail names are under 100 characters).
constexpr size_t kMaxBundleName = 1024;

std::string_view trim(std::string_view p_text) {
	const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
	while (!p_text.empty() && space(p_text.front())) {
		p_text.remove_prefix(1);
	}
	while (!p_text.empty() && space(p_text.back())) {
		p_text.remove_suffix(1);
	}
	return p_text;
}

bool iequals(std::string_view p_a, std::string_view p_b) {
	return iequals_ascii(p_a, p_b);
}

// Rust's str::parse accepts one leading '+'; from_chars does not.
std::string_view strip_plus(std::string_view p_text) {
	if (p_text.size() > 1 && p_text[0] == '+' && p_text[1] != '-' && p_text[1] != '+') {
		p_text.remove_prefix(1);
	}
	return p_text;
}

std::optional<int64_t> parse_i64(std::string_view p_text) {
	p_text = strip_plus(p_text);
	int64_t v = 0;
	const auto r = std::from_chars(p_text.data(), p_text.data() + p_text.size(), v);
	if (r.ec != std::errc() || r.ptr != p_text.data() + p_text.size() || p_text.empty()) {
		return std::nullopt;
	}
	return v;
}

std::optional<uint32_t> parse_u32(std::string_view p_text) {
	p_text = strip_plus(p_text);
	uint32_t v = 0;
	const auto r = std::from_chars(p_text.data(), p_text.data() + p_text.size(), v);
	if (r.ec != std::errc() || r.ptr != p_text.data() + p_text.size() || p_text.empty()) {
		return std::nullopt;
	}
	return v;
}

std::optional<float> parse_f32(std::string_view p_text) {
	p_text = strip_plus(p_text);
	float v = 0.0f;
	const auto r = std::from_chars(p_text.data(), p_text.data() + p_text.size(), v, std::chars_format::general);
	if (r.ec != std::errc() || r.ptr != p_text.data() + p_text.size() || p_text.empty()) {
		return std::nullopt;
	}
	return v;
}

// Exactly p_count numbers separated by ',', '(', ')' or whitespace.
bool parse_floats(std::string_view p_text, float *r_out, size_t p_count) {
	const auto separator = [](char c) { return c == ',' || c == '(' || c == ')' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
	size_t found = 0;
	size_t at = 0;
	while (at < p_text.size()) {
		if (separator(p_text[at])) {
			++at;
			continue;
		}
		size_t end = at;
		while (end < p_text.size() && !separator(p_text[end])) {
			++end;
		}
		if (found == p_count) {
			return false; // one too many
		}
		const std::optional<float> v = parse_f32(p_text.substr(at, end - at));
		if (!v) {
			return false;
		}
		r_out[found++] = *v;
		at = end;
	}
	return found == p_count;
}

bool same(const EbxInstance &p_a, const EbxInstance &p_b) {
	return p_a == p_b;
}

} // namespace

int32_t field_id(std::string_view p_name) {
	return int32_t(ebx_name_hash(p_name));
}

// --- Connections -------------------------------------------------------------------------------

Result<Connections> Connections::of(const EbxResolver &p_db, const EbxInstance &p_blueprint, WorkBudget &r_budget) {
	Connections out;
	const EbxPartition &partition = p_blueprint.partition();
	struct List {
		std::string_view name, source_field, target_field;
	};
	static constexpr List kLists[3] = {
		{ "PropertyConnections", "SourceFieldId", "TargetFieldId" },
		{ "LinkConnections", "SourceFieldId", "TargetFieldId" },
		{ "EventConnections", "SourceEvent.Id", "TargetEvent.Id" },
	};
	for (const List &list : kLists) {
		BF3_TRY(r_budget.charge(array_length(p_blueprint.data(), list.name)));
	}
	for (size_t slot = 0; slot < 3; ++slot) {
		const std::optional<EbxValue> list = p_blueprint.get(kLists[slot].name);
		const OptionalEbxArray array = list ? list->as_array() : OptionalEbxArray();
		if (!array) {
			continue;
		}
		for (const EbxValue entry : *array) {
			const auto end = [&](std::string_view p_field) -> std::optional<EbxInstance> {
				const std::optional<EbxValue> v = entry.get(p_field);
				const std::optional<EbxRef> r = v ? v->as_ref() : std::nullopt;
				return r ? p_db.resolve(partition, *r) : std::nullopt;
			};
			const auto id = [&](std::string_view p_path) -> int32_t {
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
			const Connection c{ *source, id(kLists[slot].source_field), *target, id(kLists[slot].target_field) };
			if (slot == 0) {
				out.property_index_[FieldKey{ InstanceKey::of(c.target), c.target_field }].push_back(uint32_t(out.property.size()));
			}
			(slot == 0 ? out.property : slot == 1 ? out.link : out.event).push_back(c);
		}
	}
	return out;
}

const std::vector<uint32_t> *Connections::property_into(const EbxInstance &p_target, int32_t p_field) const {
	const auto found = property_index_.find(FieldKey{ InstanceKey::of(p_target), p_field });
	return found == property_index_.end() ? nullptr : &found->second;
}

// --- Property ----------------------------------------------------------------------------------

std::optional<Property> Property::parse(std::string_view p_constant) {
	const std::string_view constant = trim(p_constant);
	if (constant.empty()) {
		return std::nullopt;
	}
	Property p;
	p.kind = Kind::Constant;
	p.text = constant;
	const size_t space = constant.find(' ');
	if (space == std::string_view::npos) {
		return p;
	}
	const std::string_view kind = constant.substr(0, space);
	const std::string_view rest = trim(constant.substr(space + 1));
	Property parsed;
	bool ok = false;
	if (kind == "Boolean") {
		if (iequals(rest, "true") || iequals(rest, "false")) {
			parsed.kind = Kind::Bool;
			parsed.b = iequals(rest, "true");
			ok = true;
		}
	} else if (kind == "Int8" || kind == "Int16" || kind == "Int32" || kind == "Int64" || kind == "Uint8" || kind == "Uint16" || kind == "Uint32" ||
			kind == "Uint64" || kind == "UInt8" || kind == "UInt16" || kind == "UInt32" || kind == "UInt64") {
		if (const std::optional<int64_t> v = parse_i64(rest)) {
			parsed.kind = Kind::Int;
			parsed.i = *v;
			ok = true;
		}
	} else if (kind == "Float32" || kind == "Float64") {
		if (const std::optional<float> v = parse_f32(rest)) {
			parsed.kind = Kind::Float;
			parsed.f = *v;
			ok = true;
		}
	} else if (kind == "CString") {
		parsed.kind = Kind::Text;
		parsed.text = rest;
		if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"') {
			parsed.text = rest.substr(1, rest.size() - 2);
		}
		ok = true;
	} else if (kind == "TeamId") {
		std::string_view team = rest;
		if (team.substr(0, 4) == "Team") {
			team.remove_prefix(4);
		}
		if (iequals(team, "Neutral")) {
			parsed.kind = Kind::Team;
			parsed.team = 0;
			ok = true;
		} else if (const std::optional<uint32_t> v = parse_u32(team)) {
			parsed.kind = Kind::Team;
			parsed.team = *v;
			ok = true;
		}
	} else if (kind == "LinearTransform") {
		float f[12];
		if (parse_floats(rest, f, 12)) {
			parsed.kind = Kind::Transform;
			parsed.transform = LevelTransform::from_rows({ { { f[0], f[1], f[2] }, { f[3], f[4], f[5] }, { f[6], f[7], f[8] }, { f[9], f[10], f[11] } } });
			ok = true;
		}
	} else if (kind == "Vec3") {
		float f[3];
		if (parse_floats(rest, f, 3)) {
			parsed.kind = Kind::Vec3;
			parsed.vec = { f[0], f[1], f[2] };
			ok = true;
		}
	}
	return ok ? parsed : p;
}

std::optional<bool> Property::as_bool() const {
	switch (kind) {
		case Kind::Bool:
			return b;
		case Kind::Field:
			return field.as_bool();
		default:
			return std::nullopt;
	}
}

std::optional<int64_t> Property::as_i64() const {
	switch (kind) {
		case Kind::Int:
			return i;
		case Kind::Team:
			return int64_t(team);
		case Kind::Field:
			return field.as_i64();
		default:
			return std::nullopt;
	}
}

std::optional<float> Property::as_f32() const {
	switch (kind) {
		case Kind::Float:
			return f;
		case Kind::Int:
			return float(i);
		case Kind::Field: {
			if (const std::optional<float> v = field.as_f32()) {
				return v;
			}
			const std::optional<int64_t> v = field.as_i64();
			return v ? std::optional<float>(float(*v)) : std::nullopt;
		}
		default:
			return std::nullopt;
	}
}

std::optional<std::string_view> Property::as_str() const {
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

std::optional<uint32_t> Property::as_team() const {
	switch (kind) {
		case Kind::Team:
			return team;
		case Kind::Int:
			return (i >= 0 && i <= int64_t(UINT32_MAX)) ? std::optional<uint32_t>(uint32_t(i)) : std::nullopt;
		case Kind::Field:
			return field.as_u32();
		default:
			return std::nullopt;
	}
}

std::optional<LevelTransform> Property::as_transform() const {
	switch (kind) {
		case Kind::Transform:
			return transform;
		case Kind::Field: {
			const auto rows = field.as_transform();
			return rows ? std::optional<LevelTransform>(LevelTransform::from_rows(*rows)) : std::nullopt;
		}
		default:
			return std::nullopt;
	}
}

std::optional<level::Vec3> Property::as_vec3() const {
	switch (kind) {
		case Kind::Vec3:
			return vec;
		case Kind::Transform:
			return transform.trans;
		case Kind::Field:
			return field.as_vec3();
		default:
			return std::nullopt;
	}
}

std::optional<EbxValue> data_field(const EbxInstance &p_object, int32_t p_field) {
	// The first field of that name hash in EbxStruct::fields() order (base types first), compared by
	// the descriptors' stored hash: the parser resolved each field's name through that hash, so it is
	// ebx_name_hash of the name, and no (possibly huge, hostile) name is hashed again. One search in
	// the type's lookup table (EbxPartition::lookup, O(log n) whatever the layout). fields() skips
	// fields that do not decode, but an instance's fields all decode: the parser validated each.
	const EbxPartition &p = p_object.partition();
	const EbxStruct data = p_object.data();
	const EbxPartition::Lookup *found = p.lookup(data.complex(), uint32_t(p_field));
	if (found == nullptr) {
		return std::nullopt;
	}
	return p.decode(found->first_field, size_t(data.offset()) + found->first_offset);
}

// --- Scopes ------------------------------------------------------------------------------------

// A scope, or a child prefab seen through one of its references while resolving (not a scope of
// its own); lives on the resolver's stack.
struct Scopes::Frame {
	std::optional<uint32_t> scope;
	const Frame *parent = nullptr;
	EbxInstance reference;
	EbxInstance blueprint;
};

Result<uint32_t> Scopes::push(const EbxInstance &p_blueprint, std::optional<EbxInstance> p_reference, std::optional<uint32_t> p_parent,
		const LevelTransform &p_transform, uint32_t p_subworld, uint32_t p_variation) {
	const InstanceKey key = InstanceKey::of(p_blueprint);
	uint32_t connections = 0;
	const auto found = by_blueprint_.find(key);
	if (found != by_blueprint_.end()) {
		connections = found->second;
	} else {
		// Each blueprint's lists are read once per traversal, however many scopes instantiate it.
		Result<Connections> read = Connections::of(*db_, p_blueprint, *budget_);
		if (!read) {
			return read.error();
		}
		connections = uint32_t(connections_.size());
		connections_.push_back(std::move(*read));
		by_blueprint_.emplace(key, connections);
	}
	const uint32_t at = uint32_t(scopes_.size());
	if (p_parent && p_reference) {
		children_[ChildKey{ *p_parent, InstanceKey::of(*p_reference) }] = at;
	}
	Scope s;
	s.blueprint = p_blueprint;
	s.reference = p_reference;
	s.parent = p_parent;
	s.transform = p_transform;
	s.subworld = p_subworld;
	s.variation = p_variation;
	s.connections = connections;
	scopes_.push_back(s);
	return at;
}

std::optional<uint32_t> Scopes::child(uint32_t p_scope, const EbxInstance &p_reference) const {
	const auto found = children_.find(ChildKey{ p_scope, InstanceKey::of(p_reference) });
	return found == children_.end() ? std::nullopt : std::optional<uint32_t>(found->second);
}

Result<std::optional<EbxInstance>> Scopes::referenced_blueprint(const EbxInstance &p_reference) const {
	if (p_reference.is_a("SubWorldReferenceObjectData")) {
		// Charged before it is copied and looked up: a resolution may meet this reference on every
		// hop, and many references may name one long string.
		const Result<std::optional<std::string_view>> charged = charged_str(p_reference.data(), "BundleName", *budget_);
		if (!charged) {
			return charged.error();
		}
		const std::optional<std::string_view> bundle = *charged;
		if (!bundle) {
			return std::optional<EbxInstance>();
		}
		if (bundle->size() > kMaxBundleName) {
			return std::optional<EbxInstance>(); // no partition has such a name
		}
		std::string name(*bundle);
		for (char &c : name) {
			if (c == '\\') {
				c = '/';
			}
		}
		const EbxPartition *partition = db_->partition_by_name(name);
		return partition ? std::optional<EbxInstance>(partition->primary()) : std::nullopt;
	}
	if (p_reference.is_a("ReferenceObjectData")) {
		return db_->follow(p_reference.data(), "Blueprint");
	}
	return std::optional<EbxInstance>();
}

Result<std::optional<Property>> Scopes::property(uint32_t p_scope, const EbxInstance &p_target, int32_t p_field) const {
	BF3_TRY(budget_->status());
	resolve_left_ = kResolveCap;
	Frame frame;
	frame.scope = p_scope;
	std::optional<Property> v = property_in(frame, p_target, p_field, kResolveDepth);
	BF3_TRY(budget_->status()); // a resolution the budget cut short has no answer
	return v;
}

Result<std::optional<Property>> Scopes::interface(uint32_t p_scope, int32_t p_field) const {
	BF3_TRY(budget_->status());
	resolve_left_ = kResolveCap;
	Frame frame;
	frame.scope = p_scope;
	std::optional<Property> v = interface_in(frame, p_field, kResolveDepth);
	BF3_TRY(budget_->status());
	return v;
}

bool Scopes::hop() const {
	if (resolve_left_ == 0) {
		++capped_;
		return false;
	}
	if (!budget_->charge(1)) {
		return false;
	}
	--resolve_left_;
	++hops_;
	return true;
}

const Connections *Scopes::connections_of(const EbxInstance &p_blueprint) const {
	const InstanceKey key = InstanceKey::of(p_blueprint);
	const auto found = by_blueprint_.find(key);
	if (found != by_blueprint_.end()) {
		return &connections_[found->second];
	}
	auto extra = extra_.find(key);
	if (extra == extra_.end()) {
		Result<Connections> read = Connections::of(*db_, p_blueprint, *budget_);
		if (!read) {
			return nullptr; // the budget is exhausted; the resolution reports it
		}
		extra = extra_.emplace(key, std::move(*read)).first;
	}
	return &extra->second;
}

std::optional<EbxValue> Scopes::descriptor_field(const EbxInstance &p_descriptor, int32_t p_field) const {
	const std::optional<EbxValue> fields = p_descriptor.get("Fields");
	const OptionalEbxArray array = fields ? fields->as_array() : OptionalEbxArray();
	if (!array) {
		return std::nullopt;
	}
	const InstanceKey key = InstanceKey::of(p_descriptor);
	auto index = descriptor_fields_.find(key);
	if (index == descriptor_fields_.end()) {
		if (!budget_->charge(array->size())) {
			return std::nullopt; // the resolution reports the exhausted budget
		}
		index = descriptor_fields_.emplace(key, std::unordered_map<int32_t, uint32_t>()).first;
		uint32_t at = 0;
		for (const EbxValue entry : *array) {
			const std::optional<EbxValue> id = entry.get("Id");
			if (const std::optional<int32_t> i = id ? id->as_i32() : std::nullopt) {
				index->second.emplace(*i, at); // the first entry with the id decides, as Rust's find
			}
			++at;
		}
	}
	const auto found = index->second.find(p_field);
	return found == index->second.end() ? std::nullopt : array->get(found->second);
}

std::optional<Property> Scopes::property_in(const Frame &p_frame, const EbxInstance &p_target, int32_t p_field, uint32_t p_depth) const {
	if (p_depth == 0) {
		return std::nullopt;
	}
	const uint32_t depth = p_depth - 1;
	const EbxInstance blueprint = p_frame.scope ? scopes_[*p_frame.scope].blueprint : p_frame.blueprint;
	const Connections *connections = connections_of(blueprint);
	if (!connections) {
		return std::nullopt;
	}
	if (const std::vector<uint32_t> *into = connections->property_into(p_target, p_field)) {
		for (const uint32_t at : *into) {
			if (!hop()) {
				break; // capped or out of budget: the rest would each be cut short
			}
			const Connection &c = connections->property[at];
			if (std::optional<Property> v = source_value(p_frame, c.source, c.source_field, depth)) {
				return v;
			}
		}
	}
	return std::nullopt;
}

std::optional<Property> Scopes::source_value(const Frame &p_frame, const EbxInstance &p_source, int32_t p_field, uint32_t p_depth) const {
	// The caller charged this hop.
	if (p_depth == 0) {
		return std::nullopt;
	}
	const uint32_t depth = p_depth - 1;
	const EbxInstance blueprint = p_frame.scope ? scopes_[*p_frame.scope].blueprint : p_frame.blueprint;
	const std::optional<EbxInstance> descriptor = db_->follow(blueprint.data(), "Descriptor");
	if (descriptor && same(*descriptor, p_source)) {
		return interface_in(p_frame, p_field, depth);
	}
	const Result<std::optional<EbxInstance>> referenced = referenced_blueprint(p_source);
	if (!referenced) {
		return std::nullopt; // the budget is exhausted; the resolution reports it
	}
	if (const std::optional<EbxInstance> inner = *referenced) {
		if (const std::optional<EbxInstance> inner_descriptor = db_->follow(inner->data(), "Descriptor")) {
			Frame child;
			child.parent = &p_frame;
			child.reference = p_source;
			child.blueprint = *inner;
			const Connections *connections = connections_of(*inner);
			if (!connections) {
				return std::nullopt;
			}
			if (const std::vector<uint32_t> *into = connections->property_into(*inner_descriptor, p_field)) {
				for (const uint32_t at : *into) {
					if (!hop()) {
						break;
					}
					const Connection &c = connections->property[at];
					if (std::optional<Property> v = source_value(child, c.source, c.source_field, depth)) {
						return v;
					}
				}
			}
		}
	}
	if (std::optional<Property> v = property_in(p_frame, p_source, p_field, depth)) {
		return v;
	}
	if (const std::optional<EbxValue> v = data_field(p_source, p_field)) {
		Property p;
		p.kind = Property::Kind::Field;
		p.field = *v;
		return p;
	}
	return std::nullopt;
}

std::optional<Property> Scopes::interface_in(const Frame &p_frame, int32_t p_field, uint32_t p_depth) const {
	std::optional<EbxInstance> reference;
	std::optional<Frame> parent;
	if (p_frame.scope) {
		const Scope &s = scopes_[*p_frame.scope];
		reference = s.reference;
		if (s.parent) {
			Frame f;
			f.scope = *s.parent;
			parent = f;
		}
	} else {
		reference = p_frame.reference;
		if (p_frame.parent) {
			parent = *p_frame.parent;
		}
	}
	if (reference && parent) {
		if (std::optional<Property> v = property_in(*parent, *reference, p_field, p_depth)) {
			return v;
		}
	}
	const EbxInstance blueprint = p_frame.scope ? scopes_[*p_frame.scope].blueprint : p_frame.blueprint;
	const std::optional<EbxInstance> descriptor = db_->follow(blueprint.data(), "Descriptor");
	if (!descriptor) {
		return std::nullopt;
	}
	const std::optional<EbxValue> entry = descriptor_field(*descriptor, p_field);
	if (!entry) {
		return std::nullopt;
	}
	// The first entry with the id decides, as Rust's find + and_then.
	// Parsing reads the whole constant: charged its text units first.
	const Result<std::optional<std::string_view>> text = charged_str(entry->get("Value"), *budget_);
	if (!text || !*text) {
		return std::nullopt; // a failed charge: the resolution reports the exhausted budget
	}
	return Property::parse(**text);
}

} // namespace bf3::core::level
