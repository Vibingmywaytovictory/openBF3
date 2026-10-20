#include "look_bridge.h"

#include "godot_util.h"
#include "level_upload.h"

#include <godot_cpp/classes/camera_attributes_practical.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/sky.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bf3::godot_side {

using namespace godot;
namespace render = bf3::render;

namespace {

constexpr float kLuminancePerIso = 0.00125f;

constexpr float kFilmicWhite = 4.0f;
constexpr float kSunShadowDistance = 250.0f;

const char *kSkyShader = R"(shader_type sky;
uniform sampler2D panorama : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D panorama_alpha : filter_linear_mipmap, repeat_enable;
uniform sampler2D gradient : source_color, filter_linear, repeat_disable;
uniform bool has_panorama = false;
uniform bool has_alpha = false;
uniform bool has_gradient = false;
uniform float brightness = 1.0;
uniform float rotation = 0.0;
uniform float uv_min_y = 0.0;
uniform float uv_max_y = 1.0;
uniform vec3 horizon = vec3(0.5);
uniform vec3 ambient_up = vec3(0.1);
uniform vec3 ambient_horizon = vec3(0.1);
uniform vec3 ambient_down = vec3(0.1);
void sky() {
	vec3 d = normalize(EYEDIR);
	if (AT_CUBEMAP_PASS) {
		COLOR = d.y >= 0.0 ? mix(ambient_horizon, ambient_up, smoothstep(0.0, 0.7, d.y)) : mix(ambient_horizon, ambient_down, smoothstep(0.0, 0.5, -d.y));
	} else {
		float vs = acos(clamp(d.y, -1.0, 1.0)) / PI;
		vec3 c = has_gradient ? textureLod(gradient, vec2(0.5, vs), 0.0).rgb * brightness : horizon;
		float v = (vs - uv_min_y) / max(uv_max_y - uv_min_y, 0.001);
		if (has_panorama && v >= 0.0 && v <= 1.0) {
			vec2 uv = vec2(fract(rotation - atan(d.x, d.z) / TAU), v);
			vec3 p = textureLod(panorama, uv, 0.0).rgb * brightness;
			float a = has_alpha ? textureLod(panorama_alpha, uv, 0.0).r : 1.0;
			c = mix(c, p, a);
		}
		COLOR = c;
	}
}
)";

Color color3(const render::LookVec3 &p_c) {
	return Color(p_c[0], p_c[1], p_c[2]);
}

float peak3(const render::LookVec3 &p_c) {
	return std::fmax(std::fmax(p_c[0], p_c[1]), std::fmax(p_c[2], 0.0f));
}

bool debug_off(const char *p_part) {
	static const std::string off = "," + user_arg_value("look-off") + ",";
	return off.find("," + std::string(p_part) + ",") != std::string::npos;
}

bool moved(float p_a, float p_b) {
	return std::fabs(p_a - p_b) > 0.01f * std::fmax(std::fmax(std::fabs(p_a), std::fabs(p_b)), 0.01f);
}

bool moved3(const render::LookVec3 &p_a, const render::LookVec3 &p_b) {
	return moved(p_a[0], p_b[0]) || moved(p_a[1], p_b[1]) || moved(p_a[2], p_b[2]);
}

}

LookBridge::LookBridge() = default;
LookBridge::~LookBridge() = default;

void LookBridge::build(Node *p_parent, std::shared_ptr<const render::LevelScenePlan> p_plan) {
	plan_ = std::move(p_plan);
	sun_ = memnew(DirectionalLight3D);
	sun_->set_name("Sun");
	sun_->set_shadow(true);
	sun_->set_param(Light3D::PARAM_SHADOW_MAX_DISTANCE, kSunShadowDistance);
	p_parent->add_child(sun_);

	Ref<Shader> shader;
	shader.instantiate();
	shader->set_code(to_godot(kSkyShader));
	sky_material_.instantiate();
	sky_material_->set_shader(shader);
	Ref<Sky> sky;
	sky.instantiate();
	sky->set_material(sky_material_);
	sky->set_radiance_size(Sky::RADIANCE_SIZE_64);
	sky->set_process_mode(Sky::PROCESS_MODE_INCREMENTAL);

	environment_.instantiate();
	environment_->set_background(Environment::BG_SKY);
	environment_->set_sky(sky);
	environment_->set_ambient_source(Environment::AMBIENT_SOURCE_SKY);
	environment_->set_ambient_light_energy(1.0f);
	environment_->set_ambient_light_sky_contribution(1.0f);
	environment_->set_reflection_source(Environment::REFLECTION_SOURCE_SKY);
	environment_->set_tonemapper(Environment::TONE_MAPPER_FILMIC);
	environment_->set_tonemap_white(kFilmicWhite);
	environment_->set_glow_blend_mode(Environment::GLOW_BLEND_MODE_ADDITIVE);
	environment_->set_glow_hdr_bleed_threshold(0.0f);
	environment_->set_glow_bloom(0.0f);

	RenderingServer::get_singleton()->environment_glow_set_use_bicubic_upscale(false);
	environment_->set_fog_mode(Environment::FOG_MODE_DEPTH);
	environment_->set_fog_sky_affect(0.0f);
	environment_->set_fog_aerial_perspective(0.0f);
	environment_->set_fog_sun_scatter(0.0f);

	const render::LookEnvironment &env = plan_->look_environment;
	const int32_t n = int32_t(render::kGradeLutSide);
	for (const std::vector<uint8_t> &lut : env.luts) {
		TypedArray<Image> slices;
		const size_t slice = size_t(n) * size_t(n) * 4;
		if (lut.size() != slice * size_t(n)) {
			luts_.emplace_back();
			continue;
		}
		for (int32_t z = 0; z < n; ++z) {
			PackedByteArray bytes;
			bytes.resize(int64_t(slice));
			std::memcpy(bytes.ptrw(), lut.data() + size_t(z) * slice, slice);
			slices.push_back(Image::create_from_data(n, n, false, Image::FORMAT_RGBA8, bytes));
		}
		Ref<ImageTexture3D> texture;
		texture.instantiate();
		if (texture->create(Image::FORMAT_RGBA8, n, n, n, false, slices) != OK) {
			texture.unref();
		}
		luts_.push_back(texture);
	}

	world_ = memnew(WorldEnvironment);
	world_->set_name("Environment");
	world_->set_environment(environment_);
	attributes_.instantiate();
	world_->set_camera_attributes(attributes_);
	p_parent->add_child(world_);
	apply(render::look_frame_of(env.base), true);
}

