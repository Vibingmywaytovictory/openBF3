// UI ui2: Bf3MinimapControl (minimap_control.h; docs/design/ui-minimap-icons.md §10.2).
#include "minimap_control.h"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace godot;
namespace ui = bf3::core::ui;

namespace bf3::godot_side {

namespace {

// Icons are drawn at this many canvas units per atlas texel per 100 units of minimap side [inferred: tuned by eye on
// the G11 shots; BF3's own minimap icon scale is a UIMinimapCompData source, a Client row].
constexpr float kIconUnitsPerTexelPer100 = 0.45f;
constexpr float kDiscMargin = 1.5f; // canvas units between the square's edge and the disc
constexpr float kRimInsetPer100 = 7.0f; // objectives clamped this far inside the disc edge, per 100 units of side (about half an icon)

ui::ScreenPoint nan_point() {
	const float n = std::numeric_limits<float>::quiet_NaN();
	return { n, n };
}

} // namespace

Bf3MinimapControl::Bf3MinimapControl() {
	last_marker_ = nan_point();
	set_mouse_filter(MOUSE_FILTER_IGNORE);
}

Bf3MinimapControl::~Bf3MinimapControl() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs != nullptr) {
		if (icon_item_.is_valid()) {
			rs->free_rid(icon_item_);
		}
		if (map_item_.is_valid()) {
			rs->free_rid(map_item_);
		}
	}
}

void Bf3MinimapControl::_bind_methods() {}

void Bf3MinimapControl::_notification(int p_what) {
	if (p_what == NOTIFICATION_PREDELETE) {
		RenderingServer *rs = RenderingServer::get_singleton();
		if (icon_item_.is_valid()) {
			rs->free_rid(icon_item_);
			icon_item_ = RID();
		}
		if (map_item_.is_valid()) {
			rs->free_rid(map_item_);
			map_item_ = RID();
		}
	}
}

void Bf3MinimapControl::setup(const ui::MinimapData &p_data, Ref<ImageTexture> p_satellite, Ref<ShaderMaterial> p_material, Ref<ImageTexture> p_atlas,
		const ui::HudIconAtlas *p_icons) {
	data_ = p_data;
	satellite_ = p_satellite;
	material_ = p_material;
	atlas_ = p_atlas;
	icon_atlas_ = p_icons;
	ensure_items();
	queue_redraw();
}

void Bf3MinimapControl::ensure_items() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!map_item_.is_valid()) {
		map_item_ = rs->canvas_item_create();
		rs->canvas_item_set_parent(map_item_, get_canvas_item());
		rs->canvas_item_set_draw_index(map_item_, 0);
	}
	if (!icon_item_.is_valid()) {
		icon_item_ = rs->canvas_item_create();
		rs->canvas_item_set_parent(icon_item_, get_canvas_item());
		rs->canvas_item_set_draw_index(icon_item_, 1);
	}
	if (material_.is_valid()) {
		rs->canvas_item_set_material(map_item_, material_->get_rid());
	}
}

void Bf3MinimapControl::set_view(const ui::MinimapView &p_view) {
	view_ = p_view;
	queue_redraw();
}

void Bf3MinimapControl::set_icons(std::vector<MinimapIconDraw> p_icons) {
	icons_ = std::move(p_icons);
	queue_redraw();
}

float Bf3MinimapControl::icon_scale() const {
	return kIconUnitsPerTexelPer100 * std::min(get_size().x, get_size().y) / 100.0f;
}

float Bf3MinimapControl::disc_radius() const {
	const Vector2 s = get_size();
	return std::max(1.0f, 0.5f * std::min(s.x, s.y) - kDiscMargin);
}

float Bf3MinimapControl::rim_radius() const {
	return std::max(1.0f, disc_radius() - kRimInsetPer100 * std::min(get_size().x, get_size().y) / 100.0f);
}

void Bf3MinimapControl::set_debug_uv(bool p_on) {
	debug_uv_ = p_on;
	if (material_.is_valid()) {
		material_->set_shader_parameter("debug_uv", p_on);
	}
	queue_redraw();
}

void Bf3MinimapControl::set_icons_visible(bool p_on) {
	icons_visible_ = p_on;
	queue_redraw();
}

void Bf3MinimapControl::set_marker_only(bool p_on) {
	marker_only_ = p_on;
	queue_redraw();
}

Transform2D Bf3MinimapControl::canvas_to_viewport() const {
	return get_viewport()->get_final_transform() * get_global_transform_with_canvas();
}

