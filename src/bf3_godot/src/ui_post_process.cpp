#include "ui_post_process.h"

#include "bf3_localisation.h"
#include "godot_util.h"
#include "overview_map.h"
#include "ui_cursor.h"
#include "ui_stage.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/install.h"
#include "bf3/core/util/elapsed.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;

namespace {

Bf3UiPostProcess *g_current = nullptr;

constexpr std::string_view kGlitchShader = R"(shader_type canvas_item;
render_mode unshaded, blend_disabled;
uniform sampler2D screen : hint_screen_texture, filter_linear, repeat_disable;
uniform sampler2D noise : filter_nearest, repeat_enable;
uniform float power = 0.0;
uniform float r_shift = 0.0;
uniform float g_shift = 0.0;
uniform float b_shift = 0.0;
uniform float distort = 0.0;
uniform float x_power = 0.0;
uniform float y_power = 0.0;
uniform float time_scale = 1.0;
uniform float highlight = 0.0;
uniform float highlight_threshold = 1.0;
uniform float time = 0.0;
uniform vec2 noise_size = vec2(256.0);
uniform float stage_scale = 1.0;
void fragment() {
	vec2 px = FRAGCOORD.xy / stage_scale;
	float amount = clamp(power * time_scale, 0.0, 1.0);
	vec2 scroll = fract(vec2(time * time_scale * 0.731, time * time_scale * 0.317));
	float block = texture(noise, vec2(scroll.x, floor(px.y / 8.0) / noise_size.y + scroll.y)).r;
	float band = texture(noise, vec2(scroll.y, floor(px.y / 8.0) / noise_size.y + scroll.x + 0.5)).r - 0.5;
	float row = texture(noise, vec2(scroll.x + 0.5, px.y / noise_size.y + scroll.y)).r - 0.5;
	float active = step(block, power);
	vec2 offset = vec2((band * 2.0 * active + row * x_power) * distort, row * y_power * distort) * amount;
	vec2 uv = SCREEN_UV + offset;
	vec3 c;
	c.r = texture(screen, uv + vec2(r_shift * amount, 0.0)).r;
	c.g = texture(screen, uv + vec2(g_shift * amount, 0.0)).g;
	c.b = texture(screen, uv + vec2(b_shift * amount, 0.0)).b;
	float luma = dot(c, vec3(0.299, 0.587, 0.114));
	c += c * max(luma - highlight_threshold, 0.0) * highlight * amount;
	COLOR = vec4(clamp(c, 0.0, 1.0), 1.0);
}
)";

constexpr std::string_view kGradeShader = R"(shader_type canvas_item;
render_mode unshaded, blend_disabled;
uniform sampler2D screen : hint_screen_texture, filter_linear, repeat_disable;
uniform float visibility = 0.0;
uniform vec3 brightness = vec3(1.0);
uniform vec3 contrast = vec3(1.0);
uniform vec3 saturation = vec3(1.0);
uniform vec3 vignette_color = vec3(0.0);
uniform vec2 vignette_scale = vec2(1.0);
uniform float vignette_opacity = 0.0;
uniform float vignette_exponent = 1.0;
uniform float blur_px = 0.0;
uniform float stage_scale = 1.0;
void fragment() {
	vec3 base = texture(screen, SCREEN_UV).rgb;
	vec3 c = base;
	float sigma = blur_px * stage_scale * visibility;
	if (sigma > 0.01) {
		vec3 sum = vec3(0.0);
		float wsum = 0.0;
		for (int y = -3; y <= 3; y++) {
			for (int x = -3; x <= 3; x++) {
				vec2 d = vec2(float(x), float(y)) * sigma * 0.5;
				float w = exp(-dot(d, d) / (2.0 * sigma * sigma));
				sum += texture(screen, SCREEN_UV + d * SCREEN_PIXEL_SIZE).rgb * w;
				wsum += w;
			}
		}
		c = sum / wsum;
	}
	float luma = dot(c, vec3(0.299, 0.587, 0.114));
	vec3 s = vec3(luma) + (c - vec3(luma)) * saturation;
	vec3 g = clamp(((s - 0.5) * contrast + 0.5) * brightness, 0.0, 1.0);
	float d = length((SCREEN_UV - 0.5) * vignette_scale);
	float v = pow(clamp(d, 0.0, 1.0), max(vignette_exponent, 0.001)) * clamp(vignette_opacity, 0.0, 1.0);
	g = mix(g, vignette_color, v);
	COLOR = vec4(mix(base, g, clamp(visibility, 0.0, 1.0)), 1.0);
}
)";

Ref<ShaderMaterial> make_material(std::string_view p_code) {
	Ref<Shader> shader;
	shader.instantiate();
	shader->set_code(String::utf8(p_code.data(), int64_t(p_code.size())));
	Ref<ShaderMaterial> m;
	m.instantiate();
	m->set_shader(shader);
	return m;
}

}

