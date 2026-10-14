#include "bf3/render/material_lookup.h"

#include "bf3/core/ebx.h"

#include <algorithm>
#include <cctype>
#include <optional>

namespace bf3::render {

using core::EbxInstance;
using core::EbxLibrary;
using core::EbxStruct;
using core::EbxValue;

namespace {

bool iequals(std::string_view p_a, std::string_view p_b) {
	return p_a.size() == p_b.size() &&
			std::equal(p_a.begin(), p_a.end(), p_b.begin(), [](char a, char b) { return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b)); });
}

std::string partition_name(const EbxLibrary &p_library, const EbxInstance &p_instance) {
	const std::optional<std::string_view> n = p_library.name_of(p_instance.partition().guid());
	return n ? std::string(*n) : std::string();
}

// TextureParameters [{ParameterName, Value -> TextureAsset}] of a struct (MeshVariationDatabaseMaterial or a
// SurfaceShaderInstanceDataStruct).
std::vector<std::pair<std::string, std::string>> texture_parameters(const EbxLibrary &p_library, const EbxStruct &p_owner) {
	std::vector<std::pair<std::string, std::string>> out;
	const std::optional<EbxValue> params = p_owner.get("TextureParameters");
	if (!params || !params->as_array()) {
		return out;
	}
	for (EbxValue v : *params->as_array()) {
		const std::optional<EbxStruct> p = v.as_struct();
		if (!p) {
			continue;
		}
		const std::optional<EbxValue> name = p->get("ParameterName");
		const std::optional<EbxInstance> texture = p_library.follow(*p, "Value");
		if (!name || !texture) {
			continue;
		}
		const std::string texture_name = partition_name(p_library, *texture);
		if (!texture_name.empty()) {
			out.emplace_back(std::string(name->as_str().value_or("")), texture_name);
		}
	}
	return out;
}

} // namespace

std::string MaterialSlot::texture(std::string_view p_parameter) const {
	for (const auto &[name, texture] : textures) {
		if (iequals(name, p_parameter)) {
			return texture;
		}
	}
	return {};
}

const std::array<float, 4> *MaterialSlot::vector(std::string_view p_parameter) const {
	for (const auto &[name, value] : vectors) {
		if (iequals(name, p_parameter)) {
			return &value;
		}
	}
	return nullptr;
}

std::vector<std::string> variation_database_names(const EbxLibrary &p_library) {
	std::vector<std::string> out;
	if (p_library.superbundle_count() == 0) {
		return out;
	}
	for (const std::string &name : p_library.mount(0).ebx_names()) {
		std::string lower = name;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) { return char(std::tolower(uint8_t(c))); });
		if (lower.find("meshvariationdb") != std::string::npos) {
			out.push_back(std::move(lower));
		}
	}
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

core::Result<MeshMaterials> mesh_materials(const EbxLibrary &p_library, std::string_view p_mesh_partition, uint32_t p_variation_hash,
		const std::vector<std::string> &p_databases) {
	const std::string where = "materials of " + std::string(p_mesh_partition);
	const core::EbxPartition *mesh = p_library.partition_by_name(p_mesh_partition);
	if (mesh == nullptr) {
		return core::make_error(where + ": no such partition");
	}
	const EbxInstance asset = mesh->primary();
	if (!asset.is_a("MeshAsset")) {
		return core::make_error(where + ": primary instance is " + std::string(asset.type_name()));
	}
	MeshMaterials out;
	out.mesh = std::string(p_mesh_partition);
	const std::vector<EbxInstance> materials = p_library.follow_all(asset.data(), "Materials");
	out.slots.resize(materials.size());
	for (size_t i = 0; i < materials.size(); ++i) {
		MaterialSlot &slot = out.slots[i];
		const std::optional<EbxValue> shader_value = materials[i].get("Shader");
		const std::optional<EbxStruct> shader = shader_value ? shader_value->as_struct() : std::nullopt;
		if (!shader) {
			continue;
		}
		if (const std::optional<EbxInstance> graph = p_library.follow(*shader, "Shader")) {
			slot.shader = partition_name(p_library, *graph);
		}
		if (const std::optional<EbxValue> vectors = shader->get("VectorParameters"); vectors && vectors->as_array()) {
			for (EbxValue v : *vectors->as_array()) {
				const std::optional<EbxStruct> p = v.as_struct();
				if (!p) {
					continue;
				}
				const std::optional<EbxValue> name = p->get("ParameterName");
				const std::optional<EbxValue> value = p->get("Value");
				if (!name || !value) {
					continue;
				}
				std::array<float, 4> f{};
				const char *axes[4] = { "x", "y", "z", "w" };
				for (int a = 0; a < 4; ++a) {
					const std::optional<EbxValue> c = value->get(axes[a]);
					f[size_t(a)] = c ? c->as_f32().value_or(0.0f) : 0.0f;
				}
				slot.vectors.emplace_back(std::string(name->as_str().value_or("")), f);
			}
		}
		slot.textures = texture_parameters(p_library, *shader);
	}

	// The MeshVariationDatabase entry: exact variation first, then the default (hash 0).
	std::optional<EbxInstance> entry;
	for (int pass = 0; pass < 2 && !entry; ++pass) {
		const uint32_t want = pass == 0 ? p_variation_hash : 0u;
		if (pass == 1 && p_variation_hash == 0) {
			break;
		}
		for (const std::string &db_name : p_databases) {
			const core::EbxPartition *db = p_library.partition_by_name(db_name);
			if (db == nullptr) {
				continue;
			}
			for (const EbxInstance &e : db->instances_of("MeshVariationDatabaseEntry")) {
				const std::optional<EbxInstance> m = p_library.follow(e.data(), "Mesh");
				const std::optional<EbxValue> hash = e.get("VariationAssetNameHash");
				if (m && &m->partition() == mesh && hash && hash->as_u32().value_or(~0u) == want) {
					entry = e;
					out.variation_hash = want;
					out.database = db_name;
					break;
				}
			}
			if (entry) {
				break;
			}
		}
	}
	if (entry) {
		const std::optional<EbxValue> list = entry->get("Materials");
		std::vector<EbxStruct> variations;
		if (list && list->as_array()) {
			for (EbxValue v : *list->as_array()) {
				if (const std::optional<EbxStruct> s = v.as_struct()) {
					variations.push_back(*s);
				}
			}
		}
		for (size_t i = 0; i < out.slots.size(); ++i) {
			const EbxStruct *chosen = nullptr;
			for (const EbxStruct &v : variations) {
				const std::optional<EbxInstance> m = p_library.follow(v, "Material");
				if (m && *m == materials[i]) {
					chosen = &v;
					break;
				}
			}
			if (chosen == nullptr && i < variations.size()) {
				chosen = &variations[i];
			}
			if (chosen != nullptr) {
				std::vector<std::pair<std::string, std::string>> textures = texture_parameters(p_library, *chosen);
				if (!textures.empty()) {
					out.slots[i].textures = std::move(textures);
					out.slots[i].from_variation_db = true;
				}
			}
		}
	}
	return out;
}

} // namespace bf3::render
