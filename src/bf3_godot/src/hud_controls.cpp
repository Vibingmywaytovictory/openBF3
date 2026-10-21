#include "hud_controls.h"

#include "bf3_localisation.h"
#include "godot_util.h"
#include "ui_assets.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/level/budget.h"
#include "bf3/core/ui/swf_atlas.h"
#include "bf3/core/ui/swf_layout.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/font_file.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;
namespace host = bf3::host;

namespace {

constexpr double kPi = 3.14159265358979323846;

PackedByteArray byte_array(const std::vector<uint8_t> &p_data) {
	PackedByteArray out;
	out.resize(int64_t(p_data.size()));
	if (!p_data.empty()) {
		std::copy(p_data.begin(), p_data.end(), out.ptrw());
	}
	return out;
}

bool image_format(bf3::core::res::TextureFormat p_format, Image::Format &r_format) {
	using bf3::core::res::TextureFormat;
	switch (p_format) {
		case TextureFormat::Dxt1:
			r_format = Image::FORMAT_DXT1;
			return true;
		case TextureFormat::Dxt3:
			r_format = Image::FORMAT_DXT3;
			return true;
		case TextureFormat::Dxt5:
			r_format = Image::FORMAT_DXT5;
			return true;
		case TextureFormat::Argb8888:
			r_format = Image::FORMAT_RGBA8;
			return true;
		default:
			return false;
	}
}

Color swf_colour(const ui::SwfColor &p_c) {
	return Color(float(p_c.r) / 255.0f, float(p_c.g) / 255.0f, float(p_c.b) / 255.0f, float(p_c.a) / 255.0f);
}

host::HudTextSlot fallback_slot(const char *p_path, float p_x, float p_y, float p_w, float p_h, float p_size, uint8_t p_align, const char *p_text) {
	host::HudTextSlot s;
	s.path = p_path;
	s.x = p_x;
	s.y = p_y;
	s.bounds.x_min = -2.0f;
	s.bounds.x_max = p_w;
	s.bounds.y_min = -2.0f;
	s.bounds.y_max = p_h;
	s.size = p_size;
	s.align = p_align;
	s.font = std::string(host::kHudNumberFont);
	s.color = ui::SwfColor{ 255, 255, 255, 255 };
	s.initial_text = p_text;
	return s;
}

host::HudScreenLayout make_fallback_layout() {
	host::HudScreenLayout l;
	l.ammo = { false, "Ammo_01", 1194.0f, 669.0f, ui::HudAnchor::BottomRight };
	l.health = { false, "Health_01", 1193.0f, 605.0f, ui::HudAnchor::BottomRight };
	l.hit_indicator = { false, "Hitindicator_01", 640.0f, 360.0f, ui::HudAnchor::Centre };
	l.ammo_clip = fallback_slot("Ammo_01/Ammo/clip/cltxt/txtDisplay", 1094.0f, 625.0f, 102.0f, 56.6f, 43.0f, 1, "");
	l.ammo_reserve = fallback_slot("Ammo_01/Ammo/amholder/amtxt/txtDisplay", 1199.0f, 648.0f, 49.0f, 27.0f, 21.0f, 1, "");
	l.ammo_slash = fallback_slot("Ammo_01 /", 1194.0f, 649.0f, 12.0f, 24.0f, 18.0f, 0, "/");
	l.health_value = fallback_slot("Health_01/Health/healthHolder/amtxt/txtDisplay", 1200.6f, 588.0f, 52.0f, 27.0f, 21.0f, 1, "");
	l.health_percent = fallback_slot("Health_01 %", 1253.7f, 593.0f, 16.0f, 20.0f, 16.0f, 0, "%");
	return l;
}

void draw_slot_text(CanvasItem &p_item, const Ref<Font> &p_font, const host::HudTextSlot &p_slot, ui::HudAnchor p_anchor, float p_ascent,
		const std::string &p_text, const Color &p_colour, float p_width, float p_height, int32_t &r_size, Vector2 &r_at) {
	const float s = host::hud_scale(p_width, p_height);
	const host::HudTextAnchor ta = host::hud_text_anchor(p_slot, p_ascent);
	const host::HudPoint p = host::hud_stage_to_window(p_anchor, ta.x, ta.baseline, p_width, p_height);
	r_size = std::max(1, int32_t(std::lround(ta.size * s)));
	r_at = Vector2(p.x, p.y);
	if (p_font.is_null() || p_text.empty()) {
		return;
	}
	const String text = to_godot(p_text);
	const float tw = float(p_font->get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, r_size).x);
	float x0 = p.x;
	if (ta.align == 1) {
		x0 = p.x - tw;
	} else if (ta.align == 2) {
		x0 = p.x - 0.5f * tw;
	}
	const float shadow = std::max(1.0f, std::round(s));
	p_item.draw_string(p_font, Vector2(x0 + shadow, p.y + shadow), text, HORIZONTAL_ALIGNMENT_LEFT, -1, r_size, kHudShadowColor);
	p_item.draw_string(p_font, Vector2(x0, p.y), text, HORIZONTAL_ALIGNMENT_LEFT, -1, r_size, p_colour);
}

