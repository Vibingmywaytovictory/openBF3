#include "ui_cursor.h"

#include "ui_post_process.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>

#include <algorithm>

namespace bf3::godot_side {

using namespace godot;

namespace {

Bf3UiCursor *g_current = nullptr;

Color rgb8(const uint8_t (&p_rgb)[3]) {
	return Color(float(p_rgb[0]) / 255.0f, float(p_rgb[1]) / 255.0f, float(p_rgb[2]) / 255.0f, 1.0f);
}

}

Bf3UiCursor::Bf3UiCursor() {
	set_layer(kUiCursorLayer);
}

Bf3UiCursor::~Bf3UiCursor() {
	if (g_current == this) {
		g_current = nullptr;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs != nullptr && item_.is_valid()) {
		rs->free_rid(item_);
		item_ = RID();
	}
}

Bf3UiCursor *Bf3UiCursor::current() {
	return g_current;
}

Bf3UiCursor *Bf3UiCursor::ensure(Node *p_parent) {
	if (g_current != nullptr) {
		return g_current;
	}
	if (p_parent == nullptr) {
		return nullptr;
	}
	Bf3UiCursor *c = memnew(Bf3UiCursor);
	c->set_name("UiCursor");
	p_parent->add_child(c);
	return c;
}

void Bf3UiCursor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("show_cursor"), &Bf3UiCursor::show_cursor);
	ClassDB::bind_method(D_METHOD("hide_cursor"), &Bf3UiCursor::hide_cursor);
	ClassDB::bind_method(D_METHOD("set_cursor_position", "position"), &Bf3UiCursor::set_cursor_position);
	ClassDB::bind_method(D_METHOD("set_follow_mouse", "follow"), &Bf3UiCursor::set_follow_mouse);
}

void Bf3UiCursor::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE && g_current == nullptr) {
		g_current = this;
	} else if (p_what == NOTIFICATION_EXIT_TREE && g_current == this) {
		g_current = nullptr;
	}
}

void Bf3UiCursor::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	item_ = rs->canvas_item_create();
	rs->canvas_item_set_parent(item_, get_canvas());
	rs->canvas_item_set_visible(item_, shown_);
	set_process(true);
	redraw();
}

float Bf3UiCursor::cursor_scale() const {
	const Viewport *vp = get_viewport();
	return vp != nullptr ? std::max(1e-3f, vp->get_visible_rect().size.y / kUiStageHeight) : 1.0f;
}

void Bf3UiCursor::redraw() {
	if (!item_.is_valid()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	const float s = cursor_scale();
	drawn_scale_ = s;
	rs->canvas_item_clear(item_);
	PackedVector2Array fill;
	for (const Vector2 &p : kUiCursorArrow) {
		fill.push_back(p * s);
	}
	PackedColorArray fill_colours;
	fill_colours.push_back(rgb8(kUiCursorFillRgb));
	rs->canvas_item_add_polygon(item_, fill, fill_colours);
	PackedVector2Array outline = fill;
	outline.push_back(fill[0]);
	PackedColorArray outline_colours;
	outline_colours.push_back(rgb8(kUiCursorOutlineRgb));
	rs->canvas_item_add_polyline(item_, outline, outline_colours, kUiCursorOutlinePx * s, true);
	stats_.draws++;
}

void Bf3UiCursor::show_cursor() {
	if (!shown_) {
		stats_.shows++;
	}
	shown_ = true;
	if (item_.is_valid()) {
		RenderingServer::get_singleton()->canvas_item_set_visible(item_, true);
	}
}

void Bf3UiCursor::hide_cursor() {
	if (shown_) {
		stats_.hides++;
	}
	shown_ = false;
	if (item_.is_valid()) {
		RenderingServer::get_singleton()->canvas_item_set_visible(item_, false);
	}
}

void Bf3UiCursor::set_cursor_position(Vector2 p_canvas) {
	position_ = p_canvas;
	if (item_.is_valid()) {
		RenderingServer::get_singleton()->canvas_item_set_transform(item_, Transform2D(0.0f, position_));
	}
}

void Bf3UiCursor::_process(double) {
	if (follow_mouse_) {
		if (Viewport *vp = get_viewport()) {
			set_cursor_position(vp->get_mouse_position());
		}
	}
	if (cursor_scale() != drawn_scale_) {
		redraw();
	}
}

}
