#pragma once
// Small field readers shared by the gameplay table readers (soldier, kit, mode, vehicle). Every
// accessor tolerates a missing field or another type and returns nullopt / the fallback.
//
// The readers that scan an array, copy a string or follow a list of references take the read's
// WorkBudget and charge it before the work (see work_budget.h and the cost model in
// logic_world.cpp). Once the budget is exhausted they return an empty value at once; the public
// reader that called them reports the budget error. The others read one field of an instance or
// struct the caller has charged (WorkBudget::charge_type covers the lookups of a wide type).

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::gameplay::detail {

// A numeric field as double: floats, integers (not enums).
inline std::optional<double> number(const EbxValue &p_value) {
	if (const std::optional<double> f = p_value.as_f64()) {
		return f;
	}
	if (p_value.kind() == EbxValue::Kind::Enum) {
		return std::nullopt;
	}
	if (const std::optional<int64_t> i = p_value.as_i64()) {
		return static_cast<double>(*i);
	}
	if (const std::optional<uint64_t> u = p_value.as_u64()) {
		return static_cast<double>(*u);
	}
	return std::nullopt;
}

inline std::optional<double> number(const EbxStruct &p_object, std::string_view p_path) {
	const std::optional<EbxValue> v = p_object.get_path(p_path);
	return v ? number(*v) : std::nullopt;
}

inline double number_or(const EbxStruct &p_object, std::string_view p_path, double p_fallback) {
	return number(p_object, p_path).value_or(p_fallback);
}

inline std::optional<bool> boolean(const EbxStruct &p_object, std::string_view p_path) {
	const std::optional<EbxValue> v = p_object.get_path(p_path);
	return v ? v->as_bool() : std::nullopt;
}

// A string or enum field as a view into the partition (no copy).
inline std::string_view text_view(const EbxValue &p_value) {
	if (const std::optional<std::string_view> s = p_value.as_str()) {
		return *s;
	}
	if (const std::optional<std::string_view> e = p_value.enum_name()) {
		return *e;
	}
	return {};
}

inline std::string_view text_view(const EbxStruct &p_object, std::string_view p_path) {
	const std::optional<EbxValue> v = p_object.get_path(p_path);
	return v ? text_view(*v) : std::string_view();
}

inline std::optional<std::array<float, 3>> vec3(const EbxStruct &p_object, std::string_view p_path) {
	const std::optional<EbxValue> v = p_object.get_path(p_path);
	return v ? v->as_vec3() : std::nullopt;
}

// A copy of p_text, charged by its length first; empty once the budget is spent.
inline std::string copy_text(WorkBudget &r_budget, std::string_view p_text) {
	if (!r_budget.take(1 + p_text.size() / WorkBudget::kBytesPerUnit)) {
		return {};
	}
	return std::string(p_text);
}

// A string or enum field, copied (charged).
inline std::string text(WorkBudget &r_budget, const EbxStruct &p_object, std::string_view p_path) {
	return copy_text(r_budget, text_view(p_object, p_path));
}

// The units a scan of p_array costs: its raw element count, times one more per kFieldsPerUnit
// fields of a wide struct element type (see WorkBudget::charge_type).
inline size_t array_cost(const EbxArray &p_array) {
	size_t extra = 0; // every element has the array's member type
	if (const std::optional<EbxValue> first = p_array.size() > 0 ? p_array.get(0) : std::nullopt) {
		if (const std::optional<EbxStruct> element = first->as_struct()) {
			extra = WorkBudget::type_width(*element) / WorkBudget::kFieldsPerUnit;
		}
	}
	return p_array.size() * (1 + extra);
}

// The array a field holds, its element count charged before any element is looked at (struct
// elements of a wide type cost more, see WorkBudget::charge_type). No array (absent, another type,
// or the budget spent) is an empty OptionalEbxArray.
inline OptionalEbxArray array_of(WorkBudget &r_budget, const std::optional<EbxValue> &p_value) {
	const OptionalEbxArray out = p_value ? p_value->as_array() : OptionalEbxArray();
	if (!out) {
		return OptionalEbxArray();
	}
	if (!r_budget.take(array_cost(*out))) {
		return OptionalEbxArray();
	}
	return out;
}

inline OptionalEbxArray array_of(WorkBudget &r_budget, const EbxStruct &p_object, std::string_view p_path) {
	return array_of(r_budget, p_object.get_path(p_path));
}

// EbxResolver::follow_all with the raw element count charged first: null and unresolvable entries
// cost as much as the ones that resolve. Partial once the budget is spent (the caller checks it).
inline std::vector<EbxInstance> follow_all(WorkBudget &r_budget, const EbxResolver &p_db, const EbxStruct &p_object, std::string_view p_field) {
	std::vector<EbxInstance> out;
	const OptionalEbxArray list = array_of(r_budget, p_object.get(p_field));
	if (!list) {
		return out;
	}
	out.reserve(list->size());
	for (const EbxValue element : *list) {
		const std::optional<EbxRef> r = element.as_ref();
		if (r) {
			if (const std::optional<EbxInstance> i = p_db.resolve(p_object.partition(), *r)) {
				// The caller examines each instance: a wide type costs its lookups up front.
				if (!r_budget.take_type(i->data())) {
					return out;
				}
				out.push_back(*i);
			}
		}
	}
	return out;
}

// The lowercase partition name an instance lives in (charged), empty when the resolver does not know it.
inline std::string partition_name(WorkBudget &r_budget, const EbxResolver &p_db, const EbxInstance &p_instance) {
	return copy_text(r_budget, p_db.name_of(p_instance.partition().guid()).value_or(std::string_view()));
}

// The partition name a reference field points at (follows the reference).
inline std::string ref_name(WorkBudget &r_budget, const EbxResolver &p_db, const EbxStruct &p_object, std::string_view p_field) {
	const std::optional<EbxInstance> target = p_db.follow(p_object, p_field);
	return target ? partition_name(r_budget, p_db, *target) : std::string();
}

// Every numeric or boolean field of an object, base types first, as (name, value); the
// blueprint bookkeeping fields (IndexInBlueprint, Is*ConnectionTarget, RuntimeComponentCount) skipped.
inline std::vector<std::pair<std::string, double>> scalars(WorkBudget &r_budget, const EbxInstance &p_object) {
	std::vector<std::pair<std::string, double>> out;
	if (!r_budget.take_type(p_object.data())) {
		return out;
	}
	for (const EbxField &f : p_object.fields()) {
		if (f.name == "IndexInBlueprint" || f.name == "IsEventConnectionTarget" || f.name == "IsPropertyConnectionTarget" ||
				f.name == "RuntimeComponentCount") {
			continue;
		}
		if (const std::optional<bool> b = f.value.as_bool()) {
			out.emplace_back(copy_text(r_budget, f.name), *b ? 1.0 : 0.0);
		} else if (const std::optional<double> n = number(f.value)) {
			out.emplace_back(copy_text(r_budget, f.name), *n);
		}
	}
	return out;
}

} // namespace bf3::core::gameplay::detail
