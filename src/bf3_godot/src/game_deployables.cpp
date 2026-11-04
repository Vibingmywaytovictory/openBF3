#include "game_deployables.h"

#include "render_bridge.h"

#include "bf3/host/gunplay.h"
#include "bf3/host/level_load.h"

#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <iterator>
#include <set>

using namespace godot;

namespace bf3::godot_side {

GameDeployables::~GameDeployables() {
	if (root_ == nullptr) {
		return;
	}
	if (Node3D *root = Object::cast_to<Node3D>(ObjectDB::get_instance(root_id_))) {
		if (root->is_inside_tree()) {
			root->queue_free();
		} else {
			memdelete(root);
		}
	}
}

core::Status GameDeployables::load(const GameLoadInputs &p_in) {
	if (p_in.db == nullptr || p_in.db->level == nullptr || p_in.shared == nullptr || p_in.shared->gunplay == nullptr) {
		return core::make_error("deployables: no level database or gunplay tables");
	}
	const host::GunplayLevel &gunplay = *p_in.shared->gunplay;
	std::vector<render::DeployedGadgetSpec> specs;
	for (const sim::weapons::GadgetDef &g : gunplay.gadgets) {
		if (g.def < gunplay.defs.size()) {
			specs.push_back(render::DeployedGadgetSpec{ g.def, gunplay.defs[g.def].firing_data });
		}
	}
	core::Result<std::unique_ptr<render::DeployedGadgetSet>> set =
			render::load_deployed_gadgets(p_in.db->level->db(), p_in.db->level->mount(), specs, p_in.cache);
	if (!set) {
		return set.error();
	}
	set_ = std::move(*set);
	stats_.parts = uint32_t(set_->parts.size());
	return core::Status::success();
}

bool GameDeployables::build(Node3D *p_parent, std::string *r_error) {
	if (set_ == nullptr || p_parent == nullptr) {
		*r_error = "deployables: not loaded";
		return false;
	}
	std::map<std::string, Ref<Texture2D>> textures;
	const auto find_texture = [&](const std::string &p_name) -> Ref<Texture2D> {
		const auto it = textures.find(p_name);
		if (it != textures.end()) {
			return it->second;
		}
		const render::PreparedTexture *t = set_->texture(p_name);
		Ref<Texture2D> tex;
		if (t != nullptr) {
			tex = make_texture(*t);
		}
		textures[p_name] = tex;
		return tex;
	};
	for (const render::DeployedGadgetPart &part : set_->parts) {
		std::vector<Ref<Material>> materials;
		for (const render::PreparedSurface &s : part.prepared.surfaces) {
			Ref<Material> m;
			if (s.material_index < part.materials.slots.size()) {
				m = make_standard_material(
						part.materials.slots[s.material_index], !s.tangents.empty(), find_texture, MaterialOptions{});
			}
			materials.push_back(m);
		}
		const render::PreparedSurface *first = part.prepared.surfaces.data();
		Ref<ArrayMesh> mesh = make_array_mesh(part.prepared, [&](const render::PreparedSurface &p_s) -> Ref<Material> {
			const size_t i = size_t(&p_s - first);
			return i < materials.size() ? materials[i] : Ref<Material>();
		});
		if (mesh.is_null()) {
			*r_error = "deployables: no mesh made for " + part.object.mesh;
			return false;
		}
		meshes_[part.def] = mesh;
	}
	root_ = memnew(Node3D);
	root_->set_name("Deployables");
	p_parent->add_child(root_);
	root_id_ = root_->get_instance_id();
	return true;
}

void GameDeployables::step(const host::PresentationFrame &p_frame) {
	if (root_ == nullptr || ObjectDB::get_instance(root_id_) == nullptr) {
		return;
	}
	std::set<uint16_t> live;
	stats_.unknown_defs = 0;
	for (size_t i = 0; i < p_frame.deployable_count && i < p_frame.deployables.size(); ++i) {
		const net::DeployableWire &d = p_frame.deployables[i];
		const auto mesh = meshes_.find(d.def);
		if (mesh == meshes_.end()) {
			++stats_.unknown_defs;
			continue;
		}
		live.insert(d.id);
		Shown &shown = shown_[d.id];
		MeshInstance3D *node = Object::cast_to<MeshInstance3D>(ObjectDB::get_instance(shown.node_id));
		if (node == nullptr) {
			node = memnew(MeshInstance3D);
			node->set_mesh(mesh->second);
			root_->add_child(node);
			shown.node_id = node->get_instance_id();
			shown.def = d.def;
			++stats_.created;
		} else if (shown.def != d.def) {
			node->set_mesh(mesh->second);
			shown.def = d.def;
		}
		node->set_position(Vector3(d.position[0], d.position[1], d.position[2]));
	}
	for (auto it = shown_.begin(); it != shown_.end();) {
		if (live.count(it->first) != 0) {
			++it;
			continue;
		}
		if (Node *node = Object::cast_to<Node>(ObjectDB::get_instance(it->second.node_id))) {
			node->queue_free();
		}
		++stats_.freed;
		it = shown_.erase(it);
	}
	stats_.shown = uint32_t(shown_.size());
}

}
