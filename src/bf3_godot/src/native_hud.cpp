#include "native_hud.h"

#include "bf3_localisation.h"
#include "godot_util.h"
#include "minimap_control.h"
#include "minimap_hud.h"
#include "swf_view.h"
#include "ui_assets.h"
#include "ui_stage.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/install.h"
#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/ui/minimap.h"
#include "bf3/core/ui/text_db.h"
#include "bf3/core/util/elapsed.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;

namespace {

constexpr std::string_view kMinimapMapShader = R"(shader_type canvas_item;
uniform sampler2D satellite : filter_linear_mipmap, repeat_disable;
uniform sampler2D combat_area : filter_linear, repeat_disable;
uniform sampler2D detail : filter_linear, repeat_disable;
uniform sampler2D mask : filter_linear, repeat_disable;
uniform sampler2D hatch : filter_linear, repeat_enable;
uniform vec4 tint = vec4(0.3, 0.6, 0.8, 1.0);
uniform vec4 area_color = vec4(0.991, 0.706, 0.513, 0.45);
uniform float threshold = 0.496;
uniform float edge_width = 0.01;
uniform vec4 detail_tint = vec4(0.9, 1.0, 1.0, 0.3);
uniform float detail_threshold = 0.5;
uniform float opacity = 0.5;
uniform float satellite_weight = 0.0;
uniform bool has_detail = false;
uniform bool has_mask = false;
uniform bool has_hatch = false;
uniform vec4 local_rect = vec4(0.0, 0.0, 170.0, 170.0);
uniform vec4 square_rect = vec4(0.0, 0.0, 170.0, 170.0);
uniform vec2 centre_local = vec2(85.0, 85.0);
uniform vec2 player_xz = vec2(0.0);
uniform float heading = 0.0;
uniform float metres_per_unit = 1.375;
uniform vec2 world_center = vec2(0.0);
uniform float world_size = 1024.0;
uniform vec2 hatch_wrap = vec2(70.0, 70.0);
void fragment() {
	vec2 p = local_rect.xy + UV * local_rect.zw;
	vec2 d = p - centre_local;
	float c = cos(heading);
	float s = sin(heading);
	vec2 r = vec2(d.x * c + d.y * s, -d.x * s + d.y * c);
	vec2 world = player_xz - r * metres_per_unit;
	vec2 uv = vec2(0.5) - (world - world_center) / world_size;
	vec3 base = mix(tint.rgb, texture(satellite, uv).r * tint.rgb, satellite_weight);
	vec4 acc = vec4(base * opacity, opacity);
	if (has_detail) {
		float w = detail_tint.a * smoothstep(detail_threshold - edge_width, detail_threshold + edge_width, texture(detail, uv).r);
		acc = vec4(detail_tint.rgb * w, w) + acc * (1.0 - w);
	}
	float outside = smoothstep(threshold - edge_width, threshold + edge_width, texture(combat_area, uv).r);
	float h = has_hatch ? texture(hatch, uv * hatch_wrap).r : 1.0;
	float ca = outside * area_color.a * h;
	acc = vec4(area_color.rgb * ca, ca) + acc * (1.0 - ca);
	vec2 m = (p - square_rect.xy) / square_rect.zw;
	float a = has_mask ? texture(mask, m).r : 1.0;
	if (m.x < 0.0 || m.y < 0.0 || m.x > 1.0 || m.y > 1.0) {
		a = 0.0;
	}
	COLOR = vec4(acc.rgb / max(acc.a, 1e-6), acc.a * a);
}
)";

constexpr std::string_view kMinimapRenderer = ui::kMinimapRenderer;
constexpr std::string_view kBgRenderer = ui::kBgDrawRenderer;
constexpr std::string_view kEkgRenderer = ui::kSoldierRenderer;
constexpr const char *kMinimapClip = "Minimap_01";

std::vector<uint8_t> image_bytes(Ref<Image> p_image, int32_t &r_w, int32_t &r_h) {
	r_w = r_h = 0;
	if (p_image.is_null() || p_image->is_empty()) {
		return {};
	}
	if (p_image->get_format() != Image::FORMAT_RGBA8) {
		p_image->convert(Image::FORMAT_RGBA8);
	}
	r_w = p_image->get_width();
	r_h = p_image->get_height();
	const PackedByteArray d = p_image->get_data();
	return std::vector<uint8_t>(d.ptr(), d.ptr() + d.size());
}

}

struct Bf3NativeHud::Loaded {
	std::array<std::optional<ui::HudScreenAsset>, kNativeHudViews> assets;
	std::optional<ui::HudMessageCompData> messages;
	std::optional<ui::HudDamageIndicatorCompData> damage;
	std::string error;
	double seconds = 0.0;
};

Bf3NativeHud::Bf3NativeHud() = default;

Bf3NativeHud::~Bf3NativeHud() {
	join_loader();
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs != nullptr) {
		for (RID *r : { &minimap_map_item_, &minimap_icon_item_, &tags_item_ }) {
			if (r->is_valid()) {
				rs->free_rid(*r);
				*r = RID();
			}
		}
	}
}

void Bf3NativeHud::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_ready"), &Bf3NativeHud::is_ready);
}

void Bf3NativeHud::_notification(int p_what) {
	if (p_what == NOTIFICATION_PREDELETE) {
		join_loader();
		tags_.reset();
		for (std::unique_ptr<ui::HudScreen> &s : screens_) {
			s.reset();
		}
	}
}

void Bf3NativeHud::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	set_process(true);
}

void Bf3NativeHud::_exit_tree() {
	join_loader();
}

void Bf3NativeHud::join_loader() {
	if (loader_.joinable()) {
		loader_.join();
	}
}

void Bf3NativeHud::set_level(const std::string &p_level) {
	level_ = p_level;
}

void Bf3NativeHud::set_model(const ui::HudModel &p_model) {
	if (!fixture_) {
		model_ = p_model;
	}
}

