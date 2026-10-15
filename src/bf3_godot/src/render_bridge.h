#pragma once
// Godot side of the render bridge: resources from bf3_render's prepared (Godot-free) data. Main thread only
// (plan §2.3: resource creation that touches RenderingServer runs here; decoding ran on loader threads).
// Generic: the level renderer reuses it for rigid meshes (no skin) and their textures.
//
// Space: Frostbite and Godot are both Y up and metric; transforms map 1:1 (columns = right, up, forward).

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

// An ImageTexture holding the prepared mip chain (no re-encode). Null on failure (logged).
godot::Ref<godot::ImageTexture> make_texture(const bf3::render::PreparedTexture &p_texture);

// One surface per prepared surface, in order; p_material(i) supplies surface i's material (may be null).
godot::Ref<godot::ArrayMesh> make_array_mesh(const bf3::render::PreparedMesh &p_mesh,
		const std::function<godot::Ref<godot::Material>(const bf3::render::PreparedSurface &)> &p_material);

// A Skeleton3D with the skeleton's bones (names, parents, rest); not yet in the tree.
godot::Skeleton3D *make_skeleton(const bf3::anim::Skeleton &p_skeleton);
// A Skin with one bind per SkinBind (bone index + inverse bind), shared by every mesh skinned against it.
godot::Ref<godot::Skin> make_skin(const std::vector<bf3::anim::SkinBind> &p_binds);

struct MaterialOptions {
	// 1P viewmodel (plan §2.5 "1p weapon"): own vertical FOV and a compressed depth range so the weapon draws
	// over nearby world geometry.
	bool viewmodel = false;
	float fov_override = 55.0f;
	float z_clip_scale = 0.25f;
};

// A StandardMaterial3D for one material slot: Diffuse -> albedo (sRGB), Normal -> normal map (tangent X/Y in
// R/G, see bf3_render texture_prep.h); DiffuseDarkening tints the albedo, SmoothnessRegular sets roughness.
// p_find_texture maps a texture res name to its uploaded texture (null when missing).
godot::Ref<godot::StandardMaterial3D> make_standard_material(const bf3::render::MaterialSlot &p_slot, bool p_has_tangents,
		const std::function<godot::Ref<godot::Texture2D>(const std::string &)> &p_find_texture, const MaterialOptions &p_options);

} // namespace bf3::godot_side