Color slot_colour(const host::HudTextSlot &p_slot) {
	return p_slot.found && p_slot.color.a > 0 ? swf_colour(p_slot.color) : kHudTextColor;
}

}

struct Bf3Hud::Loaded {
	bool layout_ok = false;
	host::HudScreenLayout layout;
	std::string layout_error;
	std::unique_ptr<ui::ItemIconIndex> index;
	std::string icon_error;
	struct Icon {
		std::vector<uint32_t> items;
		ui::ItemIconImage image;
	};
	std::vector<Icon> icons;
	size_t icons_failed = 0;
	std::vector<std::string> log;
	double seconds = 0;
};

Bf3Hud::Bf3Hud() {
	fallback_ = make_fallback_layout();
}

Bf3Hud::~Bf3Hud() {
	join_loader();
}

void Bf3Hud::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_vfov_deg", "degrees"), &Bf3Hud::set_vfov_deg);
	ClassDB::bind_method(D_METHOD("is_loaded"), &Bf3Hud::is_loaded);
	ClassDB::bind_static_method("Bf3Hud", D_METHOD("required_atlas_movies"), &Bf3Hud::required_atlas_movies);
	ADD_SIGNAL(MethodInfo("hud_loaded", PropertyInfo(Variant::BOOL, "ok")));
}

PackedStringArray Bf3Hud::required_atlas_movies() {
	PackedStringArray out;
	out.push_back("ui/assets/iconsingame");
	out.push_back("ui/assets/hudtextures");
	out.push_back("ui/assets/compass");
	out.push_back("ui/assets/ammo");
	out.push_back(to_godot(std::string(host::kHitIndicatorMovie)));
	return out;
}

void Bf3Hud::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	set_anchors_and_offsets_preset(PRESET_FULL_RECT);
	set_mouse_filter(MOUSE_FILTER_IGNORE);
	crosshair_ = memnew(Bf3HudCrosshair);
	hit_ = memnew(Bf3HudHitIndicator);
	ammo_ = memnew(Bf3HudAmmo);
	name_ = memnew(Bf3HudWeaponName);
	health_ = memnew(Bf3HudHealth);
	Bf3HudWidget *widgets[] = { crosshair_, hit_, ammo_, name_, health_ };
	const char *names[] = { "Crosshair", "HitIndicator", "Ammo", "WeaponName", "Health" };
	for (size_t i = 0; i < std::size(widgets); ++i) {
		widgets[i]->set_name(names[i]);
		widgets[i]->set_hud(this);
		widgets[i]->set_anchors_and_offsets_preset(PRESET_FULL_RECT);
		widgets[i]->set_mouse_filter(MOUSE_FILTER_IGNORE);
		add_child(widgets[i]);
	}
	set_process(true);
}

void Bf3Hud::_exit_tree() {
	join_loader();
}

void Bf3Hud::join_loader() {
	if (loader_.joinable()) {
		loader_.join();
	}
}

void Bf3Hud::_process(double p_delta) {
	(void)p_delta;
	if (finished_) {
		return;
	}
	if (!loader_.joinable()) {
		Bf3Localisation *loc = Bf3Localisation::current();
		Bf3UiAssets *assets = Bf3UiAssets::current();
		if (loc == nullptr || assets == nullptr) {
			error_ = "no Bf3Localisation / Bf3UiAssets in the scene";
			finish_load();
			return;
		}
		if ((!loc->is_loaded() && !loc->has_failed()) || (!assets->is_loaded() && !assets->has_failed())) {
			return;
		}
		resolve_assets();
		if (!loc->install() || assets->movies() == nullptr) {
			error_ = "no usable install (" + to_std(loc->get_error()) + to_std(assets->get_error()) + ")";
			finish_load();
			return;
		}
		start_loader();
		return;
	}
	if (loader_done_.load(std::memory_order_acquire)) {
		join_loader();
		finish_load();
	}
}

