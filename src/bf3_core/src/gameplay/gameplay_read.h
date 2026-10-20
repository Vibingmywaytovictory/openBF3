#pragma once

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::gameplay::detail {

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

inline std::string copy_text(WorkBudget &r_budget, std::string_view p_text) {
	if (!r_budget.take(1 + p_text.size() / WorkBudget::kBytesPerUnit)) {
		return {};
	}
	return std::string(p_text);
}

inline std::string text(WorkBudget &r_budget, const EbxStruct &p_object, std::string_view p_path) {
	return copy_text(r_budget, text_view(p_object, p_path));
}

inline size_t array_cost(const EbxArray &p_array) {
	return p_array.size();
}

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

				if (!r_budget.take_type(i->data())) {
					return out;
				}
				out.push_back(*i);
			}
		}
	}
	return out;
}

inline std::string partition_name(WorkBudget &r_budget, const EbxResolver &p_db, const EbxInstance &p_instance) {
	return copy_text(r_budget, p_db.name_of(p_instance.partition().guid()).value_or(std::string_view()));
}

inline std::string ref_name(WorkBudget &r_budget, const EbxResolver &p_db, const EbxStruct &p_object, std::string_view p_field) {
	const std::optional<EbxInstance> target = p_db.follow(p_object, p_field);
	return target ? partition_name(r_budget, p_db, *target) : std::string();
}

inline std::vector<std::pair<std::string, double>> scalars(WorkBudget &r_budget, const EbxInstance &p_object) {
	std::vector<std::pair<std::string, double>> out;
	if (!r_budget.charge_fields(p_object.data()).ok()) {
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

}
