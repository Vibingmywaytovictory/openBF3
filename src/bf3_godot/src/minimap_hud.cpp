#include "minimap_hud.h"

#include "fp_camera.h"
#include "godot_util.h"
#include "input_mapper.h"
#include "level_session.h"
#include "minimap_control.h"
#include "ui_stage.h"
#include "world_icons.h"

#if OPENBF3_BUILD_CHECKS
#include "minimap_check.h"
#endif

#include "bf3/core/ebx_db.h"
#include "bf3/core/install.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/net/loopback_ping.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <psapi.h>

using namespace godot;
namespace ui = bf3::core::ui;
namespace core = bf3::core;

namespace bf3::godot_side {

namespace {

constexpr float kDefaultSidePer1080 = 256.0f;
constexpr float kRangeMetres = 150.0f;
constexpr float kMarginUnits = 14.0f;

uint64_t peak_ws() {
	PROCESS_MEMORY_COUNTERS pmc{};
	pmc.cb = sizeof(pmc);
	return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) ? uint64_t(pmc.PeakWorkingSetSize) : 0;
}

uint64_t current_ws() {
	PROCESS_MEMORY_COUNTERS pmc{};
	pmc.cb = sizeof(pmc);
	return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) ? uint64_t(pmc.WorkingSetSize) : 0;
}

using bf3::core::seconds_since;

PackedByteArray to_bytes(const uint8_t *p_data, size_t p_size) {
	PackedByteArray out;
	out.resize(int64_t(p_size));
	if (p_size > 0) {
		std::copy(p_data, p_data + p_size, out.ptrw());
	}
	return out;
}

Ref<ImageTexture> r8_texture(const ui::L8Image &p_image, uint8_t p_fill) {
	Ref<Image> img;
	if (p_image.empty()) {
		const uint8_t one = p_fill;
		img = Image::create_from_data(1, 1, false, Image::FORMAT_R8, to_bytes(&one, 1));
	} else {
		img = Image::create_from_data(int32_t(p_image.width), int32_t(p_image.height), false, Image::FORMAT_R8,
				to_bytes(p_image.texels.data(), p_image.texels.size()));
	}
	return img.is_valid() ? ImageTexture::create_from_image(img) : Ref<ImageTexture>();
}

Color vec4_colour(const std::array<float, 4> &p_v) {
	return Color(p_v[0], p_v[1], p_v[2], p_v[3]);
}

core::Result<ui::L8Image> load_value_texture(const core::LevelMount &p_mount, const std::string &p_name, std::string &r_note) {
	core::Result<core::res::LoadedTexture> t = core::res::load_texture(p_mount, p_name);
	if (!t) {
		return t.error();
	}
	std::vector<core::res::MipSlice> mips;
	const core::res::ResError e = t->mips(mips);
	if (!e.ok() || mips.empty()) {
		return core::make_error(p_name + ": no mip" + (e.ok() ? std::string() : ": " + e.message()));
	}
	std::vector<std::array<float, 4>> texels;
	if (!mips.front().texels(t->texture.format, texels) || texels.size() != size_t(mips.front().width) * mips.front().height) {
		return core::make_error(p_name + ": texture format " + std::to_string(int(t->texture.format)) + " not decoded");
	}
	bool alpha = false;
	for (const std::array<float, 4> &x : texels) {
		if (x[3] < 0.999f) {
			alpha = true;
			break;
		}
	}
	ui::L8Image out;
	out.width = mips.front().width;
	out.height = mips.front().height;
	out.texels.resize(texels.size());
	for (size_t i = 0; i < texels.size(); ++i) {
		const float v = alpha ? texels[i][3] : (texels[i][0] + texels[i][1] + texels[i][2]) / 3.0f;
		out.texels[i] = uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
	}
	r_note = p_name + " " + std::to_string(out.width) + "x" + std::to_string(out.height) + " format " + std::to_string(int(t->texture.format)) +
			(alpha ? " alpha" : " luminance");
	return out;
}

}

Bf3MinimapHud::Bf3MinimapHud() {
	set_layer(2);
}