void Bf3Hud::resolve_assets() {
	if (assets_resolved_) {
		return;
	}
	assets_resolved_ = true;
	Bf3UiAssets *assets = Bf3UiAssets::current();
	if (assets == nullptr || !assets->is_loaded()) {
		return;
	}
	const String number = to_godot(std::string(host::kHudNumberFont));
	const String text = to_godot(std::string(host::kHudTextFont));
	number_font_ = assets->get_font(number);
	text_font_ = assets->get_font(text);
	const Ref<FontFile> number_file = assets->get_font_file(number);
	const Ref<FontFile> text_file = assets->get_font_file(text);
	number_font_name_ = std::string(host::kHudNumberFont) + "=" + (number_file.is_valid() ? to_std(number_file->get_font_name()) : std::string("system"));
	text_font_name_ = std::string(host::kHudTextFont) + "=" + (text_file.is_valid() ? to_std(text_file->get_font_name()) : std::string("system"));
	hit_levels_.clear();
	const ui::SwfAtlasMap *map = assets->atlas_map(std::string(host::kHitIndicatorMovie));
	for (std::string_view region : host::kHitIndicatorRegions) {
		std::vector<HitPiece> pieces;
		const ui::SwfAtlasRegion *r = map != nullptr ? map->find(region) : nullptr;
		if (r != nullptr) {
			for (const ui::SwfAtlasPiece &p : r->pieces) {
				HitPiece hp;
				hp.texture = make_atlas_region(assets->atlas_texture(map->movie, p.image_id), p);
				hp.transform = Transform2D(p.placement.a, p.placement.b, p.placement.c, p.placement.d, p.placement.tx, p.placement.ty);
				if (hp.texture.is_valid()) {
					pieces.push_back(hp);
				}
			}
		}
		hit_levels_.push_back(std::move(pieces));
	}
	if (map == nullptr) {
		UtilityFunctions::push_warning(
				to_godot("Bf3Hud: no atlas of " + std::string(host::kHitIndicatorMovie) + " (Bf3UiAssets needs Bf3Hud.required_atlas_movies()); drawing the fallback hit indicator"));
	}
}

void Bf3Hud::start_loader() {
	Bf3Localisation *loc = Bf3Localisation::current();
	Bf3UiAssets *assets = Bf3UiAssets::current();
	std::shared_ptr<const bf3::core::Install> install = loc->install();
	const ui::SwfLibrary *movies = assets->movies();
	loaded_.reset();
	loader_done_.store(false, std::memory_order_relaxed);
	loader_ = std::thread([this, install, movies] {
		const auto t0 = std::chrono::steady_clock::now();
		auto out = std::make_unique<Loaded>();
		{
			bf3::core::level::WorkBudget budget(ui::kSwfLayoutBudgetUnits);
			bf3::core::Result<ui::SwfLayoutNode> tree = ui::read_layout(*movies, host::kHudScreenMovie, {}, budget);
			if (!tree) {
				out->layout_error = tree.error().message;
			} else {
				bf3::core::gameplay::WorkBudget lb;
				bf3::core::Result<host::HudScreenLayout> l = host::make_hud_screen_layout(*tree, lb);
				if (!l) {
					out->layout_error = l.error().message;
				} else {
					out->layout = std::move(*l);
					out->layout_ok = true;
				}
			}
		}
		bf3::core::Result<std::unique_ptr<bf3::core::EbxLibrary>> ebx = bf3::core::EbxLibrary::open(install, { "Win32/UI" });
		if (!ebx) {
			out->icon_error = "Win32/UI: " + ebx.error().message;
		} else {
			bf3::core::Result<ui::ItemIconIndex> items = ui::ItemIconIndex::read(**ebx);
			if (!items) {
				out->icon_error = "item index: " + items.error().message;
			} else {
				out->index = std::make_unique<ui::ItemIconIndex>(std::move(*items));
				std::map<std::string, size_t> by_res;
				for (const ui::ItemIcon &icon : out->index->icons()) {
					if (icon.source == ui::ItemIconSource::Vehicle || icon.res_name.empty()) {
						continue;
					}
					auto it = by_res.find(icon.res_name);
					if (it != by_res.end()) {
						if (it->second != SIZE_MAX) {
							out->icons[it->second].items.push_back(icon.item_id);
						}
						continue;
					}
					bf3::core::Result<ui::ItemIconImage> img = ui::load_item_icon((*ebx)->mount(0), icon);
					if (!img) {
						by_res.emplace(icon.res_name, SIZE_MAX);
						++out->icons_failed;
						if (out->log.size() < 8) {
							out->log.push_back(icon.res_name + ": " + img.error().message);
						}
						continue;
					}
					by_res.emplace(icon.res_name, out->icons.size());
					Loaded::Icon li;
					li.items.push_back(icon.item_id);
					li.image = std::move(*img);
					out->icons.push_back(std::move(li));
				}
			}
		}
		out->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		loaded_ = std::move(out);
		loader_done_.store(true, std::memory_order_release);
	});
}

