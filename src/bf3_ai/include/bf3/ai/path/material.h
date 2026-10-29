#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace bf3::ai {

using MaterialId = uint8_t;

inline constexpr MaterialId kMaterialInvalid = 0;
inline constexpr MaterialId kMaterialGround = 1;
inline constexpr MaterialId kMaterialRoad = 2;
inline constexpr MaterialId kMaterialShallows = 3;
inline constexpr MaterialId kMaterialDeepWater = 4;
inline constexpr MaterialId kMaterialDangerObject = 5;
inline constexpr MaterialId kMaterialOtherDangerObject = 6;
inline constexpr MaterialId kMaterialBothDangerObject = 7;
inline constexpr MaterialId kMaterialCount = 9;

std::optional<MaterialId> parse_material(std::string_view p_token);

class MaterialManager {
public:
	MaterialManager();

	bool is_valid_material(MaterialId p_material) const;
	void set_material_cost(MaterialId p_material, float p_cost);
	float material_cost(MaterialId p_material) const;

private:
	std::array<float, kMaterialCount> costs_{};
};

}