Bf3MinimapHud::~Bf3MinimapHud() {
	stop_.store(true);
	if (loader_.joinable()) {
		loader_.join();
	}
}

void Bf3MinimapHud::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_session_path", "path"), &Bf3MinimapHud::set_session_path);
	ClassDB::bind_method(D_METHOD("get_session_path"), &Bf3MinimapHud::get_session_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "session_path"), "set_session_path", "get_session_path");
}

void Bf3MinimapHud::_notification(int p_what) {
	if (p_what == NOTIFICATION_PREDELETE) {
		stop_.store(true);
		if (loader_.joinable()) {
			loader_.join();
		}
	}
}

void Bf3MinimapHud::set_session_path(const NodePath &p_path) {
	session_path_ = p_path;
}

NodePath Bf3MinimapHud::get_session_path() const {
	return session_path_;
}

std::shared_ptr<const MinimapAssets> Bf3MinimapHud::assets() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return assets_;
}

std::optional<ui::L8Image> Bf3MinimapHud::combat_area_field() const {
	Ref<Image> img = combat_area_.is_valid() ? combat_area_->get_image() : Ref<Image>();
	if (img.is_null() || img->is_empty()) {
		return std::nullopt;
	}
	if (img->get_format() != Image::FORMAT_R8 && img->get_format() != Image::FORMAT_L8) {
		img->convert(Image::FORMAT_R8);
	}
	ui::L8Image l8;
	l8.width = uint32_t(img->get_width());
	l8.height = uint32_t(img->get_height());
	const PackedByteArray bytes = img->get_data();
	l8.texels.assign(bytes.ptr(), bytes.ptr() + bytes.size());
	return l8;
}

std::optional<ui::MinimapWorldRect> Bf3MinimapHud::combat_area_rect() {
	if (area_tried_ || !drawn_) {
		return area_;
	}
	area_tried_ = true;
	const std::shared_ptr<const MinimapAssets> a = assets();
	const std::optional<ui::L8Image> l8 = combat_area_field();
	if (a != nullptr && l8) {
		area_ = ui::combat_area_bounds(
				*l8, a->level.data.combat_area_alpha_threshold, ui::minimap_mapping(a->level.data));
	}
	return area_;
}

void Bf3MinimapHud::set_north_up(bool p_on) {
	north_up_ = p_on;
	update_view();
}

void Bf3MinimapHud::set_center_override(float p_x, float p_z) {
	center_override_ = true;
	center_override_x_ = p_x;
	center_override_z_ = p_z;
	update_view();
}

void Bf3MinimapHud::clear_center_override() {
	center_override_ = false;
	update_view();
}

void Bf3MinimapHud::parse_args() {
	mode_override_ = user_arg_value("minimap-mode");
	const std::string size = user_arg_value("minimap-size");
	if (!size.empty()) {
		size_px_ = std::clamp(std::atoi(size.c_str()), 64, 2048);
	}
	north_up_ = has_user_arg("minimap-north-up");
	check_ = has_check_arg("minimap-check");
	shots_dir_ = has_check_arg("minimap-shots") ? user_arg_value("minimap-shots") : std::string();
}

bool Bf3MinimapHud::inert_args() const {
	if (has_user_arg("no-minimap") || has_user_arg("no-hud")) {
		return true;
	}
	const bool measurement = has_user_arg("m1-load-check") || has_user_arg("shot-spawns") || has_user_arg("shots");
	return measurement && !(check_ || !shots_dir_.empty());
}

void Bf3MinimapHud::_ready() {
	parse_args();
	session_ = Object::cast_to<Bf3LevelSession>(get_node_or_null(session_path_));
	const bool headless = DisplayServer::get_singleton()->get_name() == String("headless");
	enabled_ = session_ != nullptr && !headless && session_->scene_enabled() && !inert_args();
	if (!enabled_) {
		set_process(false);
		set_process_unhandled_input(false);
		return;
	}
	set_process(true);
	set_process_unhandled_input(true);
}