Vector3 vec3_of(const std::array<float, 3> &p_v) {
	return Vector3(p_v[0], p_v[1], p_v[2]);
}

Ref<ShaderMaterial> make_grade_material() {
	return make_material(kGradeShader);
}

struct Bf3UiPostProcess::Loaded {
	std::optional<ui::UiPostProcessData> data;
	std::optional<ui::MenuGrade> grade;
	std::optional<ui::UiDimmerRamp> ramp;
	ui::L8Image noise;
	std::string error;
	double seconds = 0.0;
};

Bf3UiPostProcess::Bf3UiPostProcess() = default;

Bf3UiPostProcess::~Bf3UiPostProcess() {
	join_loader();
	if (g_current == this) {
		g_current = nullptr;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs != nullptr) {
		for (RID *r : { &grade_item_, &glitch_item_ }) {
			if (r->is_valid()) {
				rs->free_rid(*r);
				*r = RID();
			}
		}
	}
}

Bf3UiPostProcess *Bf3UiPostProcess::current() {
	return g_current;
}

Bf3UiPostProcess *Bf3UiPostProcess::ensure(Node *p_parent, const std::string &p_level) {
	if (g_current != nullptr) {
		return g_current;
	}
	if (p_parent == nullptr) {
		return nullptr;
	}
	Bf3UiPostProcess *p = memnew(Bf3UiPostProcess);
	p->set_name("UiPostProcess");
	p->set_level(p_level);
	p_parent->add_child(p);
	return p;
}

void Bf3UiPostProcess::_bind_methods() {
	ClassDB::bind_method(D_METHOD("trigger_glitch", "power"), &Bf3UiPostProcess::trigger_glitch);
	ClassDB::bind_method(D_METHOD("dim_in"), &Bf3UiPostProcess::dim_in);
	ClassDB::bind_method(D_METHOD("dim_out"), &Bf3UiPostProcess::dim_out);
	ClassDB::bind_method(D_METHOD("is_ready"), &Bf3UiPostProcess::is_ready);
}

void Bf3UiPostProcess::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE && g_current == nullptr) {
		g_current = this;
	} else if (p_what == NOTIFICATION_PREDELETE) {
		join_loader();
		if (g_current == this) {
			g_current = nullptr;
		}
	}
}

void Bf3UiPostProcess::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	set_process(true);
}

void Bf3UiPostProcess::_exit_tree() {
	join_loader();
	if (g_current == this) {
		g_current = nullptr;
	}
}

void Bf3UiPostProcess::set_level(const std::string &p_level) {
	if (!started_) {
		level_ = p_level;
	}
}

void Bf3UiPostProcess::join_loader() {
	if (loader_.joinable()) {
		loader_.join();
	}
}

void Bf3UiPostProcess::start_loader() {
	started_ = true;
	Bf3Localisation *loc = Bf3Localisation::current();
	std::shared_ptr<const bf3::core::Install> install = loc != nullptr ? loc->install() : nullptr;
	const std::string superbundle = "Win32/Levels/" + level_ + "/" + level_;
	loader_done_.store(false, std::memory_order_relaxed);
	loader_ = std::thread([this, install, superbundle] {
		const auto t0 = std::chrono::steady_clock::now();
		auto out = std::make_unique<Loaded>();
		if (install == nullptr) {
			out->error = "no install (no Bf3Localisation)";
		} else if (bf3::core::Result<std::unique_ptr<bf3::core::EbxLibrary>> lib =
						   bf3::core::EbxLibrary::open(install, { superbundle }, 2);
				!lib) {
			out->error = superbundle + ": " + lib.error().message;
		} else {
			bf3::core::Result<ui::UiPostProcessData> d = ui::read_ui_post_process(**lib);
			bf3::core::Result<ui::MenuGrade> g = ui::read_menu_visual_environment(**lib);
			bf3::core::Result<ui::UiDimmerRamp> r = ui::read_ingame_dimmer(**lib);
			if (!d) {
				out->error += d.error().message + "; ";
			}
			if (!g) {
				out->error += g.error().message + "; ";
			}
			if (!r) {
				out->error += r.error().message + "; ";
			}
			if (d) {
				bf3::core::Result<ui::L8Image> noise = ui::load_l8_texture((*lib)->mount(0), d->noise_texture);
				if (!noise) {
					out->error += d->noise_texture + ": " + noise.error().message + "; ";
				} else {
					out->noise = std::move(*noise);
				}
				out->data = std::move(*d);
			}
			if (g) {
				out->grade = std::move(*g);
			}
			if (r) {
				out->ramp = std::move(*r);
			}
		}
		out->seconds = bf3::core::seconds_since(t0);
		loaded_ = std::move(out);
		loader_done_.store(true, std::memory_order_release);
	});
}

