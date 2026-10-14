#pragma once
// Small field readers shared by the gameplay table readers (soldier, kit, mode, vehicle). Every
// accessor tolerates a missing field or another type and returns nullopt / the fallback.

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"

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

inline std::string text(const EbxStruct &p_object, std::string_view p_path) {
	const std::optional<EbxValue> v = p_object.get_path(p_path);
	if (!v) {
		return {};
	}
	if (const std::optional<std::string_view> s = v->as_str()) {
		return std::string(*s);
	}
	if (const std::optional<std::string_view> e = v->enum_name()) {
		return std::string(*e);
	}
	return {};
}

inline std::optional<std::array<float, 3>> vec3(const EbxStruct &p_object, std::string_view p_path) {
	const std::optional<EbxValue> v = p_object.get_path(p_path);
	return v ? v->as_vec3() : std::nullopt;
}

// The lowercase partition name an instance lives in, empty when the resolver does not know it.
inline std::string partition_name(const EbxResolver &p_db, const EbxInstance &p_instance) {
	return std::string(p_db.name_of(p_instance.partition().guid()).value_or(std::string_view()));
}

// The partition name a reference field points at (follows the reference).
inline std::string ref_name(const EbxResolver &p_db, const EbxStruct &p_object, std::string_view p_field) {
	const std::optional<EbxInstance> target = p_db.follow(p_object, p_field);
	return target ? partition_name(p_db, *target) : std::string();
}

// Every numeric or boolean field of an object, base types first, as (name, value); the
// blueprint bookkeeping fields (IndexInBlueprint, Is*ConnectionTarget, RuntimeComponentCount) skipped.
inline std::vector<std::pair<std::string, double>> scalars(const EbxInstance &p_object) {
	std::vector<std::pair<std::string, double>> out;
	for (const EbxField &f : p_object.fields()) {
		if (f.name == "IndexInBlueprint" || f.name == "IsEventConnectionTarget" || f.name == "IsPropertyConnectionTarget" ||
				f.name == "RuntimeComponentCount") {
			continue;
		}
		if (const std::optional<bool> b = f.value.as_bool()) {
			out.emplace_back(std::string(f.name), *b ? 1.0 : 0.0);
		} else if (const std::optional<double> n = number(f.value)) {
			out.emplace_back(std::string(f.name), *n);
		}
	}
	return out;
}

} // namespace bf3::core::gameplay::detail
