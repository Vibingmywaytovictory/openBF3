#pragma once

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

namespace bf3::core::ui::detail {

inline std::optional<double> number(const std::optional<EbxValue> &p_value) {
	if (!p_value) {
		return std::nullopt;
	}
	if (const std::optional<double> f = p_value->as_f64()) {
		return f;
	}
	if (p_value->kind() == EbxValue::Kind::Enum) {
		return std::nullopt;
	}
	if (const std::optional<int64_t> i = p_value->as_i64()) {
		return double(*i);
	}
	if (const std::optional<uint64_t> u = p_value->as_u64()) {
		return double(*u);
	}
	return std::nullopt;
}

inline std::optional<float> f32(const EbxStruct &p_s, std::string_view p_path) {
	const std::optional<double> d = number(p_s.get_path(p_path));
	return d ? std::optional<float>(float(*d)) : std::nullopt;
}

inline float f32_or(const EbxStruct &p_s, std::string_view p_path, float p_default) {
	return f32(p_s, p_path).value_or(p_default);
}

inline bool flag_or(const EbxStruct &p_s, std::string_view p_path, bool p_default) {
	const std::optional<EbxValue> v = p_s.get_path(p_path);
	const std::optional<bool> b = v ? v->as_bool() : std::nullopt;
	return b.value_or(p_default);
}

inline int32_t i32_or(const EbxStruct &p_s, std::string_view p_field, int32_t p_default) {
	const std::optional<EbxValue> v = p_s.get(p_field);
	return v ? v->as_i32().value_or(p_default) : p_default;
}

inline std::array<float, 4> vec4_or(const EbxStruct &p_s, std::string_view p_field, std::array<float, 4> p_default) {
	const std::optional<EbxValue> v = p_s.get(p_field);
	const std::optional<EbxStruct> s = v ? v->as_struct() : std::nullopt;
	if (!s) {
		return p_default;
	}
	return { f32_or(*s, "x", p_default[0]), f32_or(*s, "y", p_default[1]), f32_or(*s, "z", p_default[2]), f32_or(*s, "w", p_default[3]) };
}

inline std::optional<std::array<float, 2>> vec2(const EbxStruct &p_s, std::string_view p_field) {
	const std::optional<EbxValue> v = p_s.get(p_field);
	const std::optional<EbxStruct> s = v ? v->as_struct() : std::nullopt;
	if (!s) {
		return std::nullopt;
	}
	const std::optional<float> x = f32(*s, "x");
	const std::optional<float> y = f32(*s, "y");
	if (!x || !y) {
		return std::nullopt;
	}
	return std::array<float, 2>{ *x, *y };
}

inline std::optional<EbxStruct> sub(const EbxStruct &p_s, std::string_view p_field) {
	const std::optional<EbxValue> v = p_s.get(p_field);
	return v ? v->as_struct() : std::nullopt;
}

inline std::optional<EbxRef> ref(const EbxStruct &p_s, std::string_view p_field) {
	const std::optional<EbxValue> v = p_s.get(p_field);
	return v ? v->as_ref() : std::nullopt;
}

inline std::optional<std::string> ref_partition(const EbxResolver &p_db, const EbxPartition &p_from, const EbxRef &p_ref) {
	if (p_ref.is_null()) {
		return std::string();
	}
	const Guid partition = p_ref.kind == EbxRef::Kind::External ? p_ref.partition : p_from.guid();
	const std::optional<std::string_view> name = p_db.name_of(partition);
	if (!name) {
		return std::nullopt;
	}
	return std::string(*name);
}

inline std::string instance_label(const EbxResolver &p_db, const EbxInstance &p_instance) {
	const std::optional<std::string_view> name = p_db.name_of(p_instance.partition().guid());
	return (name ? std::string(*name) : std::string("?") + p_instance.partition().guid().to_string()) + "#" + std::to_string(p_instance.index());
}

inline bool finite_in_unit(float p_x) {
	return std::isfinite(p_x) && p_x >= 0.0f && p_x <= 1.0f;
}

}
