#include "bf3/render/deployed_gadgets.h"

#include "bf3/core/res/res_loader.h"
#include "bf3/core/thread_pool.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/render/mesh_cache.h"

#include <chrono>
#include <set>
#include <utility>

namespace bf3::render {

const DeployedGadgetPart *DeployedGadgetSet::part(uint16_t p_def) const {
	for (const DeployedGadgetPart &p : parts) {
		if (p.def == p_def) {
			return &p;
		}
	}
	return nullptr;
}

const PreparedTexture *DeployedGadgetSet::texture(const std::string &p_name) const {
	for (const PreparedTexture &t : textures) {
		if (t.name == p_name) {
			return &t;
		}
	}
	return nullptr;
}

core::Result<std::unique_ptr<DeployedGadgetSet>> load_deployed_gadgets(const core::EbxResolver &p_db,
		const core::LevelMount &p_mount, std::span<const DeployedGadgetSpec> p_specs, core::Cache *p_cache) {
	const auto start = std::chrono::steady_clock::now();
	auto set = std::make_unique<DeployedGadgetSet>();
	const std::vector<std::string> databases = variation_database_names(p_mount);
	const core::LevelMount &mount = p_mount;
	for (const DeployedGadgetSpec &spec : p_specs) {
		const std::string what = "deployed gadget def " + std::to_string(spec.def);
		core::Result<core::gameplay::DeployedObject> object = core::gameplay::read_fired_object(p_db, spec.firing_data);
		if (!object) {
			return core::error_at(what, object.error());
		}
		if (object->mesh.empty()) {
			return core::make_error(what + ": " + object->projectile + " has no Mesh");
		}
		DeployedGadgetPart part;
		part.def = spec.def;
		core::Result<MeshMaterials> materials = mesh_materials(p_db, object->mesh, 0, databases);
		if (!materials) {
			return core::error_at(what + ": " + object->mesh, materials.error());
		}
		part.materials = std::move(*materials);
		MeshRequest request;
		request.lods = { 0 };
		request.filter = core::res::SubsetFilter::Drawn;
		core::Result<CachedMesh> mesh = load_mesh_cached(p_cache, mount, object->mesh, request);
		if (!mesh) {
			return core::error_at(what + ": " + object->mesh, mesh.error());
		}
		if (mesh->mesh.meshes.size() != 1) {
			return core::make_error(what + ": " + object->mesh + ": no LOD 0 prepared");
		}
		part.prepared = std::move(mesh->mesh.meshes[0]);
		part.cache = mesh->item;
		part.object = std::move(*object);
		set->parts.push_back(std::move(part));
	}
	std::vector<std::pair<std::string, TextureRole>> wanted;
	std::set<std::string> seen;
	for (const DeployedGadgetPart &part : set->parts) {
		for (const PreparedSurface &s : part.prepared.surfaces) {
			if (s.material_index >= part.materials.slots.size()) {
				continue;
			}
			const MaterialSlot &slot = part.materials.slots[s.material_index];
			for (auto [param, role] :
					{ std::pair{ "Diffuse", TextureRole::Color }, std::pair{ "Normal", TextureRole::Normal } }) {
				const std::string name = slot.texture(param);
				if (!name.empty() && seen.insert(name).second) {
					wanted.emplace_back(name, role);
				}
			}
		}
	}
	std::vector<std::string> errors(wanted.size());
	std::vector<PreparedTexture> textures(wanted.size());
	core::parallel_for(wanted.size(), [&](size_t i) {
		core::Result<core::res::LoadedTexture> tex = core::res::load_texture(mount, wanted[i].first);
		if (!tex) {
			errors[i] = tex.error().message;
			return;
		}
		core::Result<PreparedTexture> p = prepare_texture(*tex, wanted[i].second);
		if (!p) {
			errors[i] = p.error().message;
			return;
		}
		textures[i] = std::move(*p);
	});
	for (size_t i = 0; i < wanted.size(); ++i) {
		if (!errors[i].empty()) {
			set->notes.push_back("deployed gadget texture skipped: " + errors[i]);
			continue;
		}
		set->textures.push_back(std::move(textures[i]));
	}
	set->seconds = core::seconds_since(start);
	return set;
}

}