void Bf3MinimapHud::_unhandled_input(const Ref<InputEvent> &p_event) {
	if (!drawn_) {
		return;
	}
	const InputEventKey *key = Object::cast_to<InputEventKey>(p_event.ptr());
	if (key != nullptr && key->is_pressed() && !key->is_echo() && key->get_keycode() == KEY_M) {
		set_north_up(!north_up_);
	}
}

void Bf3MinimapHud::run_loader() {
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	const auto start = std::chrono::steady_clock::now();
	const uint64_t peak_before = peak_ws();
	const uint64_t ws_before = current_ws();
	auto out = std::make_shared<MinimapAssets>();
	auto pixels = std::make_unique<MinimapPixels>();
	char buf[512];
	auto publish = [&]() {
		out->seconds = seconds_since(start);
		out->peak_working_set_delta = int64_t(peak_ws()) - int64_t(peak_before);
		out->working_set_delta = int64_t(current_ws()) - int64_t(ws_before);
		std::lock_guard<std::mutex> lock(mutex_);
		assets_ = out;
		pixels_ = out->error.empty() ? std::move(pixels) : nullptr;
		published_.store(true, std::memory_order_release);
	};
	core::Result<std::shared_ptr<core::Install>> install = core::Install::open(reader_);
	if (!install) {
		out->error = "install: " + install.error().message;
		publish();
		return;
	}
	const double t_install = seconds_since(start);
	ui::MinimapLoadOptions opts;
	opts.threads = ui::kMinimapHudLoaderThreads;
	core::Result<ui::MinimapLevel> level = ui::load_minimap_level(*install, level_id_, mode_id_, opts);
	if (!level) {
		out->error = "minimap level: " + level.error().message;
		publish();
		return;
	}
	const double t_level = seconds_since(start);
	if (stop_.load()) {
		return;
	}
	core::Result<core::LevelMount> mount = ui::mount_minimap_level(**install, level_id_);
	if (!mount) {
		out->error = "mount: " + mount.error().message;
		publish();
		return;
	}
	core::Result<ui::MinimapImages> images = ui::load_minimap_images(*mount, *level);
	if (!images) {
		out->error = "images: " + images.error().message;
		publish();
		return;
	}
	const double t_images = seconds_since(start);
	if (stop_.load()) {
		return;
	}
	out->satellite_side = images->satellite.width;
	pixels->satellite_mips = ui::l8_mip_chain(images->satellite);
	images->satellite = ui::L8Image{};
	core::Result<ui::L8Image> field = ui::combat_area_min_field(images->combat_areas);
	std::vector<ui::L8Image> one;
	if (field) {
		out->combat_area_side = field->width;
		one.push_back(std::move(*field));
	}
	images->combat_areas = std::move(one);
	images->surroundings.clear();
	pixels->images = std::move(*images);
	const double t_mips = seconds_since(start);
	if (stop_.load()) {
		return;
	}
	core::Result<std::unique_ptr<core::EbxLibrary>> ui_lib = core::EbxLibrary::open(*install, { "Win32/UI" }, ui::kMinimapHudLoaderThreads);
	if (!ui_lib) {
		out->error = "Win32/UI: " + ui_lib.error().message;
		publish();
		return;
	}
	core::Result<ui::HudIconAtlas> atlas = ui::read_hud_icon_atlas(**ui_lib);
	if (!atlas) {
		out->error = "atlas: " + atlas.error().message;
		publish();
		return;
	}
	core::Result<ui::Rgba8Image> atlas_img = ui::load_hud_icon_texture((*ui_lib)->mount(0), *atlas);
	if (!atlas_img) {
		out->error = "atlas texture: " + atlas_img.error().message;
		publish();
		return;
	}
	out->atlas_width = atlas_img->width;
	out->atlas_height = atlas_img->height;

	if (const ui::HudIcon *me = atlas->find(ui::soldier_icon(true, 0, 0).icon)) {
		if (const ui::HudIconStateFrames *st = me->state(ui::HudIconState::Default); st != nullptr && !st->frames.empty()) {
			const ui::HudIconRect r = ui::hud_icon_rect(st->frames.front(), atlas_img->width, atlas_img->height);
			int32_t x0 = r.width, y0 = r.height, x1 = -1, y1 = -1;
			for (int32_t y = 0; y < r.height; ++y) {
				for (int32_t x = 0; x < r.width; ++x) {
					const int64_t ax = int64_t(r.x) + x;
					const int64_t ay = int64_t(r.y) + y;
					if (ax < 0 || ay < 0 || ax >= int64_t(atlas_img->width) || ay >= int64_t(atlas_img->height)) {
						continue;
					}
					if (atlas_img->rgba[(size_t(ay) * atlas_img->width + size_t(ax)) * 4 + 3] > 0) {
						x0 = std::min(x0, x);
						y0 = std::min(y0, y);
						x1 = std::max(x1, x);
						y1 = std::max(y1, y);
					}
				}
			}
			if (x1 >= x0 && y1 >= y0) {

				const float dx = 0.5f * float(x0 + x1 + 1) - 0.5f * float(r.width);
				const float dy = 0.5f * float(y0 + y1 + 1) - 0.5f * float(r.height);
				out->marker_alpha_dx = r.flip_x ? -dx : dx;
				out->marker_alpha_dy = r.flip_y ? -dy : dy;
			}
		}
	}
	const auto value_texture = [&](const std::string &p_name, ui::L8Image &r_image, std::string &r_note) {
		if (p_name.empty()) {
			r_note = "no texture named";
			return;
		}
		const std::array<const core::LevelMount *, 2> mounts{ &*mount, &(*ui_lib)->mount(0) };
		for (const core::LevelMount *m : mounts) {
			core::Result<ui::L8Image> img = load_value_texture(*m, p_name, r_note);
			if (img) {
				r_image = std::move(*img);
				return;
			}
			r_note = img.error().message;
		}
	};
	value_texture(level->data.fade_texture, pixels->mask, out->mask_note);
	value_texture(level->data.combat_area_multiply_texture, pixels->hatch, out->hatch_note);
	pixels->atlas_image = std::move(*atlas_img);
	out->atlas = std::move(*atlas);
	out->level = std::move(*level);
	const double t_atlas = seconds_since(start);
	std::snprintf(buf, sizeof(buf), "install %.2f s, level %.2f s, images %.2f s, mips + field %.2f s, Win32/UI atlas %.2f s", t_install, t_level - t_install,
			t_images - t_level, t_mips - t_images, t_atlas - t_mips);
	out->steps = buf;
	publish();
}

