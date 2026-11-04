#include "bf3/core/gameplay/material_sound.h"

#include "ebx_fields.h"

#include <cmath>
#include <string>

namespace bf3::core::gameplay {

using detail::Fields;

namespace {

constexpr size_t kMaxMaterialRows = 1024;
constexpr size_t kMaxIndexMap = 256;

std::optional<float> row_sound_id(Fields &r_f, const EbxStruct &p_pair) {
	const OptionalEbxArray props = r_f.array(p_pair, "PhysicsMaterialProperties");
	if (!props) {
		return std::nullopt;
	}
	for (const EbxValue v : *props) {
		const std::optional<EbxRef> r = v.as_ref();
		if (!r || r->is_null()) {
			continue;
		}
		const std::optional<EbxInstance> p =
				r_f.follow_ref(p_pair.partition(), *r, "", r_f.where(p_pair, "PhysicsMaterialProperties"));
		if (!p) {
			return std::nullopt;
		}
		if (p->is_a("MaterialPropertySoundData")) {
			const float id = r_f.f32(p->data(), "MaterialSoundId");
			if (r_f.ok() && !std::isfinite(id)) {
				r_f.fail(r_f.where(p->data(), "MaterialSoundId") + ": not finite");
			}
			return id;
		}
	}
	return std::nullopt;
}

}

std::optional<float> MaterialSounds::sound_id(uint8_t p_physics_material) const {
	return p_physics_material < by_material.size() ? by_material[p_physics_material] : std::nullopt;
}

Result<MaterialSounds> read_material_sounds(const EbxResolver &p_db, std::string_view p_grid) {
	Fields f(p_db);
	const EbxPartition *gp = p_db.partition_by_name(p_grid);
	if (gp == nullptr) {
		return make_error(std::string(p_grid) + ": partition not found");
	}
	const std::vector<EbxInstance> grids = gp->instances_of("MaterialGridData");
	if (grids.empty()) {
		return make_error(std::string(p_grid) + ": no MaterialGridData");
	}
	const EbxStruct g = grids.front().data();
	const OptionalEbxArray map = f.array(g, "MaterialIndexMap");
	const OptionalEbxArray rows = f.array(g, "MaterialProperties");
	if (!f.ok()) {
		return f.error();
	}
	if (rows->size() > kMaxMaterialRows || map->size() > kMaxIndexMap) {
		return make_error(std::string(p_grid) + ": " + std::to_string(rows->size()) + " MaterialProperties and " +
				std::to_string(map->size()) + " MaterialIndexMap entries (limits " + std::to_string(kMaxMaterialRows) +
				", " + std::to_string(kMaxIndexMap) + ")");
	}
	MaterialSounds out;
	out.rows = rows->size();
	std::vector<std::optional<float>> by_row(out.rows);
	for (size_t r = 0; r < out.rows; ++r) {
		const std::optional<EbxStruct> pair = rows->get(r)->as_struct();
		if (!pair) {
			return make_error(std::string(p_grid) + ": MaterialProperties[" + std::to_string(r) + "] is not a struct");
		}
		by_row[r] = row_sound_id(f, *pair);
		if (!f.ok()) {
			return f.error();
		}
		out.rows_with_sound += by_row[r] ? 1u : 0u;
	}
	out.by_material.reserve(map->size());
	for (const EbxValue v : *map) {
		const int64_t row = v.as_i64().value_or(-1);
		out.by_material.push_back(row >= 0 && uint64_t(row) < out.rows ? by_row[size_t(row)] : std::nullopt);
	}
	return out;
}

}
