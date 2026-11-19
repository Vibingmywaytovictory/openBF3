#include "fx_meshes.h"

#include "render_bridge.h"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/texture2d.hpp>

#include <cstring>

namespace bf3::godot_side {

using namespace godot;

namespace {

constexpr uint32_t kFxMeshMinCapacity = 16;
constexpr size_t kFxMeshTransformFloats = 12;

uint32_t grow_to(uint32_t p_needed) {
	uint32_t c = kFxMeshMinCapacity;
	while (c < p_needed && c < (1u << 24)) {
		c <<= 1;
	}
	return c;
}

}

FxMeshes::~FxMeshes() {
	clear();
}

void FxMeshes::load(std::unique_ptr<render::EffectMeshSet> p_set) {
	set_ = std::move(p_set);
	stats_ = FxMeshStats{};
	notes_.clear();
	if (set_ != nullptr) {
		stats_.failed = uint32_t(set_->failed);
		notes_ = set_->notes;
	}
}

bool FxMeshes::build(RID p_scenario, std::string *r_error) {
	clear();
	if (set_ == nullptr) {
		return true;
	}
	scenario_ = p_scenario;
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
	RenderingServer *rs = RenderingServer::get_singleton();
	for (const render::EffectMesh &m : set_->meshes) {
		std::vector<Ref<Material>> world;
		Drawn d;
		MaterialOptions fp;
		fp.viewmodel = true;
		fp.fov_override = fp_fov_;
		fp.z_clip_scale = fp_z_clip_;
		for (const render::PreparedSurface &s : m.prepared.surfaces) {
			Ref<StandardMaterial3D> w, f;
			if (s.material_index < m.materials.slots.size()) {
				w = make_standard_material(
						m.materials.slots[s.material_index], !s.tangents.empty(), find_texture, MaterialOptions{});
				f = make_standard_material(m.materials.slots[s.material_index], !s.tangents.empty(), find_texture, fp);
			}
			world.push_back(w);
			d.first_person.push_back(f);
		}
		const render::PreparedSurface *first = m.prepared.surfaces.data();
		d.mesh = make_array_mesh(m.prepared, [&](const render::PreparedSurface &p_s) -> Ref<Material> {
			const size_t i = size_t(&p_s - first);
			return i < world.size() ? world[i] : Ref<Material>();
		});
		d.first_person_mesh = make_array_mesh(m.prepared, [&](const render::PreparedSurface &p_s) -> Ref<Material> {
			const size_t i = size_t(&p_s - first);
			return i < d.first_person.size() ? Ref<Material>(d.first_person[i]) : Ref<Material>();
		});
		if (d.mesh.is_null() || d.first_person_mesh.is_null()) {
			*r_error = "effect meshes: no mesh made for " + m.name;
			return false;
		}
		for (size_t p = 0; p < 2; ++p) {
			Pass &pass = d.passes[p];
			pass.multimesh = rs->multimesh_create();
			rs->multimesh_set_mesh(pass.multimesh, (p == 1 ? d.first_person_mesh : d.mesh)->get_rid());
			pass.capacity = kFxMeshMinCapacity;
			rs->multimesh_allocate_data(
					pass.multimesh, int32_t(pass.capacity), RenderingServer::MULTIMESH_TRANSFORM_3D);
			rs->multimesh_set_visible_instances(pass.multimesh, 0);
			pass.instance = rs->instance_create2(pass.multimesh, scenario_);
			rs->instance_set_visible(pass.instance, false);
			if (p == 1) {
				rs->instance_geometry_set_cast_shadows_setting(
						pass.instance, RenderingServer::SHADOW_CASTING_SETTING_OFF);
			}
		}
		drawn_.emplace(m.name, std::move(d));
	}
	stats_.meshes = uint32_t(drawn_.size());
	return true;
}

void FxMeshes::set_first_person_projection(float p_fov_override, float p_z_clip_scale) {
	fp_fov_ = p_fov_override;
	fp_z_clip_ = p_z_clip_scale;
	for (auto &[name, d] : drawn_) {
		for (const Ref<StandardMaterial3D> &m : d.first_person) {
			if (m.is_valid()) {
				m->set_fov_override(fp_fov_);
				m->set_z_clip_scale(fp_z_clip_);
			}
		}
	}
}

void FxMeshes::upload(Pass &r_pass) {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (r_pass.count == 0) {
		if (r_pass.shown) {
			rs->instance_set_visible(r_pass.instance, false);
			rs->multimesh_set_visible_instances(r_pass.multimesh, 0);
			r_pass.shown = false;
		}
		return;
	}
	if (r_pass.count > r_pass.capacity) {
		r_pass.capacity = grow_to(r_pass.count);
		rs->multimesh_allocate_data(
				r_pass.multimesh, int32_t(r_pass.capacity), RenderingServer::MULTIMESH_TRANSFORM_3D);
	}
	PackedFloat32Array full;
	full.resize(int64_t(r_pass.capacity) * int64_t(kFxMeshTransformFloats));
	std::memcpy(full.ptrw(), r_pass.buffer.ptr(), size_t(r_pass.count) * kFxMeshTransformFloats * sizeof(float));
	rs->multimesh_set_buffer(r_pass.multimesh, full);
	rs->multimesh_set_visible_instances(r_pass.multimesh, int32_t(r_pass.count));
	if (!r_pass.shown) {
		rs->instance_set_visible(r_pass.instance, true);
		r_pass.shown = true;
	}
}

void FxMeshes::draw(const fx::RenderPrep &p_prep) {
	for (auto &[name, d] : drawn_) {
		for (Pass &p : d.passes) {
			p.count = 0;
		}
	}
	stats_.drawn = 0;
	stats_.unknown = 0;
	const std::span<const fx::FxInstance> instances = p_prep.instances();
	for (const fx::FxBatch &b : p_prep.batches()) {
		if (b.key.shader != fx::FxShaderClass::Mesh) {
			continue;
		}
		const size_t pass = b.key.pass == fx::FxPass::FirstPerson ? 1 : 0;
		for (uint32_t i = 0; i < b.instance_count; ++i) {
			const fx::FxInstance &in = instances[b.first_instance + i];
			const std::string *name = p_prep.row_mesh(uint32_t(in.custom[0]));
			const auto it = name != nullptr ? drawn_.find(*name) : drawn_.end();
			if (it == drawn_.end()) {
				++stats_.unknown;
				continue;
			}
			Pass &p = it->second.passes[pass];
			if (int64_t(p.count + 1) * int64_t(kFxMeshTransformFloats) > p.buffer.size()) {
				p.buffer.resize(int64_t(grow_to(p.count + 1)) * int64_t(kFxMeshTransformFloats));
			}
			std::memcpy(p.buffer.ptrw() + size_t(p.count) * kFxMeshTransformFloats, in.transform,
					kFxMeshTransformFloats * sizeof(float));
			++p.count;
			++stats_.drawn;
		}
	}
	for (auto &[name, d] : drawn_) {
		for (Pass &p : d.passes) {
			upload(p);
		}
	}
	stats_.drawn_total += stats_.drawn;
	stats_.unknown_total += stats_.unknown;
}

void FxMeshes::hide() {
	RenderingServer *rs = RenderingServer::get_singleton();
	for (auto &[name, d] : drawn_) {
		for (Pass &p : d.passes) {
			if (p.shown) {
				rs->instance_set_visible(p.instance, false);
				p.shown = false;
			}
			p.count = 0;
		}
	}
	stats_.drawn = 0;
}

void FxMeshes::clear() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs != nullptr) {
		for (auto &[name, d] : drawn_) {
			for (Pass &p : d.passes) {
				if (p.instance.is_valid()) {
					rs->free_rid(p.instance);
				}
				if (p.multimesh.is_valid()) {
					rs->free_rid(p.multimesh);
				}
			}
		}
	}
	drawn_.clear();
}

}