void Bf3MinimapControl::draw_icon(const MinimapIconDraw &p_icon, Vector2 p_at, float p_angle, Rect2 *r_rect) {
	if (p_icon.icon == nullptr || !atlas_.is_valid()) {
		return;
	}
	const ui::HudIconStateFrames *st = p_icon.icon->state(p_icon.state);
	if (st == nullptr || st->frames.empty()) {
		return;
	}
	const ui::HudIconRect r = ui::hud_icon_rect(st->frames.front(), uint32_t(atlas_->get_width()), uint32_t(atlas_->get_height()));
	if (r.width <= 0 || r.height <= 0) {
		return;
	}
	const float k = icon_scale();
	const float w = float(r.width) * k;
	const float h = float(r.height) * k;
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->canvas_item_add_set_transform(icon_item_, Transform2D(p_angle, p_at));
	// A negative size draws the frame mirrored (HudIconRect::flip_x / flip_y).
	rs->canvas_item_add_texture_rect_region(icon_item_, Rect2(r.flip_x ? w * 0.5f : -w * 0.5f, r.flip_y ? h * 0.5f : -h * 0.5f, r.flip_x ? -w : w, r.flip_y ? -h : h),
			atlas_->get_rid(), Rect2(float(r.x), float(r.y), float(r.width), float(r.height)));
	if (r_rect != nullptr) {
		*r_rect = Rect2(p_at.x - w * 0.5f, p_at.y - h * 0.5f, w, h);
	}
}

void Bf3MinimapControl::_draw() {
	ensure_items();
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->canvas_item_clear(map_item_);
	rs->canvas_item_clear(icon_item_);
	last_marker_ = nan_point();
	last_icons_.assign(icons_.size(), nan_point());
	last_icon_rects_.assign(icons_.size(), Rect2());
	last_clamped_.assign(icons_.size(), false);
	if (!satellite_.is_valid() || !(data_.world_size > 0.0f)) {
		return;
	}
	const Vector2 size = get_size();
	const Vector2 centre = size * 0.5f;
	const float radius = disc_radius();

	// The control's own item: a dark disc behind the map (shows where the satellite does not reach).
	draw_circle(centre, radius, Color(0.02f, 0.03f, 0.04f, 0.85f));

	// 1. The map item.
	if (material_.is_valid()) {
		const Transform2D to_vp = canvas_to_viewport();
		const Vector2 c_px = to_vp.xform(centre);
		const float scale = to_vp.get_scale().x;
		material_->set_shader_parameter("clip_center_px", c_px);
		material_->set_shader_parameter("clip_radius_px", radius * std::fabs(scale));
	}
	const float ppm = view_.pixels_per_metre;
	const float s = data_.world_size;
	const float x0 = -(0.5f * s + data_.world_center[0] - view_.player_x) * ppm;
	const float y0 = -(0.5f * s + data_.world_center[1] - view_.player_z) * ppm;
	rs->canvas_item_add_set_transform(map_item_, Transform2D(view_.rotate ? view_.heading : 0.0f, Vector2(view_.center_x, view_.center_y)));
	rs->canvas_item_add_texture_rect_region(map_item_, Rect2(x0, y0, s * ppm, s * ppm), satellite_->get_rid(),
			Rect2(0.0f, 0.0f, float(satellite_->get_width()), float(satellite_->get_height())));

	if (!icons_visible_) {
		return;
	}
	// 2. The ring on top of the map's edge.
	rs->canvas_item_add_set_transform(icon_item_, Transform2D());
	{
		constexpr int kSegments = 96;
		PackedVector2Array ring;
		ring.resize(kSegments + 1);
		for (int i = 0; i <= kSegments; ++i) {
			const float a = float(i) * 6.2831853f / float(kSegments);
			ring.set(i, centre + Vector2(std::cos(a), std::sin(a)) * radius);
		}
		PackedColorArray colours;
		colours.push_back(Color(0.75f, 0.85f, 0.9f, 0.8f));
		rs->canvas_item_add_polyline(icon_item_, ring, colours, 1.5f, true);
	}
	// 3. Objectives and others.
	if (!marker_only_) {
		const float rim = rim_radius();
		for (size_t i = 0; i < icons_.size(); ++i) {
			const MinimapIconDraw &ic = icons_[i];
			const ui::ScreenPoint p = ui::minimap_project(view_, ic.world_x, ic.world_z);
			if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
				continue;
			}
			Vector2 at(p.x, p.y);
			const Vector2 d = at - centre;
			const float len = d.length();
			bool clamped = false;
			if (len > rim) {
				if (!ic.objective) {
					continue; // others off the disc are not shown
				}
				at = centre + d * (rim / len);
				clamped = true;
			}
			float angle = 0.0f;
			const ui::HudIconStateFrames *st = ic.icon != nullptr ? ic.icon->state(ic.state) : nullptr;
			if (st != nullptr && st->should_rotate) {
				// The object's heading on the map, as minimap_marker_angle turns the player's arrow.
				angle = (view_.rotate ? view_.heading : 0.0f) - ic.yaw;
			}
			Rect2 rect;
			draw_icon(ic, at, angle, &rect);
			last_icons_[i] = { at.x, at.y };
			last_icon_rects_[i] = rect;
			last_clamped_[i] = clamped;
		}
	}
	// 4. The player marker.
	if (icon_atlas_ != nullptr) {
		const ui::HudIconChoice me = ui::soldier_icon(true, 0, 0);
		MinimapIconDraw marker;
		marker.icon = icon_atlas_->find(me.icon);
		marker.state = me.state;
		draw_icon(marker, Vector2(view_.center_x, view_.center_y), ui::minimap_marker_angle(view_), nullptr);
		last_marker_ = { view_.center_x, view_.center_y };
	}
}

} // namespace bf3::godot_side
