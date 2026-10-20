#include "render_bridge.h"

#include "godot_util.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cstring>

namespace bf3::godot_side {

using namespace godot;
using bf3::render::GpuFormat;
using bf3::render::PreparedSurface;

Vector3 to_godot(const bf3::anim::Vec3 &p_v) {
	return Vector3(p_v.x, p_v.y, p_v.z);
}

Quaternion to_godot(const bf3::anim::Quat &p_q) {
	return Quaternion(p_q.x, p_q.y, p_q.z, p_q.w);
}

Transform3D to_godot(const bf3::anim::Xform &p_x) {
	return Transform3D(Basis(to_godot(p_x.q)), to_godot(p_x.t));
}

Transform3D to_godot(const bf3::anim::Mat34 &p_m) {
	const auto &m = p_m.m;

	return Transform3D(Basis(m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2]), Vector3(m[0][3], m[1][3], m[2][3]));
}

namespace {

Image::Format image_format(GpuFormat p_format) {
	switch (p_format) {
		case GpuFormat::Bc1:
			return Image::FORMAT_DXT1;
		case GpuFormat::Bc2:
			return Image::FORMAT_DXT3;
		case GpuFormat::Bc3:
			return Image::FORMAT_DXT5;
		case GpuFormat::Bc4:
			return Image::FORMAT_RGTC_R;
		case GpuFormat::Bc5:
			return Image::FORMAT_RGTC_RG;
		case GpuFormat::Rgba8:
			return Image::FORMAT_RGBA8;
		case GpuFormat::L8:
			return Image::FORMAT_L8;
		case GpuFormat::Rg8:
			return Image::FORMAT_RG8;
	}
	return Image::FORMAT_RGBA8;
}

PackedVector3Array vec3s(const std::vector<float> &p_xyz) {
	PackedVector3Array a;
	a.resize(int64_t(p_xyz.size() / 3));
	Vector3 *w = a.ptrw();
	for (size_t i = 0; i < p_xyz.size() / 3; ++i) {
		w[i] = Vector3(p_xyz[i * 3], p_xyz[i * 3 + 1], p_xyz[i * 3 + 2]);
	}
	return a;
}

PackedVector2Array vec2s(const std::vector<float> &p_xy) {
	PackedVector2Array a;
	a.resize(int64_t(p_xy.size() / 2));
	Vector2 *w = a.ptrw();
	for (size_t i = 0; i < p_xy.size() / 2; ++i) {
		w[i] = Vector2(p_xy[i * 2], p_xy[i * 2 + 1]);
	}
	return a;
}

PackedFloat32Array floats(const std::vector<float> &p_values) {
	PackedFloat32Array a;
	a.resize(int64_t(p_values.size()));
	if (!p_values.empty()) {
		std::memcpy(a.ptrw(), p_values.data(), p_values.size() * sizeof(float));
	}
	return a;
}

PackedInt32Array ints(const std::vector<int32_t> &p_values) {
	PackedInt32Array a;
	a.resize(int64_t(p_values.size()));
	if (!p_values.empty()) {
		std::memcpy(a.ptrw(), p_values.data(), p_values.size() * sizeof(int32_t));
	}
	return a;
}

}

Ref<ImageTexture> make_texture(const bf3::render::PreparedTexture &p_texture) {
	PackedByteArray bytes;
	bytes.resize(int64_t(p_texture.data.size()));
	std::memcpy(bytes.ptrw(), p_texture.data.data(), p_texture.data.size());
	const Ref<Image> image = Image::create_from_data(int32_t(p_texture.width), int32_t(p_texture.height), p_texture.levels > 1, image_format(p_texture.format), bytes);
	if (image.is_null() || image->is_empty()) {
		UtilityFunctions::push_error(to_godot("[openbf3] texture " + p_texture.name + ": Image::create_from_data refused the prepared chain"));
		return Ref<ImageTexture>();
	}
	if (uint32_t(image->get_mipmap_count() + 1) != p_texture.levels) {
		UtilityFunctions::push_warning(to_godot("[openbf3] texture " + p_texture.name + ": " + std::to_string(image->get_mipmap_count() + 1) + " levels, prepared " +
				std::to_string(p_texture.levels)));
	}
	return ImageTexture::create_from_image(image);
}