void Bf3MinimapHud::build_godot_side() {
	std::shared_ptr<const MinimapAssets> a;
	std::unique_ptr<MinimapPixels> px;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		a = assets_;
		px = std::move(pixels_);
	}
	if (a == nullptr) {
		return;
	}
	if (!a->error.empty() || px == nullptr) {
		UtilityFunctions::push_error(to_godot("[ui2] minimap not drawn: " + a->error));
		drawn_ = false;
		return;
	}
	UtilityFunctions::print(to_godot("[ui2] minimap " + a->level.data.level_id + " " + a->level.mode + ": " + std::to_string(a->level.objectives.size()) +
			" objectives, " + std::to_string(a->level.combat_areas.size()) + " combat areas; loader " + std::to_string(a->seconds) + " s (" + a->steps +
			"); mask " + a->mask_note + "; hatch " + a->hatch_note));

	if (px->satellite_mips.empty()) {
		UtilityFunctions::push_error("[ui2] minimap not drawn: empty satellite");
		return;
	}
	size_t total = 0;
	for (const ui::L8Image &m : px->satellite_mips) {
		total += m.texels.size();
	}
	PackedByteArray sat;
	sat.resize(int64_t(total));
	{
		uint8_t *w = sat.ptrw();
		for (const ui::L8Image &m : px->satellite_mips) {
			w = std::copy(m.texels.begin(), m.texels.end(), w);
		}
	}
	const ui::L8Image &mip0 = px->satellite_mips.front();
	Ref<Image> sat_img = Image::create_from_data(int32_t(mip0.width), int32_t(mip0.height), px->satellite_mips.size() > 1, Image::FORMAT_L8, sat);
	if (sat_img.is_null()) {
		UtilityFunctions::push_error("[ui2] minimap not drawn: satellite upload refused");
		return;
	}
	satellite_ = ImageTexture::create_from_image(sat_img);
	combat_area_ = r8_texture(px->images.combat_areas.empty() ? ui::L8Image{} : px->images.combat_areas.front(), 0);
	const bool has_detail = !px->images.detail.empty();
	detail_ = r8_texture(px->images.detail, 255);
	has_detail_ = has_detail;
	mask_ = px->mask.empty() ? Ref<ImageTexture>() : r8_texture(px->mask, 255);
	hatch_ = px->hatch.empty() ? Ref<ImageTexture>() : r8_texture(px->hatch, 255);
	Ref<Image> atlas_img = Image::create_from_data(int32_t(px->atlas_image.width), int32_t(px->atlas_image.height), false, Image::FORMAT_RGBA8,
			to_bytes(px->atlas_image.rgba.data(), px->atlas_image.rgba.size()));
	if (atlas_img.is_null()) {
		UtilityFunctions::push_error("[ui2] minimap not drawn: atlas upload refused");
		return;
	}
	atlas_ = ImageTexture::create_from_image(atlas_img);
	px.reset();

	Ref<Shader> shader;
	shader.instantiate();
	shader->set_code(String::utf8(kMinimapShader.data(), int64_t(kMinimapShader.size())));
	Ref<ShaderMaterial> material;
	material.instantiate();
	material->set_shader(shader);
	const ui::MinimapData &d = a->level.data;
	material->set_shader_parameter("satellite", satellite_);
	material->set_shader_parameter("combat_area", combat_area_);
	material->set_shader_parameter("detail", detail_);
	material->set_shader_parameter("tint", vec4_colour(d.detail_texture_tint));
	material->set_shader_parameter("area_color", vec4_colour(d.combat_area_color));
	material->set_shader_parameter("threshold", d.combat_area_alpha_threshold);
	material->set_shader_parameter("detail_tint", vec4_colour(d.detail.color_tint));
	material->set_shader_parameter("detail_threshold", d.detail.alpha_threshold);
	material->set_shader_parameter("has_detail", has_detail);

	icons_ = memnew(Bf3WorldIcons);
	icons_->set_name("WorldIcons");
	add_child(icons_);
	icons_->setup(atlas_, &a->atlas, WorldIconStyle{});
	if (!session_->game_session()) {
		control_ = memnew(Bf3MinimapControl);
		control_->set_name("Minimap");
		add_child(control_);
		control_->setup(d, satellite_, material, atlas_, &a->atlas);
	}
	drawn_ = true;
	layout_controls();
	update_view();
}

