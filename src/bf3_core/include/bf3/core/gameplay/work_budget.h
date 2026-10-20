#pragma once

#include "bf3/core/ebx.h"
#include "bf3/core/result.h"

#include <cstddef>
#include <optional>
#include <string>

namespace bf3::core::gameplay {

class WorkBudget {
public:

	static constexpr size_t kDefaultLimit = 1000000;

	static constexpr size_t kBytesPerUnit = 64;

	static constexpr size_t kFieldsPerUnit = 8;

	explicit WorkBudget(size_t p_limit = kDefaultLimit) :
			limit_(p_limit) {}

	Status charge(size_t p_units) {
		if (exhausted_ || p_units > limit_ - used_) {
			exhausted_ = true;
			return error();
		}
		used_ += p_units;
		return Status::success();
	}

	Status charge_bytes(size_t p_bytes) { return charge(1 + p_bytes / kBytesPerUnit); }
	bool take(size_t p_units) { return charge(p_units).ok(); }

	Status charge_type(const EbxStruct &p_data) {
		(void)p_data;
		return charge(1);
	}
	bool take_type(const EbxStruct &p_data) { return charge_type(p_data).ok(); }

	Status charge_fields(const EbxStruct &p_data) { return charge(1 + chain_descriptors(p_data) / kFieldsPerUnit); }

	static size_t chain_descriptors(const EbxStruct &p_data) {
		const std::vector<EbxPartition::Complex> &complexes = p_data.partition().complex_descs();
		size_t out = 0;
		std::optional<uint32_t> at = p_data.complex();
		for (size_t depth = 0; at && *at < complexes.size() && depth < EbxPartition::kBaseDepth; ++depth) {
			out += complexes[*at].field_end - complexes[*at].field_start;
			at = complexes[*at].base;
		}
		return out;
	}

	Status status() const { return exhausted_ ? Status(error()) : Status::success(); }
	bool exhausted() const { return exhausted_; }
	size_t used() const { return used_; }
	size_t limit() const { return limit_; }
	Error error() const { return make_error("work budget of " + std::to_string(limit_) + " units exhausted"); }

private:
	size_t limit_ = kDefaultLimit;
	size_t used_ = 0;
	bool exhausted_ = false;
};

}