std::optional<ui::HudStageRects> Bf3NativeHud::stage_rects() const {
	for (const std::unique_ptr<ui::HudScreen> &s : screens_) {
		if (s != nullptr) {
			return s->stage_rects();
		}
	}
	return std::nullopt;
}

void Bf3NativeHud::set_vfov_deg(double p_deg) {
	vfov_deg_ = p_deg;
}

void Bf3NativeHud::set_minimap_pose(const NativeHudMinimapPose &p_pose) {
	if (!fixture_) {
		pose_ = p_pose;
	}
}

void Bf3NativeHud::set_fixture(const ui::HudModel &p_model, const NativeHudMinimapPose &p_pose) {
	fixture_ = true;
	model_ = p_model;
	pose_ = p_pose;
}

void Bf3NativeHud::set_manual(bool p_manual) {
	manual_ = p_manual;
}

void Bf3NativeHud::set_world(Bf3LevelSession *p_session, const Camera3D *p_camera, bool p_show) {
	if (tags_ == nullptr && p_session != nullptr) {
		tags_ = std::make_unique<NativeTags>(p_session);
	}
	world_camera_ = p_camera;
	world_show_ = p_show;
}

void Bf3NativeHud::start_loader() {
	Bf3Localisation *loc = Bf3Localisation::current();
	std::shared_ptr<const bf3::core::Install> install = loc->install();
	const std::string superbundle = "Win32/Levels/" + level_ + "/" + level_;
	loaded_.reset();
	loader_done_.store(false, std::memory_order_relaxed);
	loader_ = std::thread([this, install, superbundle] {
		const auto t0 = std::chrono::steady_clock::now();
		auto out = std::make_unique<Loaded>();
		bf3::core::Result<std::unique_ptr<bf3::core::EbxLibrary>> lib = bf3::core::EbxLibrary::open(install, { superbundle }, 2);
		if (!lib) {
			out->error = superbundle + ": " + lib.error().message;
		} else {
			for (size_t i = 0; i < kNativeHudViews; ++i) {
				bf3::core::Result<ui::HudScreenAsset> a = ui::read_hud_screen_asset(**lib, kNativeHudPartitions[i]);
				if (!a) {
					out->error += std::string(kNativeHudPartitions[i]) + ": " + a.error().message + "; ";
					continue;
				}
				out->assets[i] = std::move(*a);
			}
			bf3::core::Result<ui::HudMessageCompData> m = ui::read_hud_message_comp(**lib);
			if (!m) {
				out->error += std::string(ui::kUiMessageCompPartition) + ": " + m.error().message + "; ";
			} else {
				out->messages = std::move(*m);
			}
			bf3::core::Result<ui::HudDamageIndicatorCompData> d = ui::read_hud_damage_indicator_comp(**lib);
			if (!d) {
				out->error += std::string(ui::kUiDamageIndicatorCompPartition) + ": " + d.error().message + "; ";
			} else {
				out->damage = *d;
			}
		}
		out->seconds = bf3::core::seconds_since(t0);
		loaded_ = std::move(out);
		loader_done_.store(true, std::memory_order_release);
	});
}

void Bf3NativeHud::build() {
	built_ = true;
	Bf3UiAssets *assets = Bf3UiAssets::current();
	Bf3Localisation *loc = Bf3Localisation::current();
	if (!loaded_ || !loaded_->error.empty() || assets == nullptr || assets->movies() == nullptr) {
		failed_ = true;
		error_ = loaded_ ? loaded_->error : std::string("no loader result");
		if (assets == nullptr || assets->movies() == nullptr) {
			error_ += " (no Bf3UiAssets movies)";
		}
		UtilityFunctions::push_error(to_godot("[hud] native HUD not drawn: " + error_));
		return;
	}
	client_ = ui::hud_text_client(loc != nullptr ? loc->database() : nullptr, std::move(client_));
	client_.vertical_fov_deg = float(vfov_deg_);
	messages_ = std::make_unique<ui::HudMessageComp>(*loaded_->messages);
	client_.messages = messages_.get();
	damage_ = std::make_unique<ui::HudDamageIndicatorComp>(*loaded_->damage);
	client_.damage = damage_.get();
	std::string summary;
	for (size_t i = 0; i < kNativeHudViews; ++i) {
		const bf3::core::Status st = open_view(i);
		if (!st) {
			failed_ = true;
			error_ = st.error().message;
			UtilityFunctions::push_error(to_godot("[hud] native HUD not drawn: " + error_));
			return;
		}
		summary += " " + screens_[i]->asset().name + " " + std::to_string(screens_[i]->stats().instances) + " instances / " +
				std::to_string(screens_[i]->stats().controllers) + " controllers;";
	}
	UtilityFunctions::print(to_godot("[hud] native HUD built in " + std::to_string(loaded_->seconds) + " s:" + summary));
}

bf3::core::Status Bf3NativeHud::open_view(size_t p_index) {
	static const char *const kNames[kNativeHudViews] = { "HudMp", "HudTdm", "Crosshair", "HudShared" };
	Bf3UiAssets *assets = Bf3UiAssets::current();
	if (!loaded_ || !loaded_->assets[p_index] || assets == nullptr || assets->movies() == nullptr) {
		return bf3::core::make_error(std::string(kNativeHudPartitions[p_index]) + ": no loaded screen asset or Bf3UiAssets movies");
	}
	const ui::HudScreenAsset &asset = *loaded_->assets[p_index];
	Bf3SwfView *v = memnew(Bf3SwfView);
	v->set_name(kNames[p_index]);
	v->set_playing(false);
	add_child(v);
	views_[p_index] = v;
	const bf3::core::Status st = v->open(asset.movie);
	if (!st) {
		drop_view(p_index);
		return bf3::core::make_error(asset.movie + ": " + st.error().message);
	}
	ui::HudStageRects rects;
	rects.visible = ui_stage(this).visible;
	rects.original = v->scene()->stage();
	rects.safe_x = ui::hud_safe_offset(ui::kHudFootageSafeX, rects.visible.width(), safe_area_.width);
	rects.safe_y = ui::hud_safe_offset(ui::kHudFootageSafeY, rects.visible.height(), safe_area_.height);
	bf3::core::Result<std::unique_ptr<ui::HudScreen>> s = ui::HudScreen::open(*v->scene(), *assets->movies(), asset, assets->swf_fonts(), client_, rects);
	if (!s) {
		drop_view(p_index);
		return bf3::core::make_error(asset.name + ": " + s.error().message);
	}
	screens_[p_index] = std::move(*s);
	effects_seen_[p_index] = 0;
	shown_[p_index] = true;
	++shown_count_[p_index];
	return bf3::core::Status::success();
}