void Bf3MinimapHud::layout_controls() {
	const Rect2 vis = ui_window_rect(this);
	if (control_ != nullptr) {
		const float scale = std::max(1e-3f, std::fabs(get_viewport()->get_final_transform().get_scale().y));
		const float side = size_px_ > 0 ? float(size_px_) / scale : kDefaultSidePer1080 / 1080.0f * vis.size.y;
		control_->set_position(Vector2(std::round(vis.position.x + kMarginUnits), std::round(vis.position.y + vis.size.y - side - kMarginUnits)));
		control_->set_size(Vector2(side, side));
	}
	if (icons_ != nullptr) {
		icons_->set_position(vis.position);
		icons_->set_size(vis.size);
	}
}

void Bf3MinimapHud::update_view() {
	if (!drawn_ || session_ == nullptr) {
		return;
	}
	std::shared_ptr<const MinimapAssets> a = assets();
	layout_controls();
	if (control_ != nullptr) {
		const Vector2 size = control_->get_size();
		const Vector3 eye = session_->cameras().last_eye();
		ui::MinimapView v;
		v.center_x = size.x * 0.5f;
		v.center_y = size.y * 0.5f;
		v.pixels_per_metre = std::min(size.x, size.y) / (2.0f * kRangeMetres);
		v.player_x = center_override_ ? center_override_x_ : eye.x;
		v.player_z = center_override_ ? center_override_z_ : eye.z;
		v.heading = session_->mapper().view().yaw;
		v.rotate = !north_up_;
		view_ = v;
		control_->set_view(v);
	}

	std::vector<MinimapIconDraw> objectives;
	objectives.reserve(a->level.objectives.size());
	for (const ui::MinimapObjective &o : a->level.objectives) {
		const ui::HudIconChoice c = ui::objective_icon(o.kind == ui::ObjectiveKind::Hq, o.initial_owner, kNoMatchLocalTeam);
		MinimapIconDraw ic;
		ic.icon = a->atlas.find(c.icon);
		ic.state = c.state;
		ic.world_x = o.position[0];
		ic.world_y = o.position[1];
		ic.world_z = o.position[2];
		ic.yaw = o.yaw;
		ic.objective = true;
		objectives.push_back(ic);
	}
	std::vector<MinimapIconDraw> all = objectives;
	const bf3::host::PresentationFrame &f = session_->frame();

	if (!f.match_valid && f.valid && f.other_count > 0) {
		std::array<bf3::host::PresentedSoldier, bf3::net::kMaxSlots> out{};
		const size_t n = bf3::host::present_others(f, bf3::net::monotonic_ns(), out);
		const ui::HudIconChoice c = ui::soldier_icon(false, 2, kNoMatchLocalTeam);
		for (size_t i = 0; i < n; ++i) {
			MinimapIconDraw ic;
			ic.icon = a->atlas.find(c.icon);
			ic.state = c.state;
			ic.world_x = out[i].position[0];
			ic.world_y = out[i].position[1];
			ic.world_z = out[i].position[2];
			ic.yaw = out[i].yaw;
			ic.objective = false;
			all.push_back(ic);
		}
	}
	icon_draws_ = all;
	const bool fp = !session_->cameras().free();
	if (control_ != nullptr) {
		control_->set_icons(std::move(all));
		control_->set_visible(fp);
	}
	const Camera3D *cam = fp ? session_->cameras().first_person() : session_->cameras().free_camera();
	icons_->update(cam, std::move(objectives));
}