Ref<ArrayMesh> make_array_mesh(const bf3::render::PreparedMesh &p_mesh, const std::function<Ref<Material>(const PreparedSurface &)> &p_material) {
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	for (const PreparedSurface &s : p_mesh.surfaces) {
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = vec3s(s.positions);
		if (!s.normals.empty()) {
			arrays[Mesh::ARRAY_NORMAL] = vec3s(s.normals);
		}
		if (!s.tangents.empty()) {
			arrays[Mesh::ARRAY_TANGENT] = floats(s.tangents);
		}
		if (!s.uv0.empty()) {
			arrays[Mesh::ARRAY_TEX_UV] = vec2s(s.uv0);
		}
		if (!s.uv1.empty()) {
			arrays[Mesh::ARRAY_TEX_UV2] = vec2s(s.uv1);
		}
		if (!s.colors.empty()) {
			PackedColorArray c;
			c.resize(int64_t(s.colors.size() / 4));
			Color *w = c.ptrw();
			for (size_t i = 0; i < s.colors.size() / 4; ++i) {
				w[i] = Color(s.colors[i * 4] / 255.0f, s.colors[i * 4 + 1] / 255.0f, s.colors[i * 4 + 2] / 255.0f, s.colors[i * 4 + 3] / 255.0f);
			}
			arrays[Mesh::ARRAY_COLOR] = c;
		}
		if (!s.bones.empty()) {
			arrays[Mesh::ARRAY_BONES] = ints(s.bones);
			arrays[Mesh::ARRAY_WEIGHTS] = floats(s.weights);
		}
		arrays[Mesh::ARRAY_INDEX] = ints(s.indices);
		const int32_t index = int32_t(mesh->get_surface_count());
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		if (p_material) {
			const Ref<Material> m = p_material(s);
			if (m.is_valid()) {
				mesh->surface_set_material(index, m);
			}
		}
	}
	return mesh;
}

Skeleton3D *make_skeleton(const bf3::anim::Skeleton &p_skeleton) {
	Skeleton3D *sk = memnew(Skeleton3D);
	for (const bf3::anim::Bone &b : p_skeleton.bones()) {
		sk->add_bone(to_godot(b.name));
	}
	for (size_t i = 0; i < p_skeleton.size(); ++i) {
		const bf3::anim::Bone &b = p_skeleton.bone(i);
		if (b.parent >= 0) {
			sk->set_bone_parent(int32_t(i), b.parent);
		}
		sk->set_bone_rest(int32_t(i), to_godot(b.rest));
		sk->set_bone_pose_position(int32_t(i), to_godot(b.rest.t));
		sk->set_bone_pose_rotation(int32_t(i), to_godot(b.rest.q));
	}
	return sk;
}

Ref<Skin> make_skin(const std::vector<bf3::anim::SkinBind> &p_binds) {
	Ref<Skin> skin;
	skin.instantiate();
	for (const bf3::anim::SkinBind &b : p_binds) {
		skin->add_bind(b.bone, to_godot(b.inverse_bind));
	}
	return skin;
}

Ref<StandardMaterial3D> make_standard_material(const bf3::render::MaterialSlot &p_slot, bool p_has_tangents,
		const std::function<Ref<Texture2D>(const std::string &)> &p_find_texture, const MaterialOptions &p_options) {
	Ref<StandardMaterial3D> m;
	m.instantiate();
	m->set_texture_filter(BaseMaterial3D::TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC);
	const Ref<Texture2D> albedo = p_find_texture(p_slot.texture("Diffuse"));
	if (albedo.is_valid()) {
		m->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, albedo);
	} else {
		m->set_albedo(Color(0.55f, 0.55f, 0.55f));
	}
	if (const std::array<float, 4> *dark = p_slot.vector("DiffuseDarkening")) {
		const Color base = albedo.is_valid() ? Color(1, 1, 1) : m->get_albedo();
		m->set_albedo(Color(base.r * (*dark)[0], base.g * (*dark)[1], base.b * (*dark)[2]));
	}
	const Ref<Texture2D> normal = p_find_texture(p_slot.texture("Normal"));
	if (normal.is_valid() && p_has_tangents) {
		m->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, true);
		m->set_texture(BaseMaterial3D::TEXTURE_NORMAL, normal);
	}
	float roughness = 0.6f;
	if (const std::array<float, 4> *smooth = p_slot.vector("SmoothnessRegular")) {
		roughness = std::clamp(1.0f - (*smooth)[0], 0.05f, 1.0f);
	}
	m->set_roughness(roughness);
	m->set_metallic(0.0f);
	if (p_options.viewmodel) {
		m->set_flag(BaseMaterial3D::FLAG_USE_FOV_OVERRIDE, true);
		m->set_fov_override(p_options.fov_override);
		m->set_flag(BaseMaterial3D::FLAG_USE_Z_CLIP_SCALE, true);
		m->set_z_clip_scale(p_options.z_clip_scale);
	}
	return m;
}

}