namespace {

std::optional<size_t> native_hud_index(std::string_view p_partition) {
	for (size_t i = 0; i < kNativeHudViews; ++i) {
		if (kNativeHudPartitions[i] == p_partition) {
			return i;
		}
	}
	return std::nullopt;
}

}

bf3::core::Status Bf3NativeHud::open_screen(std::string_view p_partition) {
	const std::optional<size_t> i = native_hud_index(p_partition);
	if (!i) {
		return bf3::core::make_error(std::string(p_partition) + ": Bf3NativeHud hosts only the four in-round screens");
	}
	if (!built_ || failed_) {
		return bf3::core::make_error(std::string(p_partition) + ": the native HUD is not built");
	}
	if (screens_[*i] != nullptr) {
		return bf3::core::Status::success();
	}
	const bf3::core::Status st = open_view(*i);
	arrange();
	return st;
}

bool Bf3NativeHud::show_screen(std::string_view p_partition) {
	const std::optional<size_t> i = native_hud_index(p_partition);
	if (!i || views_[*i] == nullptr) {
		return false;
	}
	views_[*i]->set_visible(true);
	shown_count_[*i] += shown_[*i] ? 0 : 1;
	shown_[*i] = true;
	return true;
}

bool Bf3NativeHud::hide_screen(std::string_view p_partition) {
	const std::optional<size_t> i = native_hud_index(p_partition);
	if (!i || views_[*i] == nullptr) {
		return false;
	}
	views_[*i]->set_visible(false);
	shown_[*i] = false;
	return true;
}

bool Bf3NativeHud::order_screen(std::string_view p_partition, int32_t p_order) {
	const std::optional<size_t> i = native_hud_index(p_partition);
	if (!i) {
		return false;
	}
	order_[*i] = p_order;
	arrange();
	return true;
}

bool Bf3NativeHud::close_screen(std::string_view p_partition) {
	const std::optional<size_t> i = native_hud_index(p_partition);
	if (!i || (views_[*i] == nullptr && screens_[*i] == nullptr)) {
		return false;
	}
	screens_[*i].reset();
	drop_view(*i);
	shown_[*i] = false;
	effects_seen_[*i] = 0;
	bg_.reset();
	return true;
}

bool Bf3NativeHud::set_screen_modal(std::string_view p_partition, bool p_modal) {
	return !p_modal && native_hud_index(p_partition).has_value();
}

bool Bf3NativeHud::focus_screen(std::string_view) {
	return false;
}

void Bf3NativeHud::drop_view(size_t p_index) {
	if (Bf3SwfView *v = views_[p_index]) {
		remove_child(v);
		v->queue_free();
	}
	views_[p_index] = nullptr;
}

UiScreenSlot Bf3NativeHud::screen_slot(size_t p_index) const {
	if (p_index >= kNativeHudViews) {
		return {};
	}
	return UiScreenSlot{ kNativeHudPartitions[p_index], screens_[p_index].get(), views_[p_index], shown_[p_index], order_[p_index] };
}

bool Bf3NativeHud::reports(std::string_view p_partition) const {
	return built_ && !failed_ && native_hud_index(p_partition).has_value();
}

std::optional<UiScreenRecord> Bf3NativeHud::screen_record(std::string_view p_partition) const {
	const std::optional<size_t> i = reports(p_partition) ? native_hud_index(p_partition) : std::nullopt;
	if (!i) {
		return std::nullopt;
	}
	UiScreenRecord r;
	r.visible = views_[*i] != nullptr && shown_[*i] && is_visible_in_tree();
	r.shown = shown_count_[*i];
	if (screens_[*i] != nullptr) {
		add_screen_widget_facts(*screens_[*i], r);
	}
	return r;
}

void Bf3NativeHud::arrange() {
	std::array<size_t, kNativeHudViews> by_order{};
	for (size_t i = 0; i < kNativeHudViews; ++i) {
		by_order[i] = i;
	}
	std::stable_sort(by_order.begin(), by_order.end(), [this](size_t p_a, size_t p_b) { return order_[p_a] < order_[p_b]; });
	int32_t at = 0;
	for (const size_t i : by_order) {
		if (views_[i] != nullptr) {
			move_child(views_[i], at++);
		}
	}
}

std::optional<ui::MinimapLevel> Bf3NativeHud::minimap_level() {
	if (!minimap_ready()) {
		return std::nullopt;
	}
	std::shared_ptr<const MinimapAssets> a = minimap_source_->assets();
	return a != nullptr ? std::optional<ui::MinimapLevel>(a->level) : std::nullopt;
}

uint32_t Bf3NativeHud::minimap_combat_area_side() {
	if (!minimap_ready()) {
		return 0;
	}
	std::shared_ptr<const MinimapAssets> a = minimap_source_->assets();
	return a != nullptr ? a->combat_area_side : 0;
}

