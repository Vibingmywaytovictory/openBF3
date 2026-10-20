#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::render {

struct MaterialSlot {
	std::string shader;
	std::vector<std::pair<std::string, std::string>> textures;
	std::vector<std::pair<std::string, std::array<float, 4>>> vectors;
	bool from_variation_db = false;

	std::string variation_shader;

	std::string texture(std::string_view p_parameter) const;

	const std::array<float, 4> *vector(std::string_view p_parameter) const;
};

struct MeshMaterials {
	std::string mesh;
	uint32_t variation_hash = 0;
	std::string database;
	std::vector<MaterialSlot> slots;
};

std::vector<std::string> variation_database_names(const core::EbxLibrary &p_library);

core::Result<MeshMaterials> mesh_materials(const core::EbxLibrary &p_library, std::string_view p_mesh_partition, uint32_t p_variation_hash,
		const std::vector<std::string> &p_databases);

inline constexpr size_t kMaxMaterialSlots = 4096;
inline constexpr size_t kMaxTextureParameters = 256;
core::Result<MeshMaterials> mesh_materials(const core::EbxResolver &p_db, const core::EbxInstance &p_asset, const std::optional<core::EbxInstance> &p_entry);

}
