#include "bf3/core/gameplay/grid_pairs.h"

#include "bf3/core/gameplay/work_budget.h"

#include <string>

namespace bf3::core::gameplay {

namespace {

constexpr size_t kMaxGridPairs = size_t(1) << 24;

}

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

}