void LookBridge::update(const LevelUpload &p_upload, const Vector3 &p_camera) {
	if (plan_ == nullptr) {
		return;
	}
	const int32_t indices[3] = { plan_->sky_panorama, plan_->sky_panorama_alpha, plan_->sky_gradient };
	const char *names[3] = { "panorama", "panorama_alpha", "gradient" };
	const char *flags[3] = { "has_panorama", "has_alpha", "has_gradient" };
	for (size_t i = 0; i < 3; ++i) {
		if (sky_bound_[i] || indices[i] < 0) {
			continue;
		}
		const RID texture = p_upload.texture_rid(uint32_t(indices[i]));
		if (texture.is_valid()) {
			sky_material_->set_shader_parameter(names[i], texture);
			sky_material_->set_shader_parameter(flags[i], true);
			sky_bound_[i] = true;
		}
	}
	apply(render::look_frame(plan_->look_environment, { float(p_camera.x), float(p_camera.y), float(p_camera.z) }), false);
}

void LookBridge::apply(const render::LookFrame &p_f, bool p_force) {

	const bool first = p_force || !sent_;
	render::LookFrame &s = sent_ ? *sent_ : sent_.emplace(p_f);

	if (first || moved3(s.sun_direction, p_f.sun_direction) || moved3(s.sun_color, p_f.sun_color) || moved(s.sun_energy, p_f.sun_energy)) {
		Vector3 d(p_f.sun_direction[0], p_f.sun_direction[1], p_f.sun_direction[2]);
		d = d.length_squared() > 1e-8f ? d.normalized() : Vector3(0, -1, 0);
		const Vector3 up = std::fabs(d.y) > 0.99f ? Vector3(0, 0, 1) : Vector3(0, 1, 0);
		sun_->set_transform(Transform3D(Basis::looking_at(d, up), Vector3()));
		sun_->set_color(color3(p_f.sun_color));
		sun_->set_param(Light3D::PARAM_ENERGY, p_f.sun_energy);
		sun_->set_visible(p_f.sun_energy > 0.0f);
		s.sun_direction = p_f.sun_direction;
		s.sun_color = p_f.sun_color;
		s.sun_energy = p_f.sun_energy;
	}

	if (first || moved3(s.ambient_up, p_f.ambient_up) || moved3(s.ambient_horizon, p_f.ambient_horizon) || moved3(s.ambient_down, p_f.ambient_down) ||
			moved(s.sky_brightness, p_f.sky_brightness) || moved3(s.horizon, p_f.horizon) || s.sky_rotation != p_f.sky_rotation ||
			s.uv_min_y != p_f.uv_min_y || s.uv_max_y != p_f.uv_max_y) {
		sky_material_->set_shader_parameter("ambient_up", color3(p_f.ambient_up));
		sky_material_->set_shader_parameter("ambient_horizon", color3(p_f.ambient_horizon));
		sky_material_->set_shader_parameter("ambient_down", color3(p_f.ambient_down));
		sky_material_->set_shader_parameter("brightness", p_f.sky_brightness);
		sky_material_->set_shader_parameter("rotation", p_f.sky_rotation);
		sky_material_->set_shader_parameter("uv_min_y", p_f.uv_min_y);
		sky_material_->set_shader_parameter("uv_max_y", p_f.uv_max_y);
		sky_material_->set_shader_parameter("horizon", color3(p_f.horizon));
		s.ambient_up = p_f.ambient_up;
		s.ambient_horizon = p_f.ambient_horizon;
		s.ambient_down = p_f.ambient_down;
		s.sky_brightness = p_f.sky_brightness;
		s.sky_rotation = p_f.sky_rotation;
		s.uv_min_y = p_f.uv_min_y;
		s.uv_max_y = p_f.uv_max_y;
		s.horizon = p_f.horizon;
	}

	if (first || s.fog != p_f.fog || moved(s.fog_begin, p_f.fog_begin) || moved(s.fog_end, p_f.fog_end) || moved(s.fog_density, p_f.fog_density) ||
			moved(s.fog_curve, p_f.fog_curve) || moved3(s.fog_color, p_f.fog_color)) {
		environment_->set_fog_enabled(p_f.fog && !debug_off("fog"));
		if (p_f.fog) {
			const float level = peak3(p_f.fog_color);
			environment_->set_fog_light_color(level > 1e-6f ? Color(p_f.fog_color[0] / level, p_f.fog_color[1] / level, p_f.fog_color[2] / level) : Color(0, 0, 0));
			environment_->set_fog_light_energy(level);
			environment_->set_fog_density(p_f.fog_density);
			environment_->set_fog_depth_curve(p_f.fog_curve);
			environment_->set_fog_depth_begin(p_f.fog_begin);
			environment_->set_fog_depth_end(std::fmax(p_f.fog_end, p_f.fog_begin + 1.0f));
		}
		s.fog = p_f.fog;
		s.fog_begin = p_f.fog_begin;
		s.fog_end = p_f.fog_end;
		s.fog_density = p_f.fog_density;
		s.fog_curve = p_f.fog_curve;
		s.fog_color = p_f.fog_color;
	}

	if (first || moved(s.middle_gray, p_f.middle_gray) || moved(s.min_exposure, p_f.min_exposure) || moved(s.max_exposure, p_f.max_exposure) ||
			moved(s.exposure_speed, p_f.exposure_speed) || moved(s.exposure, p_f.exposure)) {
		static const std::string fixed = user_arg_value("look-exposure");
		const bool automatic = fixed.empty() && !debug_off("exposure");
		attributes_->set_auto_exposure_enabled(automatic);
		if (automatic) {
			environment_->set_tonemap_exposure(1.0f);
			attributes_->set_auto_exposure_scale(p_f.middle_gray);
			attributes_->set_auto_exposure_min_sensitivity(p_f.middle_gray / std::fmax(p_f.max_exposure, 1e-3f) / kLuminancePerIso);
			attributes_->set_auto_exposure_max_sensitivity(p_f.middle_gray / std::fmax(p_f.min_exposure, 1e-3f) / kLuminancePerIso);
			attributes_->set_auto_exposure_speed(p_f.exposure_speed);
		} else {
			environment_->set_tonemap_exposure(fixed.empty() ? p_f.exposure : float(std::atof(fixed.c_str())));
		}
		s.middle_gray = p_f.middle_gray;
		s.min_exposure = p_f.min_exposure;
		s.max_exposure = p_f.max_exposure;
		s.exposure_speed = p_f.exposure_speed;
		s.exposure = p_f.exposure;
	}

	if (first || moved(s.bloom, p_f.bloom)) {
		static const bool bloom_on = has_user_arg("look-bloom");
		environment_->set_glow_enabled(bloom_on && p_f.bloom > 0.0f && !debug_off("bloom"));
		environment_->set_glow_intensity(p_f.bloom);
		s.bloom = p_f.bloom;
	}

	if (first || p_f.lut != s.lut) {
		const bool have = !debug_off("lut") && p_f.lut >= 0 && size_t(p_f.lut) < luts_.size() && luts_[size_t(p_f.lut)].is_valid();
		environment_->set_adjustment_enabled(have);
		if (have) {
			environment_->set_adjustment_color_correction(luts_[size_t(p_f.lut)]);
		}
		s.lut = p_f.lut;
	}
	applied_ = p_f;
}

std::string LookBridge::describe() const {
	if (!applied_) {
		return "look: not built";
	}
	const render::LookFrame &f = *applied_;
	char line[640];
	std::snprintf(line, sizeof(line),
			"look L1: sun (%.3f %.3f %.3f) x %.2f; ambient up (%.3f %.3f %.3f) down (%.3f %.3f %.3f); sky x%.2f (textures %d%d%d); fog %s %.0f..%.0f m "
			"density %.3f curve %.2f; middle grey %.3f exposure %.2f (%.2f..%.2f); bloom %.3f; lut %d",
			f.sun_color[0], f.sun_color[1], f.sun_color[2], f.sun_energy, f.ambient_up[0], f.ambient_up[1], f.ambient_up[2], f.ambient_down[0],
			f.ambient_down[1], f.ambient_down[2], f.sky_brightness, int(sky_bound_[0]), int(sky_bound_[1]), int(sky_bound_[2]), f.fog ? "on" : "off",
			f.fog_begin, f.fog_end, f.fog_density, f.fog_curve, f.middle_gray, f.exposure, f.min_exposure, f.max_exposure, f.bloom, f.lut);
	return line;
}

}
