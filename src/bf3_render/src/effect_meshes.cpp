#include "bf3/render/effect_meshes.h"

#include "bf3/core/util/elapsed.h"
#include "bf3/render/mesh_cache.h"
#include "bf3/render/slot_textures.h"

#include <chrono>
#include <utility>

namespace bf3::render {

const EffectMesh *EffectMeshSet::mesh(std::string_view p_name) const {
	for (const EffectMesh &m : meshes) {
		if (m.name == p_name) {
			return &m;
		}
	}
	return nullptr;
}

const PreparedTexture *EffectMeshSet::texture(const std::string &p_name) const {
	for (const PreparedTexture &t : textures) {
		if (t.name == p_name) {
			return &t;
		}
	}
	return nullptr;
}

std::unique_ptr<EffectMeshSet> load_effect_meshes(const core::EbxResolver &p_db, const core::LevelMount &p_mount,
		std::span<const std::string> p_names, core::Cache *p_cache) {
	const auto start = std::chrono::steady_clock::now();
	auto set = std::make_unique<EffectMeshSet>();
	const std::vector<std::string> databases = variation_database_names(p_mount);
	std::unique_ptr<LookResolver> look = LookResolver::open(p_mount);
	if (!look) {
		set->notes.push_back("no level shader database: effect mesh slots keep only their own textures");
	}
	for (const std::string &name : p_names) {
		core::Result<MeshMaterials> materials = mesh_materials(p_db, name, 0, databases);
		if (!materials) {
			set->notes.push_back(name + ": " + materials.error().message);
			++set->failed;
			continue;
		}
		MeshRequest request;
		// STAND-IN: LOD 0 at every distance. Settle: the meshes' LOD distances read.
		request.lods = { 0 };
		request.filter = core::res::SubsetFilter::Drawn;
		core::Result<CachedMesh> mesh = load_mesh_cached(p_cache, p_mount, name, request);
		if (!mesh || mesh->mesh.meshes.size() != 1) {
			set->notes.push_back(name + ": " + (mesh ? std::string("no LOD 0 prepared") : mesh.error().message));
			++set->failed;
			continue;
		}
		EffectMesh m;
		m.name = name;
		m.materials = std::move(*materials);
		m.looks.resize(m.materials.slots.size());
		for (size_t i = 0; look && i < m.looks.size(); ++i) {
			m.looks[i] = look->resolve(m.materials.slots[i], name);
			MaterialSlot &slot = m.materials.slots[i];
			for (const auto &[parameter, texture] :
					{ std::pair{ "Diffuse", &m.looks[i].color }, std::pair{ "Normal", &m.looks[i].normal } }) {
				if (slot.texture(parameter).empty() && texture->has_value()) {
					slot.textures.emplace_back(parameter, (*texture)->name);
					++set->shader_textures;
				}
			}
		}
		m.prepared = std::move(mesh->mesh.meshes[0]);
		m.cache = mesh->item;
		set->meshes.push_back(std::move(m));
	}
	SlotTextureWants wants;
	for (const EffectMesh &m : set->meshes) {
		wants.add(m.materials, m.prepared.surfaces);
	}
	set->textures = load_slot_textures(p_mount, wants, "effect mesh", set->notes);
	set->seconds = core::seconds_since(start);
	return set;
}

}
