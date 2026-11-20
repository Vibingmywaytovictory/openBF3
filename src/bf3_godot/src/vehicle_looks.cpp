#include "vehicle_looks.h"

#include "godot_util.h"
#include "level_upload.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include <algorithm>
#include <utility>

using namespace godot;

namespace bf3::godot_side {

VehicleLooks::VehicleLooks(FindTexture p_find) : find_(std::move(p_find)) {
	register_light_pass_globals();
}

Ref<Shader> VehicleLooks::look_shader(int p_alpha_mode, bool p_double_sided) {
	const int key = p_alpha_mode * 2 + (p_double_sided ? 1 : 0);
	Ref<Shader> &shader = look_shaders_[key];
	if (shader.is_null()) {
		shader.instantiate();
		shader->set_code(to_godot(look_shader_code(p_alpha_mode, p_double_sided)));
	}
	return shader;
}

Ref<Shader> VehicleLooks::program_shader(const render::GraphProgram &p_program) {
	Ref<Shader> &shader = program_shaders_[&p_program];
	if (shader.is_null()) {
		shader.instantiate();
		shader->set_code(to_godot(p_program.translated.code));
	}
	return shader;
}

Ref<Texture2D> VehicleLooks::zero_texture() {
	if (zero_.is_null()) {
		PackedByteArray bytes;
		bytes.resize(4);
		bytes.fill(0);
		zero_ = ImageTexture::create_from_image(Image::create_from_data(1, 1, false, Image::FORMAT_RGBA8, bytes));
	}
	return zero_;
}

Ref<Material> VehicleLooks::material(
		const render::LookMaterial &p_look, const render::MaterialSlot &p_slot, bool p_has_tangents) {
	if (p_look.blend == render::LookBlend::Blend && !p_look.glass) {
		return Ref<Material>();
	}
	Ref<ShaderMaterial> m;
	m.instantiate();
	const Ref<Texture2D> albedo = p_look.color && !p_look.glass ? find_(p_look.color->name) : Ref<Texture2D>();
	const Ref<Texture2D> normal = p_look.normal && p_has_tangents ? find_(p_look.normal->name) : Ref<Texture2D>();
	if (p_look.graph) {
		const render::TranslatedProgram &program = p_look.graph->translated;
		m->set_shader(program_shader(*p_look.graph));
		for (size_t t = 0; t < program.textures.size() && t < p_look.graph_textures.size(); ++t) {
			const std::string &name = p_look.graph_textures[t];
			const Ref<Texture2D> texture = name.empty() ? zero_texture() : find_(name);
			m->set_shader_parameter(to_godot("t" + std::to_string(program.textures[t])), texture);
		}
		for (size_t c = 0; c < program.constants.size() && c < p_look.graph_constants.size(); ++c) {
			const std::array<float, 4> &v = p_look.graph_constants[c];
			m->set_shader_parameter(to_godot(program.constants[c].uniform), Vector4(v[0], v[1], v[2], v[3]));
		}
		++program_materials_;
	} else {
		const int mode = p_look.glass ? 2 : (p_look.blend == render::LookBlend::AlphaTest ? 1 : 0);
		m->set_shader(look_shader(mode, p_look.double_sided));
		m->set_shader_parameter("has_albedo", albedo.is_valid());
		if (albedo.is_valid()) {
			m->set_shader_parameter("albedo_tex", albedo);
			m->set_shader_parameter("albedo_uv", look_uv(p_look.color->channel));
			m->set_shader_parameter("albedo_scale", p_look.color->scale);
		}
		if (p_look.glass) {
			m->set_shader_parameter("glass_opacity", render::kLookGlassOpacity);
		}
		++look_materials_;
	}
	const bool flat = !p_look.graph && p_look.flat_colour;
	set_look_params(m->get_rid(), p_slot, albedo.is_valid(), p_look.glass, flat ? p_look.flat_colour : std::nullopt,
			flat && p_look.flat_unlit);
	m->set_shader_parameter("has_normal", normal.is_valid());
	if (normal.is_valid()) {
		m->set_shader_parameter("normal_tex", normal);
		m->set_shader_parameter("normal_uv", look_uv(p_look.normal->channel));
		m->set_shader_parameter("normal_scale", p_look.normal->scale);
	}
	return m;
}
}
