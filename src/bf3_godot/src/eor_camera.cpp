#include "eor_camera.h"

#include "godot_util.h"
#include "level_upload.h"

#include "bf3/render/viewmodel_set.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/spot_light3d.hpp>

#include <cstdio>
#include <string_view>

namespace bf3::godot_side {

using namespace godot;
namespace render = bf3::render;

namespace {

constexpr const char *kBackdropShaderPath = "res://shaders/eor_backdrop.gdshader";

constexpr const char *kConeShader = R"(shader_type spatial;
render_mode unshaded, %s, depth_draw_never, cull_disabled, shadows_disabled;
uniform sampler2D base_tex : source_color, filter_linear_mipmap, repeat_disable;
uniform sampler2D dust_tex : source_color, filter_linear_mipmap, repeat_enable;
uniform vec3 tint = vec3(1.0);
uniform float particle_speed = 0.0;
void fragment() {
	vec3 cone = texture(base_tex, UV).rgb;
	vec3 dust = texture(dust_tex, UV + vec2(TIME * particle_speed)).rgb;
	ALBEDO = cone * tint * (vec3(1.0) + dust);
	ALPHA = %s;
}
)";

std::string layer_uniform(render::LookLayerRole p_role) {
	return std::string(render::look_layer_role_name(p_role)) + "_tex";
}

const render::ShaderValue *look_value(const render::LookMaterial &p_look, std::string_view p_name) {
	for (const render::ShaderValue &v : p_look.values) {
		if (v.name == p_name) {
			return &v;
		}
	}
	return nullptr;
}

}

void EorCameraView::frame(Camera3D *p_camera, const std::optional<core::level::EorCamera> &p_camera_data,
		const core::ui::HudEndOfRound &p_end_of_round) {
	if (p_camera == nullptr) {
		return;
	}
	const bool want = p_camera_data.has_value() && p_end_of_round.stage != core::ui::HudEorStage::None;
	if (want && !active_) {
		player_view_ = p_camera->get_global_transform();
		player_fov_deg_ = float(p_camera->get_fov());
		active_ = true;
		++stats_.taken;
	} else if (!want && active_) {
		p_camera->set_global_transform(player_view_);
		p_camera->set_fov(player_fov_deg_);
		active_ = false;
		++stats_.released;
		return;
	}
	if (!active_) {
		return;
	}
	++stats_.frames;
	p_camera->set_global_transform(instance_transform(p_camera_data->transform));
	p_camera->set_fov(p_camera_data->fov_deg);
}

EorShowroom::EorShowroom() = default;
EorShowroom::~EorShowroom() = default;

void EorShowroom::collect(const render::LevelScenePlan &p_plan) {
	collected_ = true;
	for (uint32_t i = 0; i < p_plan.materials.size(); ++i) {
		const render::SceneMaterialPlan &m = p_plan.materials[i];
		if (!m.look || m.look->program == render::LookProgram::Standard || m.layers.size() != m.look->layers.size()) {
			continue;
		}
		const bool backdrop = m.look->program == render::LookProgram::CharacterBackdrop;
		if (!backdrop && cone_shader_.is_null()) {
			const bool add = m.look->solution_blend &&
					render::viewmodel_blend_of(*m.look->solution_blend) == render::ViewmodelBlend::Add;
			char code[1024];
			std::snprintf(code, sizeof(code), kConeShader, add ? "blend_add" : "blend_mix",
					add ? "1.0" : "max(cone.r, max(cone.g, cone.b))");
			cone_shader_.instantiate();
			cone_shader_->set_code(to_godot(code));
		}
		if (backdrop && backdrop_shader_.is_null()) {
			backdrop_shader_ = ResourceLoader::get_singleton()->load(kBackdropShaderPath);
		}
		materials_.push_back(Material{ i, false, std::vector<bool>(m.layers.size(), false) });
		stats_.backdrop_materials += backdrop ? 1 : 0;
		stats_.cone_materials += backdrop ? 0 : 1;
		stats_.samplers_wanted += m.layers.size();
	}
}