void Bf3Hud::finish_load() {
	finished_ = true;
	set_process(false);
	resolve_assets();
	bool ok = error_.empty();
	if (loaded_) {
		Loaded &l = *loaded_;
		if (l.layout_ok) {
			layout_ = l.layout;
			layout_ok_ = true;
			if (!layout_.missing.empty()) {
				std::string m;
				for (const std::string &p : layout_.missing) {
					m += " " + p;
				}
				UtilityFunctions::push_warning(to_godot("Bf3Hud: hudmpscreen slots missing (fallback placement):" + m));
			}
		} else {
			UtilityFunctions::push_warning(to_godot("Bf3Hud: no hudmpscreen layout (" + l.layout_error + "); fallback placement"));
			ok = false;
		}
		if (!l.icon_error.empty()) {
			UtilityFunctions::push_warning(to_godot("Bf3Hud: no item icons (" + l.icon_error + ")"));
			error_ = l.icon_error;
			ok = false;
		}
		for (const std::string &m : l.log) {
			UtilityFunctions::push_warning(to_godot("Bf3Hud: icon " + m));
		}
		icons_failed_ = l.icons_failed;
		for (Loaded::Icon &icon : l.icons) {
			Image::Format format = Image::FORMAT_RGBA8;
			Ref<ImageTexture> tex;
			if (image_format(icon.image.format, format)) {
				const Ref<Image> img = Image::create_from_data(int32_t(icon.image.width), int32_t(icon.image.height), false, format, byte_array(icon.image.data));
				if (img.is_valid() && !img->is_empty()) {
					tex = ImageTexture::create_from_image(img);
				}
			}
			if (tex.is_null()) {
				++icons_failed_;
				continue;
			}
			for (uint32_t id : icon.items) {
				icons_.emplace(id, tex);
			}
			icon.image.data.clear();
		}
		UtilityFunctions::print(to_godot("Bf3Hud: loaded in " + std::to_string(l.seconds) + " s: layout " + (layout_ok_ ? "ok" : "fallback") + ", " +
				std::to_string(icons_.size()) + " item ids with an icon, " + std::to_string(icons_failed_) + " icons failed"));
	} else if (!error_.empty()) {
		UtilityFunctions::push_warning(to_godot("Bf3Hud: " + error_));
	}
	redraw_all();
	emit_signal("hud_loaded", ok);
}

const ui::ItemIconIndex *Bf3Hud::item_index() const {
	return finished_ && loaded_ ? loaded_->index.get() : nullptr;
}

Ref<Texture2D> Bf3Hud::icon(uint32_t p_item_id) const {
	auto it = icons_.find(p_item_id);
	return it != icons_.end() ? it->second : Ref<Texture2D>();
}

void Bf3Hud::set_model(const ui::HudModel &p_model) {
	model_ = p_model;
	redraw_all();
}

void Bf3Hud::redraw_all() {
	Bf3HudWidget *widgets[] = { crosshair_, hit_, ammo_, name_, health_ };
	for (Bf3HudWidget *w : widgets) {
		if (w != nullptr) {
			w->queue_redraw();
		}
	}
}

void Bf3Hud::set_vfov_deg(double p_deg) {
	vfov_deg_ = p_deg;
	if (crosshair_ != nullptr) {
		crosshair_->queue_redraw();
	}
}