bool Bf3NativeHud::minimap_ready() {
	if (minimap_source_ == nullptr && is_inside_tree()) {
		const TypedArray<Node> found = get_tree()->get_root()->find_children("*", "Bf3MinimapHud", true, false);
		if (!found.is_empty()) {
			minimap_source_ = Object::cast_to<Bf3MinimapHud>(found[0]);
		}
	}
	if (minimap_source_ == nullptr || !minimap_source_->ready_to_draw() || minimap_source_->satellite_texture().is_null()) {
		return false;
	}
	if (minimap_material_.is_null()) {
		std::shared_ptr<const MinimapAssets> a = minimap_source_->assets();
		if (a == nullptr) {
			return false;
		}
		Ref<Shader> shader;
		shader.instantiate();
		shader->set_code(String::utf8(kMinimapMapShader.data(), int64_t(kMinimapMapShader.size())));
		minimap_material_.instantiate();
		minimap_material_->set_shader(shader);
		const ui::MinimapData &d = a->level.data;
		minimap_material_->set_shader_parameter("satellite", minimap_source_->satellite_texture());
		minimap_material_->set_shader_parameter("combat_area", minimap_source_->combat_area_texture());
		minimap_material_->set_shader_parameter("detail", minimap_source_->detail_texture());
		minimap_material_->set_shader_parameter("has_detail", minimap_source_->has_detail());
		minimap_material_->set_shader_parameter("mask", minimap_source_->mask_texture());
		minimap_material_->set_shader_parameter("has_mask", minimap_source_->mask_texture().is_valid());
		minimap_material_->set_shader_parameter("hatch", minimap_source_->hatch_texture());
		minimap_material_->set_shader_parameter("has_hatch", minimap_source_->hatch_texture().is_valid());
		minimap_material_->set_shader_parameter("tint", Color(d.detail_texture_tint[0], d.detail_texture_tint[1], d.detail_texture_tint[2], d.detail_texture_tint[3]));
		minimap_material_->set_shader_parameter("area_color", Color(d.combat_area_color[0], d.combat_area_color[1], d.combat_area_color[2], d.combat_area_color[3]));
		minimap_material_->set_shader_parameter("threshold", d.combat_area_alpha_threshold);
		minimap_material_->set_shader_parameter("detail_tint", Color(d.detail.color_tint[0], d.detail.color_tint[1], d.detail.color_tint[2], d.detail.color_tint[3]));
		minimap_material_->set_shader_parameter("detail_threshold", d.detail.alpha_threshold);
		minimap_material_->set_shader_parameter("opacity", a->level.profile.texture_opacity);
		minimap_material_->set_shader_parameter("satellite_weight", ui::kMinimapSatelliteWeight);
		minimap_material_->set_shader_parameter("world_center", Vector2(d.world_center[0], d.world_center[1]));
		minimap_material_->set_shader_parameter("world_size", d.world_size);
		minimap_material_->set_shader_parameter("hatch_wrap", Vector2(d.combat_area_multiply_wrap[0], d.combat_area_multiply_wrap[1]));
	}
	return true;
}

void Bf3NativeHud::_process(double p_delta) {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	if (!built_) {
		if (!started_) {
			Bf3Localisation *loc = Bf3Localisation::current();
			Bf3UiAssets *assets = Bf3UiAssets::current();
			if (loc == nullptr || assets == nullptr) {
				return;
			}
			if (loc->has_failed() || assets->has_failed()) {
				built_ = true;
				failed_ = true;
				error_ = "Bf3Localisation / Bf3UiAssets failed: " + to_std(loc->get_error()) + to_std(assets->get_error());
				UtilityFunctions::push_error(to_godot("[hud] native HUD not drawn: " + error_));
				return;
			}
			if (!loc->is_loaded() || !assets->is_loaded() || loc->install() == nullptr) {
				return;
			}
			started_ = true;
			start_loader();
			return;
		}
		if (!loader_done_.load(std::memory_order_acquire)) {
			return;
		}
		join_loader();
		build();
		if (failed_) {
			return;
		}
	}
	if (failed_ || manual_) {
		return;
	}
	step(p_delta);
}

void Bf3NativeHud::step(double p_seconds) {
	if (!built_ || failed_) {
		return;
	}
	const double dt = std::isfinite(p_seconds) && p_seconds > 0.0 ? p_seconds : 0.0;
	ekg_clock_ += dt;
	message_clock_ += dt;
	if (messages_ != nullptr) {
		messages_->update(model_, client_, message_clock_);
	}
	if (damage_ != nullptr) {
		damage_->update(model_, message_clock_);
	}
	bool client_changed = false;
	if (const std::shared_ptr<const MinimapAssets> a = !minimap_profile_applied_ && minimap_ready() ? minimap_source_->assets() : nullptr) {
		client_.minimap_small = a->level.profile.init_small;
		client_.minimap_zoom = ui::minimap_zoom(a->level.profile, a->level.profile.init_small).zoom;
		minimap_profile_applied_ = true;
		client_changed = true;
	}
	for (size_t i = 0; i < kNativeHudViews; ++i) {
		Bf3SwfView *v = views_[i];
		ui::HudScreen *s = screens_[i].get();
		if (v == nullptr || s == nullptr) {
			continue;
		}
		ui::HudStageRects rects = s->stage_rects();
		rects.visible = ui_stage(this).visible;
		rects.original = v->scene()->stage();
		rects.safe_x = ui::hud_safe_offset(ui::kHudFootageSafeX, rects.visible.width(), safe_area_.width);
		rects.safe_y = ui::hud_safe_offset(ui::kHudFootageSafeY, rects.visible.height(), safe_area_.height);
		s->set_stage_rects(rects);
		const float stage_h = rects.visible.height();
		if (client_changed || s->client().vertical_fov_deg != float(vfov_deg_) || s->client().stage_height != stage_h) {
			client_.vertical_fov_deg = float(vfov_deg_);
			client_.stage_height = stage_h;
			s->set_client(client_);
		}
		s->refresh(model_);
		stepped_serial_ = model_.serial;
		s->advance(dt);
		const bf3::core::Status st = v->scene()->advance_seconds(dt);
		if (!st) {
			UtilityFunctions::push_warning(to_godot("[hud] " + s->asset().name + ": " + st.error().message));
		}
		if (s->effects_revision() != effects_seen_[i]) {
			effects_seen_[i] = s->effects_revision();
			apply_hud_view_effects(*views_[i], *s);
		}
		paint(i);
	}
	if (tags_ != nullptr) {
		tags_->update(world_camera_, world_show_ && is_visible_in_tree(), ui_window_rect(this).size);
		paint_tags();
	}
}

