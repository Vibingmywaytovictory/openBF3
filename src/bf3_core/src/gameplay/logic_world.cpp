// Blueprint scopes and the property resolver. Ported from IW4L level_bf3 (connections.rs,
// world.rs, level.rs), Apache-2.0; see the header for what is and is not carried over.
//
// --- Cost model (hostile input) ------------------------------------------------------------------
// Every public entry point of the gameplay readers runs on ONE WorkBudget (work_budget.h): the
// caller's, or one of WorkBudget::kDefaultLimit units it makes for the call. The entry points are
// LogicWorld::build and every later query on that world, subworld_included, read_mode_def (with
// level_modes, its team, size and fuse helpers), read_scoring, read_unlock_progression,
// read_kit_def, read_team_data, read_soldier_def and read_vehicle_def. Work is charged BEFORE it is
// done, where it is done; once the budget is exhausted every charge fails at once, every loop below
// stops at its next charge, and every entry point returns the budget error (it checks status()
// after the walk and again before it returns, so a helper that returned early with an empty value
// never yields a result). Retail reads use at most ~24k units of the default 1M (README).
//
// Unit costs. An access to a struct (a field lookup, a dotted path through its inline ValueType
// fields such as "SourceEvent.Id" or vec3() of one, a fields() pass) scans at most
// EbxPartition::lookup_width descriptors: the type chain (<= 255 fields x kBaseDepth (32)), the
// enumerators its enum fields decode against (<= 255 each), and the widest inline field type's own
// width, kLookupDepth levels down (inline types nest, and may be as wide as any class). The parser
// computes it once per type, linear in the descriptors. The access rule: every instance a reader
// or loop looks into is charged its width (WorkBudget::charge_type, take_type, follow_all) right
// before it is looked into, every struct array is charged raw size x element width (array_cost,
// array_of) before its scan, and a struct reached through an inline field of either is covered by
// that charge (its own lookups are at most kLookupDepth - 1 levels further down; every reader path
// has at most 2 segments). So each unit pays for at most 64 x (the few lookups one examination
// makes) descriptors, whatever the types: retail types cost nothing or a unit extra, a wide type's
// lookups cost its width. An is_a reads <= kBaseDepth descriptors. data_field and field_name_of
// match the descriptors' stored name hashes and never hash a name. Decoding a string is a binary
// search over the partition's NUL offsets (EbxPartition::string), never a scan of the string. A
// reference resolve is one hash lookup (a lazy EbxLibrary parses a partition at most once in its
// lifetime, so parsing is bounded by the install, not by the walk). A string copied or lowercased
// costs one unit per WorkBudget::kBytesPerUnit bytes, charged before the copy; string compares stop
// at the shorter side, and every compare below has a short side of fixed size (a type or field
// name literal, the mode id) unless it is charged.
//
// Indexes. A per-entry charge only bounds the work if each insert and lookup is cheap whatever the
// keys, and many keys come from the data (field ids, GUIDs). So no index keyed by data is a hash
// with a fixed function: LogicConnections' by-target index is a sorted vector and
// UnlockProgression::gates a std::map (O(log n) worst case); the InstanceKey hash sets and maps
// (connection sets per blueprint, events_from / fuse_after / gate_open in read_fuse, the hub frontier
// in unlock_targets) and the core GUID / SHA1 maps the resolver probes (EbxDb, EbxLibrary,
// LevelMount, CasCatalog) hash with the per-process seed of ids.h (hash_keyed), so crafted keys
// cannot be lined up in one bucket. read_mode_def's crate_at is keyed by scope indices the walker
// assigns (0, 1, 2, ...), not by data. Name-keyed core maps (EbxDb / EbxLibrary by name) are filled
// when the install is mounted, never from the contents of a partition the walk reads; the walk only
// probes them.
//
// Each loop or recursion reachable from those entry points, and what bounds it ("raw size" is the
// element count of an EBX array, null and unresolvable entries included, charged before the scan;
// arrays and strings can be shared by any number of instances, so no scan is assumed to be paid by
// the input size alone):
// logic_world.cpp
//   LogicWalker::visit            recursion; 1 unit plus the container's type width per visit;
//                                 depth <= max_depth
//   LogicWalker::objects          raw Objects size (the walk's emit() calls are one per charged entry),
//                                 then each resolved object's type width before its lookups
//   LogicWalker::on_path          std::find over path_ (<= max_depth + 1 entries) per charged entry
//   subworld_included             the settings' type width and raw Settings size, then per setting
//                                 its (and its criterion's) type width and raw EnabledOptions size;
//                                 each option compare stops at |mode| bytes
//   LogicWalker::note             bundle name copy charged by length; lists capped at 4096 names
//   referenced_blueprint          bundle name lowercase copy charged by length
//   LogicConnections::of          the blueprint's type width, then raw size x entry width (array_cost,
//                                 the inline SourceEvent / TargetEvent types included) of each
//                                 connection array; the by-target index is a stable
//                                 sort of the charged entries (n log n compares), looked up by binary
//                                 search, so crafted field ids cannot collide it into O(n^2)
//   LogicWorld::connection_set    memoised per blueprint: of() runs once per blueprint, a lookup is
//                                 one hash probe made after a charged step
//   property_in, source_value,    recursion; 1 unit per call (query_step), depth <= kResolveDepth,
//   interface_in                  <= kQuerySteps per query; each loop over property_into() checks
//                                 query_spent() first, and interface_in charges each Fields entry
//                                 (its width too); source_value charges the blueprint, the source and
//                                 a child prefab's blueprint before their lookups
//   LogicWorld::effective         the property query, then the entity's type width before the stored
//                                 lookup (any number of queries may ask one entity)
//   LogicWalker::place_reference  the referenced blueprint's type width before its lookups (any
//                                 number of references may name one blueprint)
//   PropertyValue::parse          charged by the constant's length before it is parsed
//   data_field                    one pass over the source's type chain (its width charged by the
//                                 query step that asks)
// mode_def.cpp
//   level_modes                   raw Categories size, raw Mode size per category, names by length
//   read_mode_def                 the level's type width; per sub-world scope the blueprint's and the
//                                 descriptor's type width and the Fields scan
//                                 (raw size x entry width, array_cost; at most 65536 entries in all),
//                                 1 unit per object of the world and per crate interaction, names
//                                 copied by length, every effective()/interface_value() a charged query
//   location_sid                  <= 64 interface_value() queries (charged) up the scope chain
//   read_team_data (kit_def.cpp)  once per TeamEntityData object: raw size of both customization lists
//   read_fuse                     per crate scope: the blueprint's and descriptor's type width, raw
//                                 event list size (index; hash keyed by the process seed), raw property
//                                 list size (feeds), each fed SyncedBool's and each gate's type width,
//                                 and each source's event list size before it is scanned
//   read_size                     instance count of settings_win32, raw Information / Sizes / Teams sizes
//   read_scoring                  instance count, 1 unit per ScoringTypeData, names by length
// kit_def.cpp
//   read_unlock_progression       instance count of each table, raw UnlockInfos / Unlocks sizes; one
//                                 O(log n) gates insert per charged entry
//   read_kit_def / read_table     the kit's and each table's type width, raw UnlockParts size, raw
//                                 SelectableUnlocks size per part (a part listed K times is charged K
//                                 times; follow_all charges each resolved part and unlock its width),
//                                 names by length
// soldier_def.cpp
//   collect_components            recursion, depth <= 16, at most 4096 components; raw Components
//                                 size charged per visit
//   read_soldier_def              the blueprint, entity, health, physics, sprint, collision and each
//                                 unlock's asset and user data: type width before their lookups
//   curves, poses, states         raw size x element width of every array read; each pose, state and
//                                 pose info its type width (follow_all), its inline structs included
//   first_of / unlock loop        over the (<= 4096) collected components, charged once per pass
//   unlock_targets                <= 4 hops; raw property connection count charged per hop; hub
//                                 frontier lookups are hash probes; each target name copy charged
//   specialization pairs          raw UnlockValues size; per pair, 1 unit plus the compared bytes
//                                 per target
// vehicle_def.cpp
//   Layout::walk                  recursion, depth <= 32, at most 8192 components; raw Components
//                                 size charged per visit
//   seat / turret association     child tables built in one pass, then O(depth <= 33) per component
//   seat sort, per-node readers   1 unit per node, names by length; each node's type width was
//                                 charged by the walk (follow_all), the blueprint's, entity's, config's,
//                                 body's and camera's before their lookups
// ebx.cpp (core, reached through every lookup above)
//   EbxPartition::string          binary search over the NUL offsets found once at parse time
//   EbxPartition::enumerator      <= 255 enumerators per enum decode (counted in lookup_width)
//   compute_lookup_widths         at parse time: kLookupDepth folds over the complexes, each complex
//                                 once per fold (memoised), plus one pass over the field descriptors
#include "bf3/core/gameplay/logic_world.h"