void Bf3MinimapHud::_process([[maybe_unused]] double p_delta) {
	if (!enabled_ || session_ == nullptr) {
		return;
	}
	if (!loader_started_) {
		if (!session_->done() || session_->failed()) {
			return;
		}
		loader_started_ = true;
		reader_ = session_->install();
		level_id_ = session_->level_id();
		mode_id_ = mode_override_.empty() ? session_->mode_id() : mode_override_;
		loader_ = std::thread([this]() { run_loader(); });
		return;
	}
	if (!drawn_) {
		if (published_.load(std::memory_order_acquire) && !build_attempted_) {
			build_attempted_ = true;
			build_godot_side();
			if (!drawn_) {
#if OPENBF3_BUILD_CHECKS
				if (check_) {

					check_runner_ = std::make_unique<MinimapCheck>(*this, shots_dir_, true);
				} else {
					enabled_ = false;
				}
#else
				enabled_ = false;
#endif
			}
		}
#if OPENBF3_BUILD_CHECKS
		if (check_runner_ != nullptr && check_runner_->step(p_delta)) {
			session_->finish(check_runner_->exit_code());
			check_runner_.reset();
		}
#endif
		return;
	}
	update_view();
#if OPENBF3_BUILD_CHECKS
	if (check_runner_ == nullptr && (check_ || !shots_dir_.empty())) {
		check_runner_ = std::make_unique<MinimapCheck>(*this, shots_dir_, check_);
	}
	if (check_runner_ != nullptr && check_runner_->step(p_delta)) {
		const int code = check_runner_->exit_code();
		check_runner_.reset();
		session_->finish(code);
		enabled_ = false;
	}
#endif
}

}
