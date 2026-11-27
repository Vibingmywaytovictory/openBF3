#pragma once

#include "bf3/fx/render_prep.h"
#include "bf3/render/effect_meshes.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bf3::godot_side {

struct FxMeshStats {
	uint32_t meshes = 0;
	uint32_t failed = 0;
	uint32_t drawn = 0;
	uint32_t unknown = 0;
	uint64_t drawn_total = 0;
	uint64_t unknown_total = 0;
};

class FxMeshes {
public:
	FxMeshes() = default;
	~FxMeshes();
	FxMeshes(const FxMeshes &) = delete;
	FxMeshes &operator=(const FxMeshes &) = delete;

	void load(std::unique_ptr<render::EffectMeshSet> p_set);
	bool build(godot::RID p_scenario, std::string *r_error);
	bool loaded(const std::string &p_name) const { return set_ != nullptr && set_->mesh(p_name) != nullptr; }
	void set_first_person_projection(float p_fov_override, float p_z_clip_scale);
	void draw(const fx::RenderPrep &p_prep);
	void hide();
	void clear();
	const FxMeshStats &stats() const { return stats_; }
	const std::vector<std::string> &notes() const { return notes_; }

private:
	struct Pass {
		godot::RID multimesh;
		godot::RID instance;
		godot::PackedFloat32Array buffer;
		uint32_t capacity = 0;
		uint32_t count = 0;
		bool shown = false;
	};
	struct Drawn {
		godot::Ref<godot::ArrayMesh> mesh;
		godot::Ref<godot::ArrayMesh> first_person_mesh;
		std::vector<godot::Ref<godot::StandardMaterial3D>> first_person;
		Pass passes[2];
	};

	void upload(Pass &r_pass);

	std::unique_ptr<render::EffectMeshSet> set_;
	std::map<std::string, Drawn, std::less<>> drawn_;
	std::vector<std::string> notes_;
	godot::RID scenario_;
	// STAND-IN: 55 deg and a 0.25 z-clip scale for first-person mesh particles until set_first_person_projection.
	// Settle: start from the held weapon's SoldierWeaponData.RenderFov.
	float fp_fov_ = 55.0f;
	float fp_z_clip_ = 0.25f;
	FxMeshStats stats_;
};

}