double Bf3Hud::vfov_deg() const {
	if (vfov_deg_ > 0.0) {
		return vfov_deg_;
	}
	Viewport *vp = is_inside_tree() ? get_viewport() : nullptr;
	Camera3D *cam = vp != nullptr ? vp->get_camera_3d() : nullptr;
	if (cam == nullptr) {
		return kHudDefaultVfovDeg;
	}
	const double fov = cam->get_fov();
	if (cam->get_keep_aspect_mode() == Camera3D::KEEP_HEIGHT) {
		return fov;
	}

	const Vector2 size = get_size();
	if (!(size.x > 0.0f) || !(size.y > 0.0f)) {
		return fov;
	}
	return 2.0 * std::atan(std::tan(fov * kPi / 360.0) * double(size.y) / double(size.x)) * 180.0 / kPi;
}

const std::string &Bf3Hud::localised(uint32_t p_sid_hash) {
	static const std::string kEmpty;
	if (p_sid_hash == 0) {
		return kEmpty;
	}
	auto it = names_.find(p_sid_hash);
	if (it != names_.end()) {
		return it->second;
	}
	Bf3Localisation *loc = Bf3Localisation::current();
	const std::shared_ptr<const ui::TextDatabase> db = loc != nullptr ? loc->database() : nullptr;
	if (db == nullptr) {
		return kEmpty;
	}
	const std::optional<std::string_view> text = db->find(p_sid_hash);
	return names_.emplace(p_sid_hash, text ? std::string(*text) : std::string()).first->second;
}

const std::vector<Bf3Hud::HitPiece> &Bf3Hud::hit_pieces(float p_level) const {
	static const std::vector<HitPiece> kNone;
	const int level = int(std::lround(p_level));
	if (level < 1 || size_t(level) > hit_levels_.size()) {
		return kNone;
	}
	return hit_levels_[size_t(level - 1)];
}

float Bf3Hud::number_ascent_per_px() {
	if (number_ascent_ < 0.0f && number_font_.is_valid()) {
		number_ascent_ = float(number_font_->get_ascent(64)) / 64.0f;
	}
	return std::max(number_ascent_, 0.0f);
}

float Bf3Hud::text_ascent_per_px() {
	if (text_ascent_ < 0.0f && text_font_.is_valid()) {
		text_ascent_ = float(text_font_->get_ascent(64)) / 64.0f;
	}
	return std::max(text_ascent_, 0.0f);
}

const host::HudTextSlot &Bf3Hud::slot_clip() const {
	return layout_ok_ && layout_.ammo_clip.found ? layout_.ammo_clip : fallback_.ammo_clip;
}
const host::HudTextSlot &Bf3Hud::slot_reserve() const {
	return layout_ok_ && layout_.ammo_reserve.found ? layout_.ammo_reserve : fallback_.ammo_reserve;
}
const host::HudTextSlot &Bf3Hud::slot_slash() const {
	return layout_ok_ && layout_.ammo_slash.found ? layout_.ammo_slash : fallback_.ammo_slash;
}
const host::HudTextSlot &Bf3Hud::slot_health() const {
	return layout_ok_ && layout_.health_value.found ? layout_.health_value : fallback_.health_value;
}
const host::HudTextSlot &Bf3Hud::slot_percent() const {
	return layout_ok_ && layout_.health_percent.found ? layout_.health_percent : fallback_.health_percent;
}
ui::HudAnchor Bf3Hud::ammo_anchor() const {
	return layout_ok_ && layout_.ammo.found ? layout_.ammo.anchor : fallback_.ammo.anchor;
}
ui::HudAnchor Bf3Hud::health_anchor() const {
	return layout_ok_ && layout_.health.found ? layout_.health.anchor : fallback_.health.anchor;
}
host::HudPoint Bf3Hud::hit_centre_stage() const {
	const host::HudWidgetSlot &s = layout_ok_ && layout_.hit_indicator.found ? layout_.hit_indicator : fallback_.hit_indicator;
	return { s.x, s.y };
}

void Bf3HudWidget::_notification(int p_what) {
	if (p_what == NOTIFICATION_DRAW) {
		draw_widget();
	}
}

void Bf3HudWidget::begin_record() {
	const uint64_t draws = record_.draws + 1;
	record_ = HudDrawRecord();
	record_.draws = draws;
	record_.serial = hud_ != nullptr ? hud_->model().serial : 0;
}

