#pragma once
// Impact selection from the level's material grid (docs/design/m2a-vfx1.md §6.10; plan §3.14 "Impacts", §7.10
// item 5; gap VFX §3). A data lookup only: which EffectBlueprints, decal and spread a projectile material gives on a
// surface material. Starting the effect, spreading its direction and placing the decal are VFX3.
//
// Lookup [verified on the Metro and Caspian grids, 2026-10-15]: effects, decals and impact sounds live in a cell's
// PhysicsMaterialProperties (damage, penetration and terrain destruction in PhysicsPropertyProperties, which
// bf3::core::gameplay::MaterialGrid reads), so
//     row = MaterialIndexMap[projectile.PhysicsMaterialIndex & 0xFF]
//     col = MaterialIndexMap[surface.PhysicsMaterialIndex & 0xFF]
//     cell = InteractionGrid[row].Items[col].PhysicsMaterialProperties
//         -> MaterialRelationEffectData {ImpactEffects[] (EffectBlueprint), ImpactDebris[], ImpactEffectMaxSpreadAngle,
//            MirrorImpactDirection} + MaterialRelationDecalData {Decal -> DecalTemplateData}
//            + MaterialRelationSoundData {ImpactSound, ScrapeSound, ScrapeLength} (for the audio track, A5).
// A surface triangle's material value (collision_soup.h, MaterialFlagsAndIndices & 0xFFFFFF) indexes
// MaterialGridData.MaterialPairs; that pair's PhysicsMaterialIndex is the surface index (surface_material()).
// The 5.56 bullet (`weapons/common/bullets/556x45mmnato`, pair 5b6b84ac) has PhysicsMaterialIndex 1 ("Bullet").
// Metro: 102 rows, 223 effect and 110 decal relations, 16 distinct 5.56 impact effects; Caspian (MP_007): 118 rows,
// 299 / 123, 16. An empty cell selects nothing (no fallback to DefaultMaterialIndex in VFX1; docs §6.10).

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
	std::vector<std::string> effects; // ImpactEffects[] EffectBlueprint partitions, lowercase, data order
	std::vector<std::string> debris; // ImpactDebris[] ObjectBlueprint partitions
	float max_spread_degrees = 0; // ImpactEffectMaxSpreadAngle
	bool mirror_impact_direction = false;
	bool has_effect_relation = false;
	std::string decal; // DecalTemplateData partition (empty: none)
	std::string impact_sound; // SoundPatchAsset partition (empty: none)
	bool empty() const { return effects.empty() && debris.empty() && decal.empty() && impact_sound.empty(); }
};

class ImpactTable {
public:
	// p_grid: the level's MaterialGridData partition (bf3::core::gameplay::MaterialGrid::grid_of_level). The
	// container pairs come through the grid's MaterialPairs references (the delta levels' fallback rule of
	// MaterialGrid::load applies: p_fallbacks are searched for pairs the level's container copy lacks).
	static core::Result<ImpactTable> load(const core::EbxResolver &p_db, std::string_view p_grid, core::gameplay::WorkBudget &r_budget,
			std::span<const core::EbxResolver *const> p_fallbacks = {});

	size_t rows() const { return rows_; }
	size_t effect_relations() const { return effect_relations_; }
	size_t decal_relations() const { return decal_relations_; }
	// The cell for (projectile PhysicsMaterialIndex, surface PhysicsMaterialIndex); nullptr when either index maps to
	// no row (an empty cell is returned as an empty ImpactCell, not nullptr).
	const ImpactCell *select(uint8_t p_projectile_material, uint8_t p_surface_material) const;
	// A triangle material value -> the PhysicsMaterialIndex of its MaterialPairs entry (nullopt: out of range or
	// unresolved pair).
	std::optional<uint8_t> surface_material(uint32_t p_triangle_material) const;
	// MaterialContainerAsset.MaterialNames: the first index whose name is p_name ("Concrete" -> 8); names repeat
	// ("Default", "Grenade", ...), so a name is a convenience for tests and bf3dump, never a key.
	std::optional<uint8_t> material_by_name(std::string_view p_name) const;
	std::string_view material_name(uint8_t p_index) const;
	// MaterialIndexMap (material index -> grid row) and DefaultMaterialIndex, for the counting rules of docs §5.3.
	std::span<const int64_t> index_map() const { return index_map_; }
	int64_t default_material_index() const { return default_material_index_; }
	// The cell of grid row p_row, column p_col (both < rows()).
	const ImpactCell &cell(size_t p_row, size_t p_col) const { return unique_[cells_[p_row * rows_ + p_col]]; }
	// Distinct cell contents, the empty cell included (fx.fuzz checks the content deduplication with it).
	size_t distinct_cells() const { return unique_.size(); }

private:
	size_t rows_ = 0;
	size_t effect_relations_ = 0;
	size_t decal_relations_ = 0;
	int64_t default_material_index_ = 0;
	std::vector<int64_t> index_map_; // MaterialIndexMap
	std::vector<uint32_t> cells_; // rows_ * rows_ indices into unique_
	std::vector<ImpactCell> unique_; // distinct cells; [0] is the empty cell
	std::vector<std::optional<uint8_t>> pair_materials_; // MaterialPairs -> PhysicsMaterialIndex
	std::vector<std::string> names_;
};

} // namespace bf3::fx