CanvasLayer *Bf3UiPostProcess::make_layer(
		const char *p_name, int32_t p_layer, const Ref<ShaderMaterial> &p_material, RID &r_item) {
	CanvasLayer *layer = memnew(CanvasLayer);
	layer->set_name(p_name);
	layer->set_layer(p_layer);
	layer->set_visible(false);
	add_child(layer);
	RenderingServer *rs = RenderingServer::get_singleton();
	r_item = rs->canvas_item_create();
	rs->canvas_item_set_parent(r_item, layer->get_canvas());
	rs->canvas_item_set_material(r_item, p_material->get_rid());
	return layer;
}

void Bf3UiPostProcess::cover(RID p_item) {
	RenderingServer *rs = RenderingServer::get_singleton();
	const Vector2 size = ui_window_rect(this).size;
	rs->canvas_item_clear(p_item);
	rs->canvas_item_add_rect(p_item, Rect2(Vector2(), size), Color(1, 1, 1, 1));
}

void Bf3UiPostProcess::build() {
	built_ = true;
	if (!loaded_ || !loaded_->error.empty() || !loaded_->data || !loaded_->grade || !loaded_->ramp) {
		failed_ = true;
		error_ = loaded_ ? loaded_->error : std::string("no loader result");
		UtilityFunctions::push_error(to_godot("[ui post process] not drawn: " + error_));
		return;
	}
	data_ = loaded_->data;
	grade_ = loaded_->grade;
	dimmer_ = std::make_unique<ui::UiDimmer>(*loaded_->ramp);
	noise_ = std::move(loaded_->noise);
	PackedByteArray bytes;
	bytes.resize(int64_t(noise_.texels.size()));
	std::copy(noise_.texels.begin(), noise_.texels.end(), bytes.ptrw());
	Ref<Image> img =
			Image::create_from_data(int32_t(noise_.width), int32_t(noise_.height), false, Image::FORMAT_L8, bytes);
	noise_texture_ = ImageTexture::create_from_image(img);

	glitch_material_ = make_material(kGlitchShader);
	const ui::UiPostProcessData &d = *data_;
	glitch_material_->set_shader_parameter("noise", noise_texture_);
	glitch_material_->set_shader_parameter("noise_size", Vector2(float(noise_.width), float(noise_.height)));
	glitch_material_->set_shader_parameter("r_shift", d.r_shift);
	glitch_material_->set_shader_parameter("g_shift", d.g_shift);
	glitch_material_->set_shader_parameter("b_shift", d.b_shift);
	glitch_material_->set_shader_parameter("distort", d.distort);
	glitch_material_->set_shader_parameter("x_power", d.x_power);
	glitch_material_->set_shader_parameter("y_power", d.y_power);
	glitch_material_->set_shader_parameter("time_scale", d.time_scale);
	glitch_material_->set_shader_parameter("highlight", d.highlight);
	glitch_material_->set_shader_parameter("highlight_threshold", d.highlight_threshold);

	grade_material_ = make_material(kGradeShader);
	const ui::MenuGrade &g = *grade_;
	grade_material_->set_shader_parameter("brightness", vec3_of(g.brightness));
	grade_material_->set_shader_parameter("contrast", vec3_of(g.contrast));
	grade_material_->set_shader_parameter("saturation", vec3_of(g.saturation));
	grade_material_->set_shader_parameter("vignette_color", vec3_of(g.vignette_color));
	grade_material_->set_shader_parameter("vignette_scale", Vector2(g.vignette_scale[0], g.vignette_scale[1]));
	grade_material_->set_shader_parameter("vignette_opacity", g.vignette ? g.vignette_opacity : 0.0f);
	grade_material_->set_shader_parameter("vignette_exponent", g.vignette_exponent);
	grade_material_->set_shader_parameter("blur_px", g.dof ? g.blur_deviation : 0.0f);

	grade_layer_ = make_layer("MenuGrade", kUiGradeLayer, grade_material_, grade_item_);
	glitch_layer_ = make_layer("Glitch", kUiGlitchLayer, glitch_material_, glitch_item_);
	if (pending_glitch_) {
		trigger_glitch(*pending_glitch_);
		pending_glitch_.reset();
		stats_.pending_applied++;
	}
	if (pending_dim_ > 0) {
		dim_in();
		stats_.pending_applied++;
	} else if (pending_dim_ < 0) {
		dim_out();
		stats_.pending_applied++;
	}
	pending_dim_ = 0;
	UtilityFunctions::print(to_godot("[ui post process] ready in " +
			std::to_string(int(loaded_->seconds * 1000.0 + 0.5)) + " ms: noise " + d.noise_texture + " " +
			std::to_string(noise_.width) + "x" + std::to_string(noise_.height) + ", dimmer " +
			std::to_string(int(dimmer_->ramp().in.length_ms)) + " ms over " + dimmer_->ramp().visual_environment));
	loaded_.reset();
	warm_frames_ = kUiPostProcessWarmFrames;
	apply();
}

