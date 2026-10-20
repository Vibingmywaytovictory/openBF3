#include "world_icons.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/theme_db.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace godot;
namespace ui = bf3::core::ui;

namespace bf3::godot_side {

void Bf3WorldIcons::_bind_methods() {}

void Bf3WorldIcons::setup(Ref<ImageTexture> p_atlas, const ui::HudIconAtlas *p_icons, const WorldIconStyle &p_style) {
	atlas_ = p_atlas;
	icon_atlas_ = p_icons;
	style_ = p_style;
	set_mouse_filter(MOUSE_FILTER_IGNORE);
}

void Bf3WorldIcons::update(const Camera3D *p_camera, std::vector<MinimapIconDraw> p_icons) {
	camera_ = p_camera;
	icons_ = std::move(p_icons);
	queue_redraw();
}

void Bf3WorldIcons::_draw() {
	const float nan = std::numeric_limits<float>::quiet_NaN();
	last_points_.assign(icons_.size(), ui::ScreenPoint{ nan, nan });
	last_rects_.assign(icons_.size(), Rect2());
	if (camera_ == nullptr || !atlas_.is_valid() || !camera_->is_inside_tree()) {
		return;
	}
	const Vector3 eye = camera_->get_global_transform().origin;
	Ref<Font> font = ThemeDB::get_singleton()->get_fallback_font();
	for (size_t i = 0; i < icons_.size(); ++i) {
		const MinimapIconDraw &ic = icons_[i];
		if (ic.icon == nullptr) {
			continue;
		}
		const ui::HudIconStateFrames *st = ic.icon->state(ic.state);
		if (st == nullptr || st->frames.empty()) {
			continue;
		}
		const Vector3 p(ic.world_x, ic.world_y + style_.lift, ic.world_z);
		if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || camera_->is_position_behind(p)) {
			continue;
		}
		const Vector2 c = camera_->unproject_position(p) + Vector2(0.0f, style_.pixel_offset_y);
		const float d = eye.distance_to(p);

		const float ts = std::clamp(d / std::max(1.0f, style_.scale_distance), 0.0f, 1.0f);
		const float scale = 1.0f - style_.max_scale_mod * ts;
		const float span = std::max(1.0f, style_.fade_end_distance - style_.fade_distance);
		const float tf = std::clamp((d - style_.fade_distance) / span, 0.0f, 1.0f);
		const float alpha = 1.0f + (style_.max_far_fade - 1.0f) * tf;
		const ui::HudIconRect r = ui::hud_icon_rect(st->frames.front(), uint32_t(atlas_->get_width()), uint32_t(atlas_->get_height()));
		if (r.width <= 0 || r.height <= 0) {
			continue;
		}

		const float k = style_.icon_size * scale / float(std::max(r.width, r.height));
		const float w = float(r.width) * k;
		const float h = float(r.height) * k;
		draw_texture_rect_region(atlas_, Rect2(r.flip_x ? c.x + w * 0.5f : c.x - w * 0.5f, r.flip_y ? c.y + h * 0.5f : c.y - h * 0.5f, r.flip_x ? -w : w, r.flip_y ? -h : h),
				Rect2(float(r.x), float(r.y), float(r.width), float(r.height)), Color(1, 1, 1, alpha));
		last_points_[i] = { c.x, c.y };
		last_rects_[i] = Rect2(c.x - w * 0.5f, c.y - h * 0.5f, w, h);
		if (font.is_valid()) {
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%dm", int(std::lround(d)));
			String text = ic.label.is_empty() ? String(buf) : ic.label + String(" ") + String(buf);
			const int font_size = 11;
			const Vector2 ts2 = font->get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size);
			const Vector2 at(c.x - ts2.x * 0.5f, c.y + h * 0.5f + float(font_size) + 1.0f);
			draw_string(font, at + Vector2(1, 1), text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(0, 0, 0, 0.8f * alpha));
			draw_string(font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(1, 1, 1, alpha));
		}
	}
}

}