#include "bf3/core/ids.h"
#include "gameplay_read.h"

#include <algorithm>
#include <charconv>
#include <string>

namespace bf3::core::gameplay {

namespace {

// The order of LogicConnections' by-target index: (partition address, instance index, field).
struct TargetOrder {
	uintptr_t partition = 0;
	uint32_t index = 0;
	int32_t field = 0;
	auto operator<=>(const TargetOrder &) const = default;
};

TargetOrder target_order(const EbxInstance &p_target, int32_t p_field) {
	return { reinterpret_cast<uintptr_t>(&p_target.partition()), p_target.index(), p_field };
}

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

Result<LogicConnections> LogicConnections::of(const EbxResolver &p_db, const EbxInstance &p_blueprint, WorkBudget &r_budget) {
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
	BF3_TRY(r_budget.charge_type(p_blueprint.data())); // the three list lookups below
	for (const List &list : lists) {
		const std::optional<EbxValue> array_value = p_blueprint.get(list.name);
		const OptionalEbxArray array = array_value ? array_value->as_array() : OptionalEbxArray();
		if (!array) {
			continue;
		}
		// Every entry costs two resolves, whether or not they succeed, and one array may serve many
		// blueprints: charge them all before the scan (a wide connection struct type, its inline
		// SourceEvent / TargetEvent types included, costs more).
		BF3_TRY(r_budget.charge(detail::array_cost(*array)));
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
	// Stable: within one (target, field) the indices stay in connection order (the IW4L resolver's
	// order). n log n compares for n charged entries, whatever the keys.
	out.by_target_.resize(out.property.size());
	for (uint32_t i = 0; i < out.property.size(); ++i) {
		out.by_target_[i] = i;
	}
	std::stable_sort(out.by_target_.begin(), out.by_target_.end(), [&](uint32_t a, uint32_t b) {
		return target_order(out.property[a].target, out.property[a].target_field) < target_order(out.property[b].target, out.property[b].target_field);
	});
	return out;
}

std::span<const uint32_t> LogicConnections::property_into(const EbxInstance &p_target, int32_t p_field) const {
	const TargetOrder key = target_order(p_target, p_field);
	const auto lo = std::lower_bound(by_target_.begin(), by_target_.end(), key,
			[&](uint32_t p_at, const TargetOrder &p_key) { return target_order(property[p_at].target, property[p_at].target_field) < p_key; });
	const auto hi = std::upper_bound(lo, by_target_.end(), key,
			[&](const TargetOrder &p_key, uint32_t p_at) { return p_key < target_order(property[p_at].target, property[p_at].target_field); });
	return std::span<const uint32_t>(lo, hi);
}

// --- helpers ------------------------------------------------------------------------------------

std::optional<EbxValue> data_field(const EbxInstance &p_object, int32_t p_field) {
	// The fields() order (base types first, declaration order, '$' base links skipped), matched by
	// the descriptor's name hash: the parser keeps each field's ebx_name_hash, so no name is hashed
	// again here (a name can be as long as the partition's name table).
	const EbxPartition &partition = p_object.partition();
	const std::vector<EbxPartition::Complex> &complexes = partition.complex_descs();
	const std::vector<EbxPartition::Field> &fields = partition.field_descs();
	std::array<std::pair<uint32_t, size_t>, EbxPartition::kBaseDepth> chain{};
	size_t depth = 0;
	std::optional<std::pair<uint32_t, size_t>> next = std::make_pair(p_object.data().complex(), size_t(p_object.data().offset()));
	while (next && depth < chain.size() && next->first < complexes.size()) {
		chain[depth++] = *next;
		const EbxPartition::Complex &desc = complexes[next->first];
		std::optional<std::pair<uint32_t, size_t>> base;
		for (uint32_t f = desc.field_start; f < desc.field_end; ++f) {
			if (fields[f].code == ebx_code::kVoid) {
				base = std::make_pair(static_cast<uint32_t>(fields[f].reference), next->second + static_cast<size_t>(static_cast<int64_t>(fields[f].offset)));
				break;
			}
		}
		next = base;
	}
	while (depth > 0) {
		const auto [complex, at] = chain[--depth];
		const EbxPartition::Complex &desc = complexes[complex];
		for (uint32_t f = desc.field_start; f < desc.field_end; ++f) {
			const EbxPartition::Field &member = fields[f];
			if (member.code == ebx_code::kVoid || static_cast<int32_t>(member.hash) != p_field) {
				continue;
			}
			if (std::optional<EbxValue> value = partition.decode(f, at + static_cast<size_t>(static_cast<int64_t>(member.offset)))) {
				return value;
			}
		}
	}
	return std::nullopt;
}

Result<bool> subworld_included(const EbxResolver &p_db, const EbxInstance &p_reference, std::string_view p_mode, WorkBudget &r_budget) {
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
	BF3_TRY(r_budget.charge_type(settings->data()));
	const std::optional<EbxValue> list_value = settings->get("Settings");
	const OptionalEbxArray list = list_value ? list_value->as_array() : OptionalEbxArray();
	if (!list) {
		return true;
	}
	// Every entry is charged before the scan, null and unresolvable ones included: one reference
	// can be placed any number of times, and its settings array shared by any number of them.
	BF3_TRY(r_budget.charge(list->size()));
	for (const EbxValue entry : *list) {
		const std::optional<EbxRef> r = entry.as_ref();
		const std::optional<EbxInstance> resolved = r ? p_db.resolve(settings->partition(), *r) : std::nullopt;
		if (!resolved) {
			continue;
		}
		const EbxInstance &setting = *resolved;
		BF3_TRY(r_budget.charge_type(setting.data()));
		const std::optional<EbxInstance> criterion = p_db.follow(setting.data(), "Criterion");
		if (criterion) {
			BF3_TRY(r_budget.charge_type(criterion->data()));
			const std::optional<EbxValue> name = criterion->get("Name");
			const std::optional<std::string_view> text = name ? name->as_str() : std::nullopt;
			if (text && !iequals_ascii(*text, "GameMode")) {
				continue;
			}
		}
		bool enabled = false;
		const std::optional<EbxValue> options = setting.get("EnabledOptions");
		if (const OptionalEbxArray array = options ? options->as_array() : OptionalEbxArray()) {
			BF3_TRY(r_budget.charge(array->size()));
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
			world_(r_world), options_(p_options), budget_(*r_world.budget_) {}

	// Walks p_container's Objects. Every container on the current path is on path_, so a blueprint
	// that (directly or through others) contains itself is skipped instead of being re-entered.
	Status visit(const EbxInstance &p_container, uint32_t p_scope, uint32_t p_depth) {
		if (p_depth > options_.max_depth) {
			++world_.stats_.depth_limited;
			return Status::success();
		}
		BF3_TRY(step(1));
		BF3_TRY(budget_.charge_type(p_container.data())); // its Objects lookup scans the type chain
		path_.push_back(InstanceKey::of(p_container));
		const Status status = visit_objects(p_container, p_scope, p_depth);
		path_.pop_back();
		return status;
	}

	Result<uint32_t> push(const EbxInstance &p_blueprint, std::optional<EbxInstance> p_reference, std::optional<uint32_t> p_parent) {
		if (world_.scopes_.size() >= options_.max_scopes) {
			return make_error("logic world: more than " + std::to_string(options_.max_scopes) + " scopes");
		}
		const std::optional<uint32_t> set = world_.connection_set(p_blueprint);
		if (!set) {
			return budget_.error(); // the connection arrays did not fit the budget
		}
		LogicScope scope;
		scope.blueprint = p_blueprint;
		scope.reference = p_reference;
		scope.parent = p_parent;
		scope.depth = p_parent ? world_.scopes_[*p_parent].depth + 1 : 0;
		scope.connections = *set;
		world_.scopes_.push_back(scope);
		return static_cast<uint32_t>(world_.scopes_.size() - 1);
	}

private:
	// Adds walk steps (the max_walk_steps cap) for work already charged to the budget where it was
	// done (queries, inclusion tests).
	Status count(size_t p_count) {
		world_.stats_.walk_steps += p_count;
		if (world_.stats_.walk_steps > options_.max_walk_steps) {
			return make_error("logic world: walk exceeds " + std::to_string(options_.max_walk_steps) + " steps");
		}
		return Status::success();
	}

	// Walk steps for work about to be done: counted, and charged to the budget first.
	Status step(size_t p_count) {
		BF3_TRY(count(p_count));
		return budget_.charge(p_count);
	}

	// Records a sub-world bundle name in a stats list. A list keeps at most kMaxListedSubworlds
	// names (later ones are dropped), and each copy is charged by its length, so a reference reached
	// through a fan-out cannot grow the lists or the copying without bound.
	Status note(std::vector<std::string> &r_list, std::string_view p_name) {
		if (r_list.size() >= LogicWorldStats::kMaxListedSubworlds) {
			return Status::success();
		}
		BF3_TRY(budget_.charge_bytes(p_name.size()));
		r_list.emplace_back(p_name);
		return Status::success();
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
			BF3_TRY(budget_.charge_type(object.data())); // its field lookups below
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

	// The resolved Objects of a container. Every array entry is charged to the walk before any is
	// examined, null and unresolvable ones included: each costs a resolve, and a part reached
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
		// The inclusion test charges the budget itself (before each array scan); its work also
		// counts as walk steps.
		const size_t before = budget_.used();
		const Result<bool> included = subworld_included(world_.db(), p_reference, options_.mode, budget_);
		BF3_TRY(count(budget_.used() - before));
		if (!included) {
			return included.error();
		}
		if (!*included) {
			return note(world_.stats_.skipped_subworlds, name);
		}
		Result<bool> skip = excluded(p_scope, p_reference);
		if (!skip) {
			return skip.error();
		}
		if (*skip) {
			return Status::success();
		}
		const std::optional<EbxInstance> data = world_.referenced_blueprint(p_reference);
		BF3_TRY(budget_.status()); // nullopt above may mean the name copy did not fit
		if (!data) {
			return note(world_.stats_.absent_subworlds, name);
		}
		if (on_path(*data)) {
			return Status::success();
		}
		BF3_TRY(note(world_.stats_.subworlds, name));
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
		// Any number of references may name one blueprint: each pays for the Object lookup below.
		BF3_TRY(budget_.charge_type(blueprint->data()));
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

	// Whether a reference is Excluded (connected value, else its own field). The query charges the
	// budget step by step; its steps also count as walk steps.
	Result<bool> excluded(uint32_t p_scope, const EbxInstance &p_reference) {
		bool value = false;
		size_t steps = 0;
		const std::optional<PropertyValue> v = world_.property_counted(p_scope, p_reference, field_id("Excluded"), steps);
		BF3_TRY(count(steps));
		BF3_TRY(budget_.status());
		// With the query cap spent the connected value is unknown; the stored field below could
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
	WorkBudget &budget_;
	std::vector<InstanceKey> path_; // containers being walked, outermost first (at most max_depth + 1)
};

// --- LogicWorld ----------------------------------------------------------------------------------

Result<LogicWorld> LogicWorld::build(const EbxResolver &p_db, const EbxInstance &p_root, const LogicWorldOptions &p_options) {
	LogicWorld world;
	world.db_ = &p_db;
	world.max_query_steps_ = p_options.max_query_steps;
	if (p_options.budget != nullptr) {
		world.budget_ = p_options.budget;
	} else {
		world.own_budget_ = std::make_unique<WorkBudget>(p_options.max_work);
		world.budget_ = world.own_budget_.get();
	}
	BF3_TRY(world.budget_->status());
	LogicWalker walker(world, p_options);
	Result<uint32_t> root = walker.push(p_root, std::nullopt, std::nullopt);
	if (!root) {
		return root.error();
	}
	BF3_TRY(walker.visit(p_root, *root, 0));
	// A helper that ran out of budget returns an empty answer; the walk must still end in the error.
	BF3_TRY(world.budget_->status());
	return world;
}

std::optional<uint32_t> LogicWorld::connection_set(const EbxInstance &p_blueprint) const {
	const InstanceKey key = InstanceKey::of(p_blueprint);
	const auto it = set_by_blueprint_.find(key);
	if (it != set_by_blueprint_.end()) {
		return it->second;
	}
	Result<LogicConnections> set = LogicConnections::of(*db_, p_blueprint, *budget_);
	if (!set) {
		return std::nullopt; // the budget is spent (for good): the caller reports its error
	}
	const uint32_t at = static_cast<uint32_t>(sets_.size());
	sets_.push_back(std::move(*set));
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
		// The lowercase copy (and the resolver's own) costs the name's length.
		if (!budget_->take(1 + text->size() / WorkBudget::kBytesPerUnit)) {
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
	// Any number of queries may ask one entity: each stored lookup is charged.
	if (!budget_->take_type(p_entity.data())) {
		return std::nullopt;
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
	if (!budget_->take_type(p_object.entity.data())) {
		return std::nullopt;
	}
	const std::optional<EbxValue> stored = p_object.entity.get(p_field);
	return stored ? std::optional<PropertyValue>(PropertyValue::of_field(*stored)) : std::nullopt;
}

bool LogicWorld::query_step(size_t &r_steps) const {
	if (++r_steps > kQuerySteps) {
		return false;
	}
	return budget_->take(1);
}

std::optional<PropertyValue> LogicWorld::property_in(const Frame &p_frame, const EbxInstance &p_target, int32_t p_field, uint32_t p_depth, size_t &r_steps) const {
	if (p_depth == 0 || !query_step(r_steps)) {
		return std::nullopt;
	}
	const uint32_t depth = p_depth - 1;
	const EbxInstance blueprint = p_frame.child ? p_frame.blueprint : scopes_[p_frame.scope].blueprint;
	const std::optional<uint32_t> set_at = connection_set(blueprint);
	if (!set_at) {
		return std::nullopt;
	}
	// A deque keeps `set` (and the index the span views) valid while source_value adds sets.
	const LogicConnections &set = sets_[*set_at];
	for (const uint32_t at : set.property_into(p_target, p_field)) {
		// Many connections may feed one field: stop as soon as the next step would fail.
		if (query_spent(r_steps)) {
			return std::nullopt;
		}
		const LogicConnection connection = set.property[at];
		if (std::optional<PropertyValue> v = source_value(p_frame, connection.source, connection.source_field, depth, r_steps)) {
			return v;
		}
	}
	return std::nullopt;
}

std::optional<PropertyValue> LogicWorld::source_value(const Frame &p_frame, const EbxInstance &p_source, int32_t p_field, uint32_t p_depth, size_t &r_steps) const {
	if (p_depth == 0 || !query_step(r_steps)) {
		return std::nullopt;
	}
	const uint32_t depth = p_depth - 1;
	const EbxInstance blueprint = p_frame.child ? p_frame.blueprint : scopes_[p_frame.scope].blueprint;
	// Field lookups on the blueprint and the source (data_field, referenced_blueprint).
	if (!budget_->take_type(blueprint.data()) || !budget_->take_type(p_source.data())) {
		return std::nullopt;
	}
	if (const std::optional<EbxInstance> descriptor = db_->follow(blueprint.data(), "Descriptor"); descriptor && *descriptor == p_source) {
		return interface_in(p_frame, p_field, depth, r_steps);
	}
	// A child prefab's or sub-world's output port: continue inside it from its interface.
	{
		const std::optional<EbxInstance> inner = referenced_blueprint(p_source);
		if (inner && !budget_->take_type(inner->data())) { // its Descriptor lookup
			return std::nullopt;
		}
		const std::optional<EbxInstance> descriptor = inner ? db_->follow(inner->data(), "Descriptor") : std::nullopt;
		if (descriptor) {
			Frame child;
			child.parent_frame = &p_frame;
			child.child = true;
			child.reference = p_source;
			child.blueprint = *inner;
			const std::optional<uint32_t> set_at = connection_set(*inner);
			if (!set_at) {
				return std::nullopt;
			}
			const LogicConnections &set = sets_[*set_at];
			for (const uint32_t at : set.property_into(*descriptor, p_field)) {
				if (query_spent(r_steps)) {
					return std::nullopt;
				}
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
	if (p_depth == 0 || !query_step(r_steps)) {
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
	if (!budget_->take_type(blueprint.data())) {
		return std::nullopt;
	}
	const std::optional<EbxInstance> descriptor = db_->follow(blueprint.data(), "Descriptor");
	if (!descriptor || !budget_->take_type(descriptor->data())) {
		return std::nullopt;
	}
	const std::optional<EbxValue> fields = descriptor->get("Fields");
	const OptionalEbxArray array = fields ? fields->as_array() : OptionalEbxArray();
	if (!array) {
		return std::nullopt;
	}
	size_t entry_extra = 0; // a wide DataField struct type costs more per entry
	if (const std::optional<EbxValue> first = array->size() > 0 ? array->get(0) : std::nullopt) {
		if (const std::optional<EbxStruct> element = first->as_struct()) {
			entry_extra = WorkBudget::type_width(*element) / WorkBudget::kFieldsPerUnit;
		}
	}
	for (const EbxValue entry : *array) {
		// Each field scanned is a step, so a huge Fields array cannot multiply the query cost.
		if (!query_step(r_steps) || !budget_->take(entry_extra)) {
			return std::nullopt;
		}
		const std::optional<EbxValue> id = entry.get("Id");
		if (!id || id->as_i32() != p_field) {
			continue;
		}
		const std::optional<EbxValue> value = entry.get("Value");
		if (const std::optional<std::string_view> text = value ? value->as_str() : std::nullopt) {
			// Parsing reads the whole constant.
			if (!budget_->take(1 + text->size() / WorkBudget::kBytesPerUnit)) {
				return std::nullopt;
			}
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
