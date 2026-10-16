// Impact selection from the level's material grid (impact_table.h; docs/design/m2a-vfx1.md §6.10, §5.3): every cell's
// PhysicsMaterialProperties (effect, decal and sound relations), the MaterialIndexMap, the MaterialPairs and the
// container's names.
#include "bf3/fx/impact_table.h"

#include "fx_ebx.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

namespace bf3::fx {

namespace {

constexpr size_t kMaxRows = 256;
constexpr size_t kMaxIndexMap = 4096;
constexpr size_t kMaxImpactEffects = 64;
constexpr size_t kMaxPairs = 1u << 20;
constexpr size_t kMaxRelationsPerCell = 64;

// A strict weak order over every ImpactCell field: the content key that deduplicates cells in O(log U) comparisons
// each (U = distinct cells so far), so a hostile grid of distinct cells costs O(n^2 log n) for its n^2 cells, never
// a scan of every earlier cell per cell. max_spread_degrees is finite (Reader::number), so < is a total order on it.
struct CellLess {
	bool operator()(const ImpactCell &p_a, const ImpactCell &p_b) const {
		return std::tie(p_a.effects, p_a.debris, p_a.max_spread_degrees, p_a.mirror_impact_direction, p_a.has_effect_relation, p_a.decal, p_a.impact_sound) <
				std::tie(p_b.effects, p_b.debris, p_b.max_spread_degrees, p_b.mirror_impact_direction, p_b.has_effect_relation, p_b.decal, p_b.impact_sound);
	}
};

// The work units one key comparison of p_cell pays for: one plus one per kBytesPerUnit bytes of its strings.
size_t cell_key_units(const ImpactCell &p_cell) {
	size_t bytes = p_cell.decal.size() + p_cell.impact_sound.size();
	for (const std::string &e : p_cell.effects) {
		bytes += e.size();
	}
	for (const std::string &d : p_cell.debris) {
		bytes += d.size();
	}
	return 1 + bytes / core::gameplay::WorkBudget::kBytesPerUnit;
}

// MaterialContainerPair instances of a container partition: instance GUID -> PhysicsMaterialIndex.
void add_pairs(detail::Reader &r_r, const core::EbxPartition &p_container, std::unordered_map<core::Guid, uint8_t, core::GuidHash> &r_pairs) {
	for (const core::EbxInstance &inst : p_container.instances_of("MaterialContainerPair")) {
		if (!r_r.charge()) {
			return;
		}
		const core::Guid id = inst.guid();
		if (r_pairs.contains(id)) {
			continue;
		}
		const std::optional<core::EbxValue> material = inst.get("PhysicsMaterialIndex");
		const std::optional<int64_t> m = material ? material->as_i64() : std::nullopt;
		if (!m) {
			continue;
		}
		r_pairs.emplace(id, uint8_t(*m & 0xFF));
	}
}

} // namespace

core::Result<ImpactTable> ImpactTable::load(const core::EbxResolver &p_db, std::string_view p_grid, core::gameplay::WorkBudget &r_budget,
		std::span<const core::EbxResolver *const> p_fallbacks) {
	detail::Reader r(p_db, r_budget);
	const std::string grid_name = detail::canonical_name(p_grid);
	r.set_where(grid_name);
	const core::EbxPartition *gp = p_db.partition_by_name(grid_name);
	if (gp == nullptr) {
		return core::make_error(grid_name + ": partition not found");
	}
	const std::vector<core::EbxInstance> grids = gp->instances_of("MaterialGridData");
	if (grids.empty()) {
		return core::make_error(grid_name + ": no MaterialGridData");
	}
	const core::EbxStruct g = grids.front().data();
	ImpactTable out;
	if (const core::OptionalEbxArray map = r.array(g, "MaterialIndexMap", kMaxIndexMap)) {
		out.index_map_.reserve(map->size());
		for (const core::EbxValue v : *map) {
			out.index_map_.push_back(v.as_i64().value_or(-1));
		}
	}
	out.default_material_index_ = r.integer_or(g, "DefaultMaterialIndex", 0, 255, 0);
	const core::OptionalEbxArray rows = r.array(g, "InteractionGrid", kMaxRows);
	if (!r.ok()) {
		return r.error();
	}
	const size_t n = rows->size();
	std::vector<core::EbxArray> items;
	items.reserve(n);
	for (size_t row = 0; row < n; ++row) {
		const std::optional<core::EbxStruct> rs = rows->get(row)->as_struct();
		if (!rs) {
			return core::make_error(grid_name + ": InteractionGrid[" + std::to_string(row) + "] is not a struct");
		}
		const core::OptionalEbxArray it = r.array(*rs, "Items", kMaxRows);
		if (!r.ok()) {
			return r.error();
		}
		if (it->size() != n) {
			return core::make_error(grid_name + ": InteractionGrid[" + std::to_string(row) + "] has " + std::to_string(it->size()) + " items, expected " +
					std::to_string(n));
		}
		items.push_back(*it);
	}
	out.rows_ = n;
	out.cells_.assign(n * n, 0);
	out.unique_.emplace_back(); // [0]: the empty cell
	std::set<uint32_t> effect_relations;
	std::set<uint32_t> decal_relations;
	// Relation instances are shared between cells: read each once.
	std::map<std::vector<uint32_t>, uint32_t> by_relations;
	// Distinct cells by content (different relation instances can hold the same content): cell -> index in unique_.
	std::map<ImpactCell, uint32_t, CellLess> by_content;
	for (size_t row = 0; row < n; ++row) {
		for (size_t col = 0; col < n; ++col) {
			if (!r.charge()) {
				return r.error();
			}
			const std::optional<core::EbxStruct> item = items[row].get(col)->as_struct();
			if (!item) {
				return core::make_error(grid_name + ": cell (" + std::to_string(row) + ", " + std::to_string(col) + ") is not a struct");
			}
			const core::OptionalEbxArray props = r.array(*item, "PhysicsMaterialProperties", kMaxRelationsPerCell);
			if (!r.ok()) {
				return r.error();
			}
			std::vector<uint32_t> rel;
			for (const core::EbxValue v : *props) {
				const std::optional<core::EbxRef> ref = v.as_ref();
				if (!ref || ref->is_null()) {
					continue;
				}
				if (ref->kind != core::EbxRef::Kind::Internal) {
					return core::make_error(grid_name + ": PhysicsMaterialProperties holds an external reference");
				}
				rel.push_back(ref->index);
			}
			if (!r.charge(1 + rel.size())) { // the by_relations lookup compares relation lists
				return r.error();
			}
			const auto known = by_relations.find(rel);
			if (known != by_relations.end()) {
				out.cells_[row * n + col] = known->second;
				for (const uint32_t i : rel) {
					const std::optional<core::EbxInstance> inst = gp->instance(i);
					if (inst && inst->type_name() == "MaterialRelationEffectData") {
						effect_relations.insert(i);
					} else if (inst && inst->type_name() == "MaterialRelationDecalData") {
						decal_relations.insert(i);
					}
				}
				continue;
			}
			ImpactCell cell;
			for (const uint32_t i : rel) {
				if (!r.charge()) {
					return r.error();
				}
				const std::optional<core::EbxInstance> inst = gp->instance(i);
				if (!inst) {
					return core::make_error(grid_name + ": unresolved relation reference");
				}
				const core::EbxStruct s = inst->data();
				const std::string_view type = inst->type_name();
				if (type == "MaterialRelationEffectData") {
					effect_relations.insert(i);
					cell.has_effect_relation = true;
					cell.max_spread_degrees = r.number(s, "ImpactEffectMaxSpreadAngle");
					cell.mirror_impact_direction = r.flag(s, "MirrorImpactDirection");
					if (const core::OptionalEbxArray fx = r.array(s, "ImpactEffects", kMaxImpactEffects)) {
						for (const core::EbxValue e : *fx) {
							const std::optional<core::EbxRef> er = e.as_ref();
							if (er && !er->is_null()) {
								cell.effects.push_back(r.external_name_of(s, *er, "ImpactEffects"));
							}
						}
					}
					if (const core::OptionalEbxArray debris = r.array(s, "ImpactDebris", kMaxImpactEffects)) {
						for (const core::EbxValue e : *debris) {
							const std::optional<core::EbxRef> er = e.as_ref();
							if (er && !er->is_null()) {
								cell.debris.push_back(r.external_name_of(s, *er, "ImpactDebris"));
							}
						}
					}
				} else if (type == "MaterialRelationDecalData") {
					decal_relations.insert(i);
					const std::string decal = r.external_name(s, "Decal");
					if (!decal.empty()) {
						cell.decal = decal;
					}
				} else if (type == "MaterialRelationSoundData") {
					const std::string sound = r.external_name(s, "ImpactSound");
					if (!sound.empty()) {
						cell.impact_sound = sound;
					}
				}
				if (!r.ok()) {
					return r.error();
				}
			}
			uint32_t at = 0;
			if (!cell.empty() || cell.has_effect_relation) {
				if (!r.charge(cell_key_units(cell))) {
					return r.error();
				}
				const uint32_t next = uint32_t(out.unique_.size());
				const auto [it, inserted] = by_content.try_emplace(cell, next);
				if (inserted) {
					out.unique_.push_back(std::move(cell));
				}
				at = it->second;
			}
			by_relations.emplace(std::move(rel), at);
			out.cells_[row * n + col] = at;
		}
	}
	out.effect_relations_ = effect_relations.size();
	out.decal_relations_ = decal_relations.size();

	// The container (names, pairs) through DefaultMaterial; the delta levels' fallback rule of MaterialGrid::load.
	const std::optional<core::EbxRef> def = r.ref(g, "DefaultMaterial");
	const core::OptionalEbxArray grid_pairs = r.array(g, "MaterialPairs", kMaxPairs);
	if (!r.ok()) {
		return r.error();
	}
	std::unordered_map<core::Guid, uint8_t, core::GuidHash> pairs;
	if (def && def->kind == core::EbxRef::Kind::External) {
		if (const core::EbxPartition *cp = p_db.partition(def->partition)) {
			add_pairs(r, *cp, pairs);
			if (const std::optional<core::EbxValue> names = cp->primary().get("MaterialNames")) {
				if (const core::OptionalEbxArray list = names->as_array(); list && list->size() <= kMaxIndexMap) {
					for (const core::EbxValue nv : *list) {
						const std::optional<std::string_view> s = nv.as_str();
						out.names_.emplace_back(s ? *s : std::string_view());
					}
				}
			}
		}
		for (const core::EbxResolver *fb : p_fallbacks) {
			if (fb == nullptr) {
				continue;
			}
			if (const core::EbxPartition *cp = fb->partition(def->partition)) {
				add_pairs(r, *cp, pairs);
			}
		}
	}
	if (!r.ok()) {
		return r.error();
	}
	out.pair_materials_.reserve(grid_pairs->size());
	for (const core::EbxValue v : *grid_pairs) {
		const std::optional<core::EbxRef> ref = v.as_ref();
		std::optional<uint8_t> m;
		if (ref && ref->kind == core::EbxRef::Kind::External) {
			const auto it = pairs.find(ref->instance);
			if (it != pairs.end()) {
				m = it->second;
			}
		}
		out.pair_materials_.push_back(m);
	}
	return out;
}

const ImpactCell *ImpactTable::select(uint8_t p_projectile_material, uint8_t p_surface_material) const {
	if (p_projectile_material >= index_map_.size() || p_surface_material >= index_map_.size()) {
		return nullptr;
	}
	const int64_t a = index_map_[p_projectile_material];
	const int64_t b = index_map_[p_surface_material];
	if (a < 0 || b < 0 || uint64_t(a) >= rows_ || uint64_t(b) >= rows_) {
		return nullptr;
	}
	return &unique_[cells_[size_t(a) * rows_ + size_t(b)]];
}

std::optional<uint8_t> ImpactTable::surface_material(uint32_t p_triangle_material) const {
	const uint32_t i = p_triangle_material & 0xFFFFFFu;
	if (i >= pair_materials_.size()) {
		return std::nullopt;
	}
	return pair_materials_[i];
}

std::optional<uint8_t> ImpactTable::material_by_name(std::string_view p_name) const {
	for (size_t i = 0; i < names_.size() && i < 256; ++i) {
		if (names_[i] == p_name) {
			return uint8_t(i);
		}
	}
	return std::nullopt;
}

std::string_view ImpactTable::material_name(uint8_t p_index) const {
	return p_index < names_.size() ? std::string_view(names_[p_index]) : std::string_view();
}

} // namespace bf3::fx