void Bf3HudAmmo::draw_widget() {
	begin_record();
	if (hud_ == nullptr) {
		return;
	}
	const ui::HudWeapon &w = hud_->model().weapon;
	record_.visible = w.active_weapon_show_ammo;
	if (!record_.visible) {
		return;
	}
	const Vector2 size = get_size();
	const float asc = hud_->number_ascent_per_px();
	const Ref<Font> font = hud_->number_font();
	record_.font = hud_->number_font_name();
	record_.critical = w.active_weapon_critical_ammo;
	record_.text_a = std::to_string(w.active_weapon_ammo);
	record_.text_b = std::to_string(w.active_weapon_ammo_in_bag);
	const ui::HudAnchor anchor = hud_->ammo_anchor();
	const Color clip_colour = w.active_weapon_critical_ammo ? kHudCriticalColor : slot_colour(hud_->slot_clip());
	draw_slot_text(*this, font, hud_->slot_clip(), anchor, asc, record_.text_a, clip_colour, size.x, size.y, record_.size_a, record_.at_a);
	draw_slot_text(*this, font, hud_->slot_reserve(), anchor, asc, record_.text_b, slot_colour(hud_->slot_reserve()), size.x, size.y, record_.size_b,
			record_.at_b);
	const host::HudTextSlot &slash = hud_->slot_slash();
	int32_t slash_size = 0;
	Vector2 slash_at;
	draw_slot_text(*this, font, slash, anchor, asc, slash.initial_text.empty() ? std::string("/") : slash.initial_text, slot_colour(slash), size.x, size.y,
			slash_size, slash_at);
}

void Bf3HudCrosshair::draw_widget() {
	begin_record();
	if (hud_ == nullptr) {
		return;
	}
	const ui::HudWeapon &w = hud_->model().weapon;
	record_.visible = w.show_crosshair;
	if (!record_.visible) {
		return;
	}
	const Vector2 size = get_size();
	const float s = host::hud_scale(size.x, size.y);
	const float cx = 0.5f * size.x;
	const float cy = 0.5f * size.y;
	record_.centre = Vector2(cx, cy);
	const double gap = host::crosshair_gap_px(size.y, w.dispersion, hud_->vfov_deg());
	record_.gap_px = gap;
	const host::HudCrosshairKind kind = host::HudCrosshairKind(std::min<uint32_t>(w.active_weapon_crosshair, 255));
	if (kind == host::HudCrosshairKind::Knife) {
		draw_circle(Vector2(cx, cy), std::max(1.0f, kCrosshairDotRadius * s), kCrosshairColor);
		return;
	}
	const float g = float(gap);
	const float len = std::max(2.0f, std::round(kCrosshairTickLength * s));
	const float t = std::max(1.0f, std::round(kCrosshairTickWidth * s));
	const Color shadow(0.0f, 0.0f, 0.0f, 0.35f);
	const Rect2 ticks[4] = {
		Rect2(cx + g, cy - 0.5f * t, len, t),
		Rect2(cx - g - len, cy - 0.5f * t, len, t),
		Rect2(cx - 0.5f * t, cy - g - len, t, len),
		Rect2(cx - 0.5f * t, cy + g, t, len),
	};
	for (const Rect2 &r : ticks) {
		draw_rect(r.grow(1.0f), shadow, true);
	}
	for (const Rect2 &r : ticks) {
		draw_rect(r, kCrosshairColor, true);
	}
	if (kind == host::HudCrosshairKind::Shotgun && g > 2.0f) {
		draw_arc(Vector2(cx, cy), g, 0.0f, float(2.0 * kPi), 48, kCrosshairColor, std::max(1.0f, std::round(s)), true);
	}
}

void Bf3HudHitIndicator::draw_widget() {
	begin_record();
	if (hud_ == nullptr) {
		return;
	}
	const ui::HudWeapon &w = hud_->model().weapon;
	record_.visible = w.show_hitindicator;
	if (!record_.visible) {
		return;
	}
	const Vector2 size = get_size();
	const float s = host::hud_scale(size.x, size.y);
	const host::HudPoint stage = hud_->hit_centre_stage();
	const host::HudPoint c = host::hud_stage_to_window(ui::HudAnchor::Centre, stage.x, stage.y, size.x, size.y);
	record_.centre = Vector2(c.x, c.y);
	record_.gap_px = s;
	const std::vector<Bf3Hud::HitPiece> &pieces = hud_->hit_pieces(w.hitindicator_level);
	if (!pieces.empty()) {
		const Transform2D base(s, 0.0f, 0.0f, s, c.x, c.y);
		for (const Bf3Hud::HitPiece &p : pieces) {
			draw_set_transform_matrix(base * p.transform);
			draw_texture(p.texture, Vector2());
		}
		draw_set_transform_matrix(Transform2D());
		record_.art = true;
		return;
	}

	const float in = 12.0f * s;
	const float out = 24.0f * s;
	const float width = std::max(1.0f, 2.0f * s);
	for (int i = 0; i < 4; ++i) {
		const float dx = (i & 1) != 0 ? 1.0f : -1.0f;
		const float dy = (i & 2) != 0 ? 1.0f : -1.0f;
		draw_line(Vector2(c.x + dx * in, c.y + dy * in), Vector2(c.x + dx * out, c.y + dy * out), kHudTextColor, width, true);
	}
}

