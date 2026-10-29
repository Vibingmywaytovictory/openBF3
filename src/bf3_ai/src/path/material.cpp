#include "bf3/ai/path/material.h"

#include "bf3/core/util/text.h"

#include <utility>

namespace bf3::ai {

namespace {

constexpr std::pair<std::string_view, MaterialId> kMaterialNames[] = {
	{ "Invalid", kMaterialInvalid },
	{ "Ground", kMaterialGround },
	{ "Road", kMaterialRoad },
	{ "Shallows", kMaterialShallows },
	{ "DeepWater", kMaterialDeepWater },
	{ "DangerObject", kMaterialDangerObject },
	{ "OtherDangerObject", kMaterialOtherDangerObject },
	{ "BothDangerObject", kMaterialBothDangerObject },
	{ "NumberOfMaterials", kMaterialCount },
};

constexpr float kDefaultCost = 1.0f;
constexpr float kDangerCost = 1000.0f;

}

std::optional<MaterialId> parse_material(std::string_view p_token) {
	for (const auto &[name, id] : kMaterialNames) {
		if (core::iequals_ascii(p_token, name)) {
			return id;
		}
	}
	return std::nullopt;
}

MaterialManager::MaterialManager() {
	costs_.fill(kDefaultCost);
	set_material_cost(kMaterialInvalid, -1.0f);
	set_material_cost(kMaterialDangerObject, kDangerCost);
	set_material_cost(kMaterialBothDangerObject, kDangerCost);
}

bool MaterialManager::is_valid_material(MaterialId p_material) const {
	return p_material < kMaterialCount && costs_[p_material] >= 0.0f;
}

void MaterialManager::set_material_cost(MaterialId p_material, float p_cost) {
	if (p_material < kMaterialCount) {
		costs_[p_material] = p_cost;
	}
}

float MaterialManager::material_cost(MaterialId p_material) const {
	return p_material < kMaterialCount ? costs_[p_material] : -1.0f;
}

}
