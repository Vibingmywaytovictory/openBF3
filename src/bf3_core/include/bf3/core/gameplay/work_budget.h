#pragma once
// WorkBudget: the total work one gameplay read may do. One budget object covers a whole read (a
// LogicWorld walk with every property query, sub-world inclusion test and connection set it needs,
// and the mode, kit, team, soldier or vehicle reader around it), so hostile data cannot multiply
// work that is cheap per step: a world part reached k times, an array shared by many instances, or a
// connection loop all draw from the same total.
//
// Units: one per EBX array element scanned (null and unresolvable entries included), per reference
// resolved or connection hop taken, per object or table entry examined (charge_type), one per
// kBytesPerUnit bytes of a string copied, and one per kFieldsPerUnit descriptors of a fields() pass
// (charge_fields). Callers charge BEFORE the work, at the place it happens.
// Once a charge would pass the limit the budget is exhausted for good: every later charge fails at
// once with the same error, so loops and recursions stop at their next charge, and each public
// reader ends with status() so a helper that returned early (an empty list, nullopt) never yields a
// partial result.
// See the cost model at the top of src/gameplay/logic_world.cpp.

#include "bf3/core/ebx.h"
#include "bf3/core/result.h"

#include <cstddef>
#include <optional>
#include <string>

namespace bf3::core::gameplay {

class WorkBudget {
public:
	// Retail reads stay far below this (README "Soldier, kit, mode and vehicle tables" lists the
	// measured maxima); hostile data that asks for more ends with the budget error within seconds.
	static constexpr size_t kDefaultLimit = 1000000;
	// String bytes one unit pays for.
	static constexpr size_t kBytesPerUnit = 64;
	// Descriptors one unit of a fields() pass pays for (see charge_fields).
	static constexpr size_t kFieldsPerUnit = 64;

	explicit WorkBudget(size_t p_limit = kDefaultLimit) :
			limit_(p_limit) {}

	// Charges p_units for work about to be done. Fails, and stays failed, when the total would pass
	// the limit; the failing charge is not added to used().
	Status charge(size_t p_units) {
		if (exhausted_ || p_units > limit_ - used_) {
			exhausted_ = true;
			return error();
		}
		used_ += p_units;
		return Status::success();
	}
	// A string of p_bytes about to be copied: one unit plus one per kBytesPerUnit bytes.
	Status charge_bytes(size_t p_bytes) { return charge(1 + p_bytes / kBytesPerUnit); }
	bool take(size_t p_units) { return charge(p_units).ok(); }
	// An instance or struct about to be examined: one unit. An examination is a fixed number of field
	// lookups (a reader names its fields as literals), each a path of a few literal segments through
	// inline (ValueType) fields, and the EBX parser makes every segment O(log n) whatever the type's
	// layout: lookups and enumerator decodes are binary searches over tables built at parse time, a
	// string decode a binary search over the NUL offsets (EbxPartition::lookup, enumerator, string).
	// A struct reached through an inline field of a charged one is part of that examination; one
	// reached any other way (an array element, a reference) is charged itself.
	Status charge_type(const EbxStruct &p_data) {
		(void)p_data;
		return charge(1);
	}
	bool take_type(const EbxStruct &p_data) { return charge_type(p_data).ok(); }
	// A fields() pass over a struct about to be made: one unit plus one per kFieldsPerUnit descriptors
	// of its type chain (at most EbxPartition::kChainFields, the parser rejects wider chains).
	Status charge_fields(const EbxStruct &p_data) { return charge(1 + chain_descriptors(p_data) / kFieldsPerUnit); }

	// The field descriptors along a struct's base chain (at most kBaseDepth types, so O(1)).
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

	// The budget error once exhausted, else success.
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

} // namespace bf3::core::gameplay
