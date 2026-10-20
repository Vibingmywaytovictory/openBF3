#pragma once

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/result.h"

#include <atomic>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace bf3::core::level {

class WorkBudget {
public:

	static constexpr size_t kDefaultLimit = size_t(1) << 22;

	explicit WorkBudget(size_t p_limit = kDefaultLimit) :
			limit_(p_limit) {}
	WorkBudget(const WorkBudget &) = delete;
	WorkBudget &operator=(const WorkBudget &) = delete;

	Status charge(size_t p_units);
	bool exhausted() const { return exhausted_.load(std::memory_order_relaxed); }

	Status status() const;
	size_t spent() const { return spent_.load(std::memory_order_relaxed); }
	size_t limit() const { return limit_; }

private:
	Error error() const;

	const size_t limit_;
	std::atomic<size_t> spent_{ 0 };
	std::atomic<bool> exhausted_{ false };
};

inline constexpr size_t kTextBytesPerUnit = 16;
constexpr size_t text_units(size_t p_length) {
	return 1 + p_length / kTextBytesPerUnit;
}

Result<std::optional<std::string_view>> charged_str(const std::optional<EbxValue> &p_value, WorkBudget &r_budget);

Result<std::optional<std::string_view>> charged_str(const EbxStruct &p_object, std::string_view p_field, WorkBudget &r_budget);

Result<std::optional<std::string_view>> charged_enum_name(const std::optional<EbxValue> &p_value, WorkBudget &r_budget);

Status charge_text(WorkBudget &r_budget, std::string_view p_text);

size_t array_length(const EbxStruct &p_object, std::string_view p_field);

Status charged_follow_all(const EbxResolver &p_db, const EbxStruct &p_object, std::string_view p_field, WorkBudget &r_budget,
		std::vector<EbxInstance> &r_out);

}
