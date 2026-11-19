#pragma once

#include "bf3/render/look_materials.h"
#include "bf3/render/material_lookup.h"

#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/texture2d.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace bf3::godot_side {

class VehicleLooks {
public:
	using FindTexture = std::function<godot::Ref<godot::Texture2D>(const std::string &)>;

	explicit VehicleLooks(FindTexture p_find);

	godot::Ref<godot::Material> material(
			const render::LookMaterial &p_look, const render::MaterialSlot &p_slot, bool p_has_tangents);

	uint32_t program_materials() const { return program_materials_; }
	uint32_t look_materials() const { return look_materials_; }

private:
	godot::Ref<godot::Shader> look_shader(int p_alpha_mode, bool p_double_sided);
	godot::Ref<godot::Shader> program_shader(const render::GraphProgram &p_program);
	godot::Ref<godot::Texture2D> zero_texture();

	FindTexture find_;
	std::map<int, godot::Ref<godot::Shader>> look_shaders_;
	std::map<const render::GraphProgram *, godot::Ref<godot::Shader>> program_shaders_;
	godot::Ref<godot::Texture2D> zero_;
	uint32_t program_materials_ = 0;
	uint32_t look_materials_ = 0;
};

}
