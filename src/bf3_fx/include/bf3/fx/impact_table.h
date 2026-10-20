#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::fx {

struct ImpactCell {
	std::vector<std::string> effects;
	std::vector<std::string> debris;
	float max_spread_degrees = 0;
	bool mirror_impact_direction = false;
	bool has_effect_relation = false;
	std::string decal;
	std::string impact_sound;
	bool empty() const { return effects.empty() && debris.empty() && decal.empty() && impact_sound.empty(); }
};

class ImpactTable {
public:

	static core::Result<ImpactTable> load(const core::EbxResolver &p_db, std::string_view p_grid, core::gameplay::WorkBudget &r_budget,
			std::span<const core::EbxResolver *const> p_fallbacks = {});

	size_t rows() const { return rows_; }
	size_t effect_relations() const { return effect_relations_; }
	size_t decal_relations() const { return decal_relations_; }

	const ImpactCell *select(uint8_t p_projectile_material, uint8_t p_surface_material) const;

	std::optional<uint8_t> surface_material(uint32_t p_triangle_material) const;

	std::optional<uint8_t> material_by_name(std::string_view p_name) const;
	std::string_view material_name(uint8_t p_index) const;

	std::span<const int64_t> index_map() const { return index_map_; }
	int64_t default_material_index() const { return default_material_index_; }

	const ImpactCell &cell(size_t p_row, size_t p_col) const { return unique_[cells_[p_row * rows_ + p_col]]; }

	size_t distinct_cells() const { return unique_.size(); }

private:
	size_t rows_ = 0;
	size_t effect_relations_ = 0;
	size_t decal_relations_ = 0;
	int64_t default_material_index_ = 0;
	std::vector<int64_t> index_map_;
	std::vector<uint32_t> cells_;
	std::vector<ImpactCell> unique_;
	std::vector<std::optional<uint8_t>> pair_materials_;
	std::vector<std::string> names_;
};

}