void Bf3UiPostProcess::trigger_glitch(float p_power) {
	if (!data_) {
		pending_glitch_ = p_power;
		return;
	}
	if (!std::isfinite(p_power) || p_power <= 0.0f || !data_->enabled) {
		return;
	}
	glitch_start_ = p_power;
	glitch_at_ = clock_;
	stats_.glitch_triggers++;
	stats_.last_trigger_power = p_power;
	apply();
}

void Bf3UiPostProcess::dim_in() {
	if (dimmer_ == nullptr) {
		pending_dim_ = 1;
		return;
	}
	dimmer_->fade_in(clock_);
	stats_.dim_ins++;
	apply();
}

void Bf3UiPostProcess::dim_out() {
	if (dimmer_ == nullptr) {
		pending_dim_ = -1;
		return;
	}
	dimmer_->fade_out(clock_);
	stats_.dim_outs++;
	apply();
}

float Bf3UiPostProcess::glitch_power() const {
	return data_ ? ui::ui_glitch_power(*data_, glitch_start_, clock_ - glitch_at_) : 0.0f;
}

float Bf3UiPostProcess::dim_visibility() const {
	return dimmer_ != nullptr ? dimmer_->visibility(clock_) : 0.0f;
}

bool Bf3UiPostProcess::dim_enabled() const {
	return dimmer_ != nullptr && dimmer_->enabled(clock_);
}

const ui::UiPostProcessData *Bf3UiPostProcess::data() const {
	return data_ ? &*data_ : nullptr;
}

const ui::MenuGrade *Bf3UiPostProcess::grade() const {
	return grade_ ? &*grade_ : nullptr;
}

const ui::UiDimmerRamp *Bf3UiPostProcess::ramp() const {
	return dimmer_ != nullptr ? &dimmer_->ramp() : nullptr;
}

const ui::L8Image *Bf3UiPostProcess::noise() const {
	return noise_.empty() ? nullptr : &noise_;
}

bool Bf3UiPostProcess::glitch_drawn() const {
	return glitch_layer_ != nullptr && glitch_layer_->is_visible() && glitch_power() > 0.0f;
}

bool Bf3UiPostProcess::grade_drawn() const {
	return grade_layer_ != nullptr && grade_layer_->is_visible() && dim_visibility() > 0.0f;
}

void Bf3UiPostProcess::apply() {
	if (!built_ || failed_) {
		return;
	}
	const Vector2 size = ui_window_rect(this).size;
	if (size != covered_) {
		cover(grade_item_);
		cover(glitch_item_);
		covered_ = size;
	}
	const float scale = std::max(1e-3f, float(ui_stage(this).scale));
	const float power = glitch_power();
	glitch_layer_->set_visible(power > 0.0f || warm_frames_ > 0);
	glitch_material_->set_shader_parameter("power", power);
	glitch_material_->set_shader_parameter("time", float(clock_));
	glitch_material_->set_shader_parameter("stage_scale", scale);
	const float v = dim_visibility();
	grade_layer_->set_visible(v > 0.0f || warm_frames_ > 0);
	grade_material_->set_shader_parameter("visibility", v);
	grade_material_->set_shader_parameter("stage_scale", scale);
}

void Bf3UiPostProcess::step(double p_seconds) {
	if (!built_ || failed_) {
		return;
	}
	if (std::isfinite(p_seconds) && p_seconds > 0.0) {
		clock_ += p_seconds;
	}
	apply();
	if (warm_frames_ > 0) {
		--warm_frames_;
		stats_.warm_frames++;
	}
	if (glitch_drawn()) {
		stats_.glitch_frames++;
	}
	if (grade_drawn()) {
		stats_.dim_frames++;
	}
}

void Bf3UiPostProcess::_process(double p_delta) {
	if (!built_) {
		if (!started_) {
			start_loader();
			return;
		}
		if (!loader_done_.load(std::memory_order_acquire)) {
			return;
		}
		join_loader();
		build();
		return;
	}
	if (failed_ || manual_) {
		return;
	}
	step(p_delta);
}

}

void openbf3_register_ui_effects() {
	GDREGISTER_CLASS(bf3::godot_side::Bf3UiPostProcess);
	GDREGISTER_CLASS(bf3::godot_side::Bf3UiCursor);
	GDREGISTER_CLASS(bf3::godot_side::Bf3OverviewMap);
}
