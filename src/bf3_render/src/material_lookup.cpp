#include "bf3/render/material_lookup.h"

#include "bf3/core/ebx.h"

#include <algorithm>
#include <cctype>
#include <optional>

namespace bf3::render {

using core::EbxInstance;
using core::EbxLibrary;
using core::EbxResolver;
using core::EbxStruct;
using core::EbxValue;

namespace {

bool iequals(std::string_view p_a, std::string_view p_b) {
	return p_a.size() == p_b.size() &&
			std::equal(p_a.begin(), p_a.end(), p_b.begin(), [](char a, char b) { return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b)); });
}

std::string partition_name(const EbxResolver &p_db, const EbxInstance &p_instance) {
	const std::optional<std::string_view> n = p_db.name_of(p_instance.partition().guid());
	return n ? std::string(*n) : std::string();
}

std::vector<std::pair<std::string, std::string>> texture_parameters(const EbxResolver &p_db, const EbxStruct &p_owner) {
	std::vector<std::pair<std::string, std::string>> out;
	const std::optional<EbxValue> params = p_owner.get("TextureParameters");
	if (!params || !params->as_array()) {
		return out;
	}
	size_t seen = 0;
	for (EbxValue v : *params->as_array()) {
		if (++seen > kMaxTextureParameters) {
			break;
		}
		const std::optional<EbxStruct> p = v.as_struct();
		if (!p) {
			continue;
		}
		const std::optional<EbxValue> name = p->get("ParameterName");
		const std::optional<EbxInstance> texture = p_db.follow(*p, "Value");
		if (!name || !texture) {
			continue;
		}
		const std::string texture_name = partition_name(p_db, *texture);
		if (!texture_name.empty()) {
			out.emplace_back(std::string(name->as_str().value_or("")), texture_name);
		}
	}
	return out;
}

core::Result<MeshMaterials> materials_of(const EbxResolver &p_db, const EbxInstance &p_asset, const std::optional<EbxInstance> &p_entry,
		const std::string &p_where) {
	if (!p_asset.is_a("MeshAsset")) {
		return core::make_error(p_where + ": primary instance is " + std::string(p_asset.type_name()));
	}
	MeshMaterials out;
	out.mesh = partition_name(p_db, p_asset);
	const std::vector<EbxInstance> materials = p_db.follow_all(p_asset.data(), "Materials");
	if (materials.size() > kMaxMaterialSlots) {
		return core::make_error(p_where + ": " + std::to_string(materials.size()) + " materials (more than " + std::to_string(kMaxMaterialSlots) + ")");
	}
	out.slots.resize(materials.size());
	for (size_t i = 0; i < materials.size(); ++i) {
		MaterialSlot &slot = out.slots[i];
		const std::optional<EbxValue> shader_value = materials[i].get("Shader");
		const std::optional<EbxStruct> shader = shader_value ? shader_value->as_struct() : std::nullopt;
		if (!shader) {
			continue;
		}
		if (const std::optional<EbxInstance> graph = p_db.follow(*shader, "Shader")) {
			slot.shader = partition_name(p_db, *graph);
		}
		if (const std::optional<EbxValue> vectors = shader->get("VectorParameters"); vectors && vectors->as_array()) {
			size_t seen = 0;
			for (EbxValue v : *vectors->as_array()) {
				if (++seen > kMaxTextureParameters) {
					break;
				}
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
		slot.textures = texture_parameters(p_db, *shader);
	}
	if (!p_entry) {
		return out;
	}
	out.database = partition_name(p_db, *p_entry);
	if (const std::optional<EbxValue> hash = p_entry->get("VariationAssetNameHash")) {
		out.variation_hash = hash->as_u32().value_or(0);
	}
	const std::optional<EbxValue> list = p_entry->get("Materials");
	std::vector<EbxStruct> variations;
	if (list && list->as_array()) {
		for (EbxValue v : *list->as_array()) {
			if (variations.size() >= kMaxMaterialSlots) {
				break;
			}
			if (const std::optional<EbxStruct> s = v.as_struct()) {
				variations.push_back(*s);
			}
		}
	}

	std::vector<std::optional<EbxInstance>> named(variations.size());
	for (size_t v = 0; v < variations.size(); ++v) {
		named[v] = p_db.follow(variations[v], "Material");
	}
	for (size_t i = 0; i < out.slots.size(); ++i) {
		const EbxStruct *chosen = nullptr;
		for (size_t v = 0; v < variations.size(); ++v) {
			if (named[v] && *named[v] == materials[i]) {
				chosen = &variations[v];
				break;
			}
		}
		if (chosen == nullptr && i < variations.size()) {
			chosen = &variations[i];
		}
		if (chosen != nullptr) {

			if (const std::optional<EbxInstance> variation = p_db.follow(*chosen, "MaterialVariation")) {
				const std::optional<EbxValue> vshader_value = variation->get("Shader");
				if (const std::optional<EbxStruct> vshader = vshader_value ? vshader_value->as_struct() : std::nullopt) {
					if (const std::optional<EbxInstance> graph = p_db.follow(*vshader, "Shader")) {
						out.slots[i].variation_shader = partition_name(p_db, *graph);
					}
				}
			}
			std::vector<std::pair<std::string, std::string>> textures = texture_parameters(p_db, *chosen);
			if (!textures.empty()) {
				out.slots[i].textures = std::move(textures);
				out.slots[i].from_variation_db = true;
			}
		}
	}
	return out;
}

}

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
					break;
				}
			}
			if (entry) {
				break;
			}
		}
	}
	core::Result<MeshMaterials> out = materials_of(p_library, asset, entry, where);
	if (!out) {
		return out.error();
	}
	out->mesh = std::string(p_mesh_partition);
	return out;
}

core::Result<MeshMaterials> mesh_materials(const EbxResolver &p_db, const EbxInstance &p_asset, const std::optional<EbxInstance> &p_entry) {
	return materials_of(p_db, p_asset, p_entry, "materials of " + partition_name(p_db, p_asset));
}

}