uint64_t Bf3NativeHud::kill_feed_rows_added() const {
	uint64_t rows = 0;
	for (const std::unique_ptr<ui::HudScreen> &s : screens_) {
		if (s == nullptr) {
			continue;
		}
		for (const std::unique_ptr<ui::HudWidgetState> &w : s->widgets()) {
			const ui::HudMessageApi *api = dynamic_cast<const ui::HudMessageApi *>(w->controller.get());
			if (api != nullptr && w->screen_widget && api->source() == ui::HudMessageSource::Kill) {
				rows += api->rows_added();
			}
		}
	}
	return rows;
}

NativeHudReadout Bf3NativeHud::readout() const {
	NativeHudReadout r;
	r.serial = stepped_serial_;
	r.stage_height = client_.stage_height;
	r.vfov_deg = client_.vertical_fov_deg;
	const auto shown = [](const ui::SwfScene *p_scene, const std::string &p_path) {
		if (p_scene == nullptr) {
			return false;
		}
		for (size_t at = p_path.find('.'); ; at = p_path.find('.', at + 1)) {
			const ui::SwfSceneNode *n = p_scene->find_node(at == std::string::npos ? std::string_view(p_path) : std::string_view(p_path).substr(0, at));
			if (n == nullptr || !n->visible) {
				return false;
			}
			if (at == std::string::npos) {
				return true;
			}
		}
	};
	const ui::SwfScene *xh = views_[2] != nullptr ? views_[2]->scene() : nullptr;
	const std::string widget = "instance1.Crosshair_01";
	r.crosshair_widget = shown(xh, widget);
	if (xh != nullptr) {
		if (const ui::SwfSceneNode *c = xh->find_node(widget + ".crosshair")) {
			r.crosshair_alpha = c->visible ? c->color.mul[3] : 0.0f;
		}
		if (const ui::SwfSceneNode *arm = xh->find_node(widget + ".crosshair.right")) {
			r.crosshair_arm = arm->matrix.tx;
		}
	}
	if (screens_[2] != nullptr) {
		for (const ui::HudWidgetNode &n : screens_[2]->asset().widgets) {
			if (n.dispersion_base_offset) {
				r.crosshair_base_offset = *n.dispersion_base_offset;
				break;
			}
		}
	}
	const ui::SwfScene *mp = views_[0] != nullptr ? views_[0]->scene() : nullptr;
	const std::string clip = "instance1.Ammo_01.Ammo.clip.cltxt.txtDisplay";
	r.ammo_visible = shown(mp, clip);
	if (const ui::SwfSceneNode *t = mp != nullptr ? mp->find_node(clip) : nullptr) {
		r.clip_text = t->text;
	}
	if (const ui::SwfSceneNode *t = mp != nullptr ? mp->find_node("instance1.Ammo_01.Ammo.amholder.amtxt.txtDisplay") : nullptr) {
		r.reserve_text = t->text;
	}
	if (const ui::SwfSceneNode *t = mp != nullptr ? mp->find_node("instance1.Health_01.Health.healthHolder.amtxt.txtDisplay") : nullptr) {
		r.health_text = t->text;
	}
	const std::string hit = "instance1.Hitindicator_01.hitindicator";
	if (const ui::SwfSceneNode *h = mp != nullptr ? mp->find_node(hit) : nullptr) {
		r.hit_shown = shown(mp, hit) && h->frame > 0;
	}
	return r;
}

std::optional<ui::SwfRect> Bf3NativeHud::window_bounds(size_t p_view, std::string_view p_path) const {
	const Bf3SwfView *v = p_view < kNativeHudViews ? views_[p_view] : nullptr;
	const ui::SwfScene *sc = v != nullptr ? v->scene() : nullptr;
	const ui::SwfSceneNode *root = sc != nullptr ? sc->find_node(p_path) : nullptr;
	if (root == nullptr || !root->world_visible) {
		return std::nullopt;
	}
	std::optional<ui::SwfRect> out;
	std::vector<const ui::SwfSceneNode *> stack{ root };
	while (!stack.empty()) {
		const ui::SwfSceneNode *n = stack.back();
		stack.pop_back();
		if (!n->world_visible) {
			continue;
		}
		std::vector<ui::SwfPoint> local;
		const auto corners = [&local](const ui::SwfRect &p_r) {
			local.push_back({ p_r.x_min, p_r.y_min });
			local.push_back({ p_r.x_max, p_r.y_min });
			local.push_back({ p_r.x_min, p_r.y_max });
			local.push_back({ p_r.x_max, p_r.y_max });
		};
		if (n->shape != nullptr) {
			for (const ui::SwfFillMesh &mesh : n->shape->meshes) {
				local.insert(local.end(), mesh.outline.begin(), mesh.outline.end());
			}
			float half = 0.0f;
			for (const ui::SwfLineStyle &l : n->shape->lines) {
				half = std::max(half, 0.5f * l.width);
			}
			for (const ui::SwfStroke &stroke : n->shape->strokes) {
				for (const ui::SwfPoint &q : stroke.points) {
					corners(ui::SwfRect{ q.x - half, q.x + half, q.y - half, q.y + half });
				}
			}
		} else if (n->edit_text != nullptr) {
			corners(ui::swf_node_text_bounds(*n));
		}
		const ui::SwfMatrix &m = n->world;
		for (const ui::SwfPoint &q : local) {
			const Vector2 at =
					v->stage_to_canvas(ui::SwfPoint{ m.a * q.x + m.c * q.y + m.tx, m.b * q.x + m.d * q.y + m.ty });
			const float wx = at.x;
			const float wy = at.y;
			if (!out) {
				out = ui::SwfRect{ wx, wx, wy, wy };
			} else {
				out->x_min = std::min(out->x_min, wx);
				out->x_max = std::max(out->x_max, wx);
				out->y_min = std::min(out->y_min, wy);
				out->y_max = std::max(out->y_max, wy);
			}
		}
		for (const ui::SwfNodeId c : n->children) {
			if (const ui::SwfSceneNode *cn = sc->node(c)) {
				stack.push_back(cn);
			}
		}
	}
	return out;
}

