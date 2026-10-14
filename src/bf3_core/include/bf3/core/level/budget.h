#pragma once
// The work budget of a level load, its traversal and every helper run on the traversed World (the
// property resolver, start-hidden, hidden parts, mesh census, collision placements, sub-world
// inclusion, level info). One object bounds the total: each piece of work charges its units BEFORE
// it is done, at the place it is done (an array's raw element count before the array is scanned,
// a connection hop before it is taken), and once a charge fails the budget stays exhausted, so
// every later charge fails at once and the walk ends with the budget error. The cost model (which
// loop charges what, and the structural caps of the rest) is the comment block at the top of
// src/level/world.cpp.

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/result.h"

#include <atomic>
#include <cstddef>
#include <string_view>
#include <vector>

namespace bf3::core::level {

class WorkBudget {
public:
	// Retail levels spend at most a few percent of it (see world.cpp for the measured maxima); only
	// a hostile or corrupt partition graph reaches it.
	static constexpr size_t kDefaultLimit = size_t(1) << 22;

	explicit WorkBudget(size_t p_limit = kDefaultLimit) :
			limit_(p_limit) {}
	WorkBudget(const WorkBudget &) = delete;
	WorkBudget &operator=(const WorkBudget &) = delete;

	// Pays for p_units of work about to be done. Fails when the total would pass the limit, and
	// from then on every charge fails (sticky). Safe to call from several threads.
	Status charge(size_t p_units);
	bool exhausted() const { return exhausted_.load(std::memory_order_relaxed); }
	// The budget error once exhausted, success before.
	Status status() const;
	size_t spent() const { return spent_.load(std::memory_order_relaxed); }
	size_t limit() const { return limit_; }

private:
	Error error() const;

	const size_t limit_;
	std::atomic<size_t> spent_{ 0 };
	std::atomic<bool> exhausted_{ false };
};

// The raw element count of array field p_field of p_object (null and unresolved slots included);
// 0 when the object has no such array.
size_t array_length(const EbxStruct &p_object, std::string_view p_field);

// p_db.follow_all(p_object, p_field) after charging the array's raw length to r_budget.
Status charged_follow_all(const EbxResolver &p_db, const EbxStruct &p_object, std::string_view p_field, WorkBudget &r_budget,
		std::vector<EbxInstance> &r_out);

} // namespace bf3::core::level
