// read_grid_pairs: the level grid's MaterialPairs in order (M2a weapons; see grid_pairs.h and
// docs/design/m2a-weapons.md §4.3).
#include "bf3/core/gameplay/grid_pairs.h"

#include "bf3/core/gameplay/work_budget.h"

#include <string>

namespace bf3::core::gameplay {

namespace {

// The index field of a soup material value is 24 bits wide (level::kMaterialPairMask).
constexpr size_t kMaxGridPairs = size_t(1) << 24;

} // namespace

Result<std::vector<MaterialPairRef>> read_grid_pairs(const EbxResolver &p_db, std::string_view p_grid, const MaterialGrid &p_material_grid) {
	const EbxPartition *gp = p_db.partition_by_name(p_grid);
	if (gp == nullptr) {
		return make_error(std::string(p_grid) + ": partition not found");
	}
	const std::vector<EbxInstance> grids = gp->instances_of("MaterialGridData");
	if (grids.empty()) {
		return make_error(std::string(p_grid) + ": no MaterialGridData");
	}
	const std::optional<EbxValue> list = grids.front().get("MaterialPairs");
	const OptionalEbxArray pairs = list ? list->as_array() : OptionalEbxArray();
	if (!pairs) {
		return make_error(std::string(p_grid) + ": MaterialGridData.MaterialPairs missing or not an array");
	}
	if (pairs->size() > kMaxGridPairs) {
		return make_error(std::string(p_grid) + ": MaterialPairs has " + std::to_string(pairs->size()) + " entries (limit 2^24, the index field's range)");
	}
	// One unit per element scanned (the gameplay readers' cost model, work_budget.h): the default budget covers 30
	// retail grids (32,400 pairs); a hostile array of millions of entries ends with the budget error instead.
	WorkBudget budget;
	std::vector<MaterialPairRef> out;
	out.reserve(pairs->size());
	for (const EbxValue v : *pairs) {
		if (Status s = budget.charge(1); !s.ok()) {
			return s.error();
		}
		MaterialPairRef ref;
		const std::optional<EbxRef> r = v.as_ref();
		if (r && !r->is_null()) {
			ref.present = true;
			ref.partition = r->partition;
			ref.instance = r->instance;
			if (r->kind == EbxRef::Kind::External) {
				if (const std::optional<MaterialPairRef> p = p_material_grid.pair(r->instance)) {
					ref = *p;
					ref.present = true;
				}
			}
		}
		out.push_back(ref);
	}
	return out;
}

} // namespace bf3::core::gameplay