namespace {

ui::HudIconState relation_state(ui::TagRelation p_relation) {
	switch (p_relation) {
		case ui::TagRelation::Friendly:
			return ui::HudIconState::Friendly;
		case ui::TagRelation::Squad:
			return ui::HudIconState::Squad;
		case ui::TagRelation::Enemy:
			return ui::HudIconState::Enemy;
		case ui::TagRelation::None:
			break;
	}
	return ui::HudIconState::Neutral;
}

constexpr std::string_view kPlayerIcon = ui::soldier_icon(false, 1, 1).icon;

void note_status(const bf3::core::Status &p_status, const std::string &p_path) {
	if (!p_status) {
		UtilityFunctions::push_warning(to_godot("[hud] engine target " + p_path + ": " + p_status.error().message));
	}
}

}

void Bf3NativeHud::paint(size_t p_index) {
	Bf3SwfView *v = views_[p_index];
	for (const SwfEngineTarget &t : v->engine_targets()) {
		if (!t.canvas_item.is_valid()) {
			continue;
		}
		if (t.renderer == kBgRenderer) {
			if (bg_.paint(*v, t)) {
				paint_.bg_panels++;
				paint_.paints++;
			}
		} else if (t.renderer == kEkgRenderer) {
			paint_ekg(t);
			if (!t.live) {
				note_status(v->set_engine_target_live(t.path, true), t.path);
			}
		} else if (t.renderer == kMinimapRenderer) {
			if (!minimap_ready()) {
				continue;
			}
			paint_minimap(t, *v->scene());
			if (!t.live) {
				note_status(v->set_engine_target_live(t.path, true), t.path);
			}
		}
	}
}

void Bf3NativeHud::paint_ekg(const SwfEngineTarget &p_target) {
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->canvas_item_clear(p_target.canvas_item);
	const ui::HudEkgLook &k = ui::kHudEkgLook;
	const ui::SwfRect &b = p_target.local_bounds;
	const float sx = b.width() / (k.rect_x1 - k.rect_x0);
	const float sy = b.height() / (k.rect_y1 - k.rect_y0);
	PackedVector2Array pts;
	PackedColorArray cols;
	const Color c(float(k.rgb[0]) / 255.0f, float(k.rgb[1]) / 255.0f, float(k.rgb[2]) / 255.0f, 1.0f);
	for (float x = std::max(k.rect_x0, k.fade_start_x); x <= k.rect_x1 + 1e-3f; x += 1.0f) {
		const float y = k.baseline_y - ui::hud_ekg_offset_px(k, ekg_clock_, x);
		pts.push_back(Vector2(b.x_min + (x - k.rect_x0) * sx, b.y_min + (y - k.rect_y0) * sy));
		cols.push_back(Color(c.r, c.g, c.b, ui::hud_ekg_alpha(k, x)));
	}
	if (pts.size() >= 2) {
		rs->canvas_item_add_polyline(p_target.canvas_item, pts, cols, k.line_px * sx, true);
	}
	paint_.ekg_traces++;
	paint_.paints++;
}