void Bf3HudWeaponName::draw_widget() {
	begin_record();
	if (hud_ == nullptr) {
		return;
	}
	const ui::HudModel &m = hud_->model();
	const ui::HudWeapon &w = m.weapon;
	record_.visible = m.soldier.is_alive && (w.active_weapon_name != 0 || w.active_weapon_data.item_id != 0);
	if (!record_.visible) {
		return;
	}
	const Vector2 size = get_size();
	const float s = host::hud_scale(size.x, size.y);
	const ui::HudAnchor anchor = hud_->ammo_anchor();
	record_.text_a = hud_->localised(w.active_weapon_name);
	record_.font = hud_->text_font_name();
	host::HudTextSlot slot;
	slot.found = true;
	slot.x = kHudNameRight;
	slot.y = kHudNameBaseline;
	slot.size = kHudNameSize;
	slot.align = 1;
	slot.bounds.x_min = -2.0f;
	slot.bounds.x_max = 2.0f;
	slot.bounds.y_min = -2.0f;
	const float asc = hud_->text_ascent_per_px();
	slot.y -= asc * kHudNameSize;
	draw_slot_text(*this, hud_->text_font(), slot, anchor, asc, record_.text_a, kHudTextColor, size.x, size.y, record_.size_a, record_.at_a);
	record_.icon_item = w.active_weapon_data.item_id;
	const Ref<Texture2D> icon = hud_->icon(w.active_weapon_data.item_id);
	const host::HudPoint top_left = host::hud_stage_to_window(anchor, kHudNameRight - kHudIconWidth, kHudIconTop, size.x, size.y);
	record_.icon_at = Vector2(top_left.x, top_left.y);
	record_.icon_size = Vector2(kHudIconWidth * s, kHudIconHeight * s);
	if (icon.is_valid()) {
		draw_texture_rect(icon, Rect2(record_.icon_at, record_.icon_size), false, kHudTextColor);
		record_.art = true;
	}
}

void Bf3HudHealth::draw_widget() {
	begin_record();
	if (hud_ == nullptr) {
		return;
	}
	const ui::HudSoldier &so = hud_->model().soldier;
	record_.visible = so.is_alive;
	if (!record_.visible) {
		return;
	}
	const Vector2 size = get_size();
	const float asc = hud_->number_ascent_per_px();
	const Ref<Font> font = hud_->number_font();
	record_.font = hud_->number_font_name();
	record_.text_a = std::to_string(std::lround(std::isfinite(so.health) ? so.health : 0.0f));
	const ui::HudAnchor anchor = hud_->health_anchor();
	const host::HudTextSlot &value = hud_->slot_health();
	draw_slot_text(*this, font, value, anchor, asc, record_.text_a, slot_colour(value), size.x, size.y, record_.size_a, record_.at_a);
	const host::HudTextSlot &pct = hud_->slot_percent();
	draw_slot_text(*this, font, pct, anchor, asc, pct.initial_text.empty() ? std::string("%") : pct.initial_text, slot_colour(pct), size.x, size.y,
			record_.size_b, record_.at_b);
}

}

void openbf3_register_hud() {
	GDREGISTER_CLASS(bf3::godot_side::Bf3HudWidget);
	GDREGISTER_CLASS(bf3::godot_side::Bf3HudAmmo);
	GDREGISTER_CLASS(bf3::godot_side::Bf3HudCrosshair);
	GDREGISTER_CLASS(bf3::godot_side::Bf3HudHitIndicator);
	GDREGISTER_CLASS(bf3::godot_side::Bf3HudWeaponName);
	GDREGISTER_CLASS(bf3::godot_side::Bf3HudHealth);
	GDREGISTER_CLASS(bf3::godot_side::Bf3Hud);
}
