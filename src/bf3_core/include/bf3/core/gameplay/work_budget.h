#pragma once
// WorkBudget: the total work one gameplay read may do. One budget object covers a whole read (a
// LogicWorld walk with every property query, sub-world inclusion test and connection set it needs,
// and the mode, kit, team, soldier or vehicle reader around it), so hostile data cannot multiply
// work that is cheap per step: a world part reached k times, an array shared by many instances, or a
// connection loop all draw from the same total.
//
// Units: one per EBX array element scanned (null and unresolvable entries included), per reference
// resolved or connection hop taken, per object or table entry examined, one per kBytesPerUnit bytes
// of a string copied, and one per kFieldsPerUnit descriptors of a wide type (its chain, the
// enumerators of its enum fields and its inline struct types: EbxPartition::lookup_width) each time
// an instance or struct of it is examined (charge_type). Callers charge BEFORE the work, at the
// place it happens.
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
	// Type fields one examination of an instance or struct may scan for free (see charge_type).
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
	// An instance or struct about to be examined: a few field lookups, each a path of at most
	// EbxPartition::kLookupDepth segments through its inline (ValueType) fields, or one fields()
	// pass. Each such access scans at most type_width descriptors (the type chain, the enumerators
	// of its enum fields and the deepest inline field types; up to 255 fields x kBaseDepth (32) base
	// types per level), so a type wider than kFieldsPerUnit costs one more unit per kFieldsPerUnit;
	// retail types are narrower or about as wide, and cost nothing or a unit extra. A struct reached
	// through an inline field of a charged one is covered by that charge; one reached any other way
	// (an array element, a reference) is charged itself.
	Status charge_type(const EbxStruct &p_data) { return charge(type_width(p_data) / kFieldsPerUnit); }
	bool take_type(const EbxStruct &p_data) { return charge_type(p_data).ok(); }

	// EbxPartition::lookup_width of the struct's type (precomputed when the partition was parsed).
	static size_t type_width(const EbxStruct &p_data) { return p_data.partition().lookup_width(p_data.complex()); }

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