void Bf3NativeHud::paint_minimap(const SwfEngineTarget &p_target, const ui::SwfScene &p_scene) {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (minimap_parent_ != p_target.canvas_item) {
		for (RID *r : { &minimap_map_item_, &minimap_icon_item_ }) {
			if (r->is_valid()) {
				rs->free_rid(*r);
			}
			*r = rs->canvas_item_create();
			rs->canvas_item_set_parent(*r, p_target.canvas_item);
		}
		rs->canvas_item_set_draw_index(minimap_map_item_, 0);
		rs->canvas_item_set_draw_index(minimap_icon_item_, 1);
		rs->canvas_item_set_material(minimap_map_item_, minimap_material_->get_rid());
		minimap_parent_ = p_target.canvas_item;
	}
	const ui::SwfRect &b = p_target.local_bounds;
	ui::SwfAffine to_map;
	const ui::SwfScene *scene = &p_scene;
	const ui::SwfSceneNode *map = scene->node(p_target.node);
	const ui::SwfSceneNode *owner = map != nullptr ? scene->node(map->parent) : nullptr;
	while (owner != nullptr && owner->name != kMinimapClip) {
		owner = scene->node(owner->parent);
	}
	if (map != nullptr && owner != nullptr) {
		const ui::SwfAffine owner_world = ui::swf_affine(owner->world);
		const std::optional<ui::SwfAffine> map_inverse = ui::swf_inverse(ui::swf_affine(map->world));
		to_map = map_inverse ? ui::swf_concat(*map_inverse, owner_world) : owner_world;
	}
	const ui::SwfPoint p0 = ui::swf_apply(to_map, 0.0, 0.0);
	const ui::SwfPoint p1 = ui::swf_apply(to_map, ui::kMinimapSquareLocal, ui::kMinimapSquareLocal);
	const Vector2 q0(p0.x, p0.y);
	const Vector2 q1(p1.x, p1.y);
	const Vector2 sq0(std::min(q0.x, q1.x), std::min(q0.y, q1.y));
	const Vector2 sq1(std::max(q0.x, q1.x), std::max(q0.y, q1.y));
	const Vector2 centre = (sq0 + sq1) * 0.5f;
	const float mpu = ui::kMinimapMetresPerLocalUnit;
	const float unit = std::max(1e-6f, (sq1.x - sq0.x) / ui::kMinimapSquareLocal);
	minimap_material_->set_shader_parameter("local_rect", Vector4(b.x_min, b.y_min, b.width(), b.height()));
	minimap_material_->set_shader_parameter("square_rect", Vector4(sq0.x, sq0.y, sq1.x - sq0.x, sq1.y - sq0.y));
	minimap_material_->set_shader_parameter("centre_local", centre);
	minimap_material_->set_shader_parameter("player_xz", Vector2(pose_.x, pose_.z));
	minimap_material_->set_shader_parameter("heading", pose_.yaw);
	minimap_material_->set_shader_parameter("metres_per_unit", mpu / unit);
	rs->canvas_item_clear(minimap_map_item_);
	rs->canvas_item_add_rect(minimap_map_item_, Rect2(b.x_min, b.y_min, b.width(), b.height()), Color(1, 1, 1, 1));
	paint_.minimap_maps++;
	paint_.paints++;

	rs->canvas_item_clear(minimap_icon_item_);
	std::shared_ptr<const MinimapAssets> a = minimap_source_->assets();
	const Ref<ImageTexture> atlas = minimap_source_->icon_texture();
	if (a == nullptr || atlas.is_null()) {
		return;
	}
	ui::MinimapView view;
	view.center_x = centre.x;
	view.center_y = centre.y;
	view.pixels_per_metre = unit / mpu;
	view.player_x = pose_.x;
	view.player_z = pose_.z;
	view.heading = pose_.yaw;
	view.rotate = true;
	const auto draw = [&](const ui::HudIcon *p_icon, ui::HudIconState p_state, Vector2 p_at, float p_angle) {
		const ui::HudIconStateFrames *st = p_icon != nullptr ? p_icon->state(p_state) : nullptr;
		if (st == nullptr || st->frames.empty()) {
			return;
		}
		const ui::HudIconRect r = ui::hud_icon_rect(st->frames.front(), uint32_t(atlas->get_width()), uint32_t(atlas->get_height()));
		if (r.width <= 0 || r.height <= 0) {
			return;
		}
		const float w = float(r.width) * unit;
		const float h = float(r.height) * unit;
		rs->canvas_item_add_set_transform(minimap_icon_item_, Transform2D(p_angle, p_at));
		rs->canvas_item_add_texture_rect_region(minimap_icon_item_, Rect2(r.flip_x ? w * 0.5f : -w * 0.5f, r.flip_y ? h * 0.5f : -h * 0.5f, r.flip_x ? -w : w, r.flip_y ? -h : h),
				atlas->get_rid(), Rect2(float(r.x), float(r.y), float(r.width), float(r.height)));
		paint_.minimap_icons++;
	};
	for (const MinimapIconDraw &ic : minimap_source_->icon_draws()) {
		const ui::ScreenPoint p = ui::minimap_project(view, ic.world_x, ic.world_z);
		if (!std::isfinite(p.x) || !std::isfinite(p.y) || p.x < sq0.x || p.y < sq0.y || p.x > sq1.x || p.y > sq1.y) {
			continue;
		}
		const ui::HudIconStateFrames *st = ic.icon != nullptr ? ic.icon->state(ic.state) : nullptr;
		draw(ic.icon, ic.state, Vector2(p.x, p.y), st != nullptr && st->should_rotate ? view.heading - ic.yaw : 0.0f);
	}
	if (tags_ != nullptr) {
		const ui::HudIcon *player = a->atlas.find(kPlayerIcon);
		for (const IffMinimapEntry &e : tags_->minimap()) {
			const ui::ScreenPoint p = ui::minimap_project(view, e.feet[0], e.feet[2]);
			if (!std::isfinite(p.x) || !std::isfinite(p.y) || p.x < sq0.x || p.y < sq0.y || p.x > sq1.x || p.y > sq1.y) {
				continue;
			}
			const ui::HudIconState state = relation_state(e.relation);
			const ui::HudIconStateFrames *st = player != nullptr ? player->state(state) : nullptr;
			draw(player, state, Vector2(p.x, p.y), st != nullptr && st->should_rotate ? view.heading - e.yaw : 0.0f);
		}
	}
	const ui::HudIconChoice me = ui::local_player_icon(model_.squad.local_player_has_squad || model_.squad.local_squad_members.count > 0);
	draw(a->atlas.find(me.icon), me.state, centre, ui::minimap_marker_angle(view));
	rs->canvas_item_add_set_transform(minimap_icon_item_, Transform2D());
}

