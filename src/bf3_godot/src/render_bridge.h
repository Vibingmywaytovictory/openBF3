#pragma once

#include "bf3/anim/pose_math.h"
#include "bf3/anim/skeleton.h"
#include "bf3/anim/skinning.h"
#include "bf3/render/material_lookup.h"
#include "bf3/render/mesh_prep.h"
#include "bf3/render/texture_prep.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/skin.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <functional>
#include <string>
#include <vector>

namespace godot {
class Skeleton3D;
}

namespace bf3::godot_side {

godot::Transform3D to_godot(const bf3::anim::Xform &p_x);
godot::Transform3D to_godot(const bf3::anim::Mat34 &p_m);
godot::Quaternion to_godot(const bf3::anim::Quat &p_q);
godot::Vector3 to_godot(const bf3::anim::Vec3 &p_v);

godot::Ref<godot::ImageTexture> make_texture(const bf3::render::PreparedTexture &p_texture);

godot::Ref<godot::ArrayMesh> make_array_mesh(const bf3::render::PreparedMesh &p_mesh,
		const std::function<godot::Ref<godot::Material>(const bf3::render::PreparedSurface &)> &p_material);

godot::Skeleton3D *make_skeleton(const bf3::anim::Skeleton &p_skeleton);

godot::Ref<godot::Skin> make_skin(const std::vector<bf3::anim::SkinBind> &p_binds);

struct MaterialOptions {

	bool viewmodel = false;
	float fov_override = 55.0f;
	float z_clip_scale = 0.25f;
};

godot::Ref<godot::StandardMaterial3D> make_standard_material(const bf3::render::MaterialSlot &p_slot, bool p_has_tangents,
		const std::function<godot::Ref<godot::Texture2D>(const std::string &)> &p_find_texture, const MaterialOptions &p_options);

}