void EorShowroom::bind(const render::LevelScenePlan &p_plan, const LevelUpload &p_upload) {
	RenderingServer *rs = RenderingServer::get_singleton();
	bool complete = true;
	for (Material &m : materials_) {
		const render::SceneMaterialPlan &plan = p_plan.materials[m.index];
		const RID material = p_upload.material_rid(m.index);
		if (!material.is_valid()) {
			complete = false;
			continue;
		}
		if (!m.shaded) {
			const bool backdrop = plan.look->program == render::LookProgram::CharacterBackdrop;
			const Ref<Shader> &shader = backdrop ? backdrop_shader_ : cone_shader_;
			if (shader.is_null()) {
				complete = false;
				continue;
			}
			rs->material_set_shader(material, shader->get_rid());
			if (const render::ShaderValue *color = look_value(*plan.look, "Color")) {
				rs->material_set_param(material, "tint", Vector3(color->value[0], color->value[1], color->value[2]));
			}
			if (const render::ShaderValue *speed = look_value(*plan.look, "ParticleSpeed")) {
				rs->material_set_param(material, "particle_speed", speed->value[0]);
			}
			m.shaded = true;
		}
		for (size_t l = 0; l < m.bound.size(); ++l) {
			if (m.bound[l] || plan.layers[l] < 0) {
				continue;
			}
			const RID texture = p_upload.texture_rid(uint32_t(plan.layers[l]));
			if (!texture.is_valid()) {
				complete = false;
				continue;
			}
			rs->material_set_param(material, to_godot(layer_uniform(plan.look->layers[l].role)), texture);
			m.bound[l] = true;
			++stats_.samplers_bound;
		}
	}
	complete_ = complete;
}

void EorShowroom::place_light(Node *p_parent, const core::level::EorSpotLight &p_light) {
	light_ = memnew(SpotLight3D);
	light_->set_name("EorShowroomLight");
	light_->set_color(Color(p_light.color[0], p_light.color[1], p_light.color[2]));
	const float offset = p_light.attenuation_offset > 0.0f ? p_light.attenuation_offset : 1.0f;
	light_->set_param(Light3D::PARAM_ENERGY, p_light.intensity / (offset * offset));
	light_->set_param(Light3D::PARAM_RANGE, p_light.radius);
	light_->set_param(Light3D::PARAM_SPOT_ANGLE, p_light.cone_outer_deg);
	light_->set_shadow(p_light.cast_shadows);
	light_->set_visible(false);
	p_parent->add_child(light_);
	light_->set_global_transform(instance_transform(p_light.transform));
	stats_.light_placed = true;
}

void EorShowroom::frame(Node *p_parent, const render::LevelScenePlan &p_plan, const LevelUpload &p_upload,
		const std::optional<core::level::EorCamera> &p_camera, bool p_light_on) {
	if (!collected_) {
		collect(p_plan);
	}
	if (!complete_) {
		bind(p_plan, p_upload);
	}
	if (light_ == nullptr && p_parent != nullptr && p_camera && p_camera->light) {
		place_light(p_parent, *p_camera->light);
	}
	if (light_ != nullptr && light_->is_visible() != p_light_on) {
		light_->set_visible(p_light_on);
	}
	stats_.light_visible = light_ != nullptr && p_light_on;
}

std::string EorShowroom::describe() const {
	char line[256];
	std::snprintf(line, sizeof(line),
			"[eor] showroom: %zu backdrop and %zu light cone materials, %zu of %zu samplers bound, light %s%s",
			stats_.backdrop_materials, stats_.cone_materials, stats_.samplers_bound, stats_.samplers_wanted,
			stats_.light_placed ? "placed" : "not placed", stats_.light_visible ? ", on" : "");
	return line;
}

}