void Bf3NativeHud::paint_tags() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!tags_item_.is_valid()) {
		tags_item_ = rs->canvas_item_create();
		rs->canvas_item_set_parent(tags_item_, get_canvas_item());
		rs->canvas_item_set_draw_index(tags_item_, -1);
	}
	rs->canvas_item_clear(tags_item_);
	if (tags_ == nullptr || tags_->tags().empty() || !minimap_ready()) {
		return;
	}
	std::shared_ptr<const MinimapAssets> a = minimap_source_->assets();
	const Ref<ImageTexture> atlas = minimap_source_->icon_texture();
	if (a == nullptr || atlas.is_null()) {
		return;
	}
	if (tag_font_.is_null()) {
		if (Bf3UiAssets *u = Bf3UiAssets::current()) {
			tag_font_ = u->get_font(String("$Menu_medium"));
		}
	}
	const ui::NametagStyle &style = tags_->style();
	const float cs = tags_->canvas_scale();
	const ui::HudIcon *player = a->atlas.find(kPlayerIcon);
	const auto frame = [&](const ui::HudIcon *p_icon, ui::HudIconState p_state, Vector2 p_centre, float p_size, const Color &p_modulate) {
		const ui::HudIconStateFrames *st = p_icon != nullptr ? p_icon->state(p_state) : nullptr;
		if (st == nullptr && p_icon != nullptr) {
			st = p_icon->state(ui::HudIconState::Default);
		}
		if (st == nullptr || st->frames.empty()) {
			return;
		}
		const ui::HudIconRect r = ui::hud_icon_rect(st->frames.front(), uint32_t(atlas->get_width()), uint32_t(atlas->get_height()));
		if (r.width <= 0 || r.height <= 0) {
			return;
		}
		const float texel = std::min(1.0f, p_size / float(std::max(r.width, r.height))) * cs;
		const float w = float(r.width) * texel;
		const float h = float(r.height) * texel;
		rs->canvas_item_add_texture_rect_region(tags_item_,
				Rect2(r.flip_x ? p_centre.x + w * 0.5f : p_centre.x - w * 0.5f, r.flip_y ? p_centre.y + h * 0.5f : p_centre.y - h * 0.5f, r.flip_x ? -w : w,
						r.flip_y ? -h : h),
				atlas->get_rid(), Rect2(float(r.x), float(r.y), float(r.width), float(r.height)), p_modulate);
		paint_.tag_icons++;
	};
	const int font_size = std::max(6, int(std::lround(style.name_font_size * cs)));
	const int glow = std::max(1, int(std::lround(style.name_glow_size * 0.5f * cs)));
	for (const NativeTag &t : tags_->tags()) {
		const ui::HudIcon *kit = t.kit < tags_->kit_icons().size() && !tags_->kit_icons()[t.kit].empty() ? a->atlas.find(tags_->kit_icons()[t.kit]) : nullptr;
		const bool squad = t.relation == ui::TagRelation::Squad;
		const float icon_size = style.icon_size * t.scale;
		float top = t.anchor.y;
		if (t.icon) {
			if (squad && kit != nullptr) {
				frame(kit, ui::HudIconState::Squad, t.anchor, icon_size, Color(1, 1, 1, t.alpha));
			} else {
				frame(player, relation_state(t.relation), t.anchor, icon_size, Color(1, 1, 1, t.alpha));
			}
			top -= 0.5f * std::min(icon_size, 16.0f) * cs + 2.0f * cs;
		}
		if (!t.name || tag_font_.is_null() || t.text.is_empty()) {
			continue;
		}
		const std::array<float, 4> c = ui::tag_tint(style, t.relation);
		const Color tint(c[0], c[1], c[2], c[3] * t.alpha);
		const Vector2 ts = tag_font_->get_string_size(t.text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size);
		const ui::HudIcon *name_kit = t.relation != ui::TagRelation::Enemy ? kit : nullptr;
		const float kit_w = name_kit != nullptr ? 18.0f * cs : 0.0f;
		const float x0 = t.anchor.x - (kit_w + ts.x) * 0.5f;
		const float baseline = top - 2.0f * cs;
		if (name_kit != nullptr) {
			frame(name_kit, squad ? ui::HudIconState::Squad : ui::HudIconState::Default, Vector2(x0 + kit_w * 0.5f - 1.0f * cs, baseline - ts.y * 0.35f), 16.0f,
					squad ? Color(1, 1, 1, t.alpha) : tint);
		}
		tag_font_->draw_string_outline(tags_item_, Vector2(x0 + kit_w, baseline), t.text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, glow, Color(0, 0, 0, 0.55f * t.alpha));
		tag_font_->draw_string(tags_item_, Vector2(x0 + kit_w, baseline), t.text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, tint);
		paint_.tag_names++;
	}
	paint_.paints++;
}

void Bf3NativeHud::set_additive_coverage(bool p_coverage) {
	for (Bf3SwfView *v : views_) {
		if (v != nullptr) {
			v->set_additive_coverage(p_coverage);
		}
	}
}

std::array<Ref<Image>, kNativeHudViews> Bf3NativeHud::capture_views() const {
	std::array<Ref<Image>, kNativeHudViews> out;
	for (size_t i = 0; i < kNativeHudViews; ++i) {
		out[i] = views_[i] != nullptr ? views_[i]->capture_hud() : Ref<Image>();
	}
	return out;
}

Ref<Image> Bf3NativeHud::capture_layer(const std::array<Ref<Image>, kNativeHudViews> &p_colour, const std::array<Ref<Image>, kNativeHudViews> &p_coverage) {
	int32_t w = 0, h = 0;
	std::vector<double> acc;
	for (size_t i = 0; i < kNativeHudViews; ++i) {
		int32_t vw = 0, vh = 0, cw = 0, ch = 0;
		const std::vector<uint8_t> top = image_bytes(p_colour[i], vw, vh);
		const std::vector<uint8_t> cov = image_bytes(p_coverage[i], cw, ch);
		if (top.empty() && cov.empty()) {
			continue;
		}
		if (top.empty() || cov.empty() || cw != vw || ch != vh || (!acc.empty() && (vw != w || vh != h))) {
			return Ref<Image>();
		}
		if (acc.empty()) {
			w = vw;
			h = vh;
			acc.assign(top.size(), 0.0);
		}
		for (size_t p = 0; p + 3 < acc.size(); p += 4) {
			const double ta = double(top[p + 3]) / 255.0;
			const double ca = double(cov[p + 3]) / 255.0;
			for (size_t c = 0; c < 3; ++c) {
				acc[p + c] = double(top[p + c]) + acc[p + c] * (1.0 - ta);
			}
			acc[p + 3] = double(cov[p + 3]) + acc[p + 3] * (1.0 - ca);
		}
	}
	if (acc.empty()) {
		return Ref<Image>();
	}
	PackedByteArray bytes;
	bytes.resize(int64_t(acc.size()));
	uint8_t *out = bytes.ptrw();
	for (size_t p = 0; p + 3 < acc.size(); p += 4) {
		const long a = std::clamp(std::lround(acc[p + 3]), 0L, 255L);
		for (size_t c = 0; c < 3; ++c) {
			out[p + c] = a == 0 ? 0 : uint8_t(std::clamp(std::lround(acc[p + c] * 255.0 / double(a)), 0L, 255L));
		}
		out[p + 3] = uint8_t(a);
	}
	return Image::create_from_data(w, h, false, Image::FORMAT_RGBA8, bytes);
}

}

void openbf3_register_native_hud() {
	GDREGISTER_CLASS(bf3::godot_side::Bf3NativeHud);
}
