// The work budget of a level load and traversal (see budget.h and the cost model in world.cpp).
#include "bf3/core/level/budget.h"

#include <string>

namespace bf3::core::level {

Error WorkBudget::error() const {
	return make_error("level traversal: more than " + std::to_string(limit_) + " work steps (a hostile or corrupt blueprint graph)");
}

Status WorkBudget::charge(size_t p_units) {
	if (exhausted_.load(std::memory_order_relaxed)) {
		return error();
	}
	if (p_units > limit_) {
		// Not added, so spent_ cannot wrap whatever a caller asks for.
		exhausted_.store(true, std::memory_order_relaxed);
		return error();
	}
	const size_t before = spent_.fetch_add(p_units, std::memory_order_relaxed);
	if (before + p_units > limit_) {
		exhausted_.store(true, std::memory_order_relaxed);
		return error();
	}
	return Status::success();
}

Status WorkBudget::status() const {
	return exhausted() ? Status(error()) : Status::success();
}

Result<std::optional<std::string_view>> charged_str(const std::optional<EbxValue> &p_value, WorkBudget &r_budget) {
	BF3_TRY(r_budget.status());
	const std::optional<std::string_view> text = p_value ? p_value->as_str() : std::nullopt;
	if (text) {
		BF3_TRY(r_budget.charge(text_units(text->size())));
	}
	return text;
}

Result<std::optional<std::string_view>> charged_str(const EbxStruct &p_object, std::string_view p_field, WorkBudget &r_budget) {
	return charged_str(p_object.get(p_field), r_budget);
}

Result<std::optional<std::string_view>> charged_enum_name(const std::optional<EbxValue> &p_value, WorkBudget &r_budget) {
	BF3_TRY(r_budget.status());
	const std::optional<std::string_view> name = p_value ? p_value->enum_name() : std::nullopt;
	if (name) {
		BF3_TRY(r_budget.charge(text_units(name->size())));
	}
	return name;
}

Status charge_text(WorkBudget &r_budget, std::string_view p_text) {
	return r_budget.charge(text_units(p_text.size()));
}

size_t array_length(const EbxStruct &p_object, std::string_view p_field) {
	const std::optional<EbxValue> v = p_object.get(p_field);
	const OptionalEbxArray array = v ? v->as_array() : OptionalEbxArray();
	return array ? array->size() : 0;
}

Status charged_follow_all(const EbxResolver &p_db, const EbxStruct &p_object, std::string_view p_field, WorkBudget &r_budget,
		std::vector<EbxInstance> &r_out) {
	BF3_TRY(r_budget.charge(array_length(p_object, p_field)));
	r_out = p_db.follow_all(p_object, p_field);
	return Status::success();
}

} // namespace bf3::core::level
