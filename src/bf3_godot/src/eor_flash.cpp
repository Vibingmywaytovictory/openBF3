#include "eor_flash.h"

#include "ui_post_process.h"

#include <godot_cpp/classes/canvas_item.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>

#include <algorithm>

namespace bf3::godot_side {

using namespace godot;

namespace {

CanvasLayer *make_layer(Node *p_parent, const char *p_name, int32_t p_layer, RID &r_item) {
	CanvasLayer *layer = memnew(CanvasLayer);
	layer->set_name(p_name);
	layer->set_layer(p_layer);
	layer->set_visible(false);
	p_parent->add_child(layer);
	r_item = RenderingServer::get_singleton()->canvas_item_create();
	RenderingServer::get_singleton()->canvas_item_set_parent(r_item, layer->get_canvas());
	RenderingServer::get_singleton()->canvas_item_set_visible(r_item, false);
	return layer;
}

}

EorFlash::~EorFlash() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		return;
	}
	if (white_item_.is_valid()) {
		rs->free_rid(white_item_);
	}
	if (fade_item_.is_valid()) {
		rs->free_rid(fade_item_);
	}
}

void EorFlash::build(Node *p_parent) {
	white_material_ = make_grade_material();
	white_layer_ = make_layer(p_parent, "EorWhiteScreen", kEorWhiteLayer, white_item_);
	RenderingServer::get_singleton()->canvas_item_set_material(white_item_, white_material_->get_rid());
	fade_layer_ = make_layer(p_parent, "EorScreenFade", kEorFadeLayer, fade_item_);
}

void EorFlash::cover(Node *p_parent) {
	const Vector2 size = p_parent->get_viewport()->get_visible_rect().size;
	if (size.x == covered_w_ && size.y == covered_h_) {
		return;
	}
	covered_w_ = size.x;
	covered_h_ = size.y;
	drawn_black_ = -1.0f;
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->canvas_item_clear(white_item_);
	rs->canvas_item_add_rect(white_item_, Rect2(Vector2(), size), Color(1, 1, 1, 1));
}

void EorFlash::fade_ui(CanvasLayer *p_ui, float p_alpha) {
	if (p_ui == nullptr) {
		return;
	}
	for (int32_t i = 0; i < p_ui->get_child_count(); ++i) {
		CanvasItem *item = Object::cast_to<CanvasItem>(p_ui->get_child(i));
		if (item != nullptr && item->get_modulate().a != p_alpha) {
			Color c = item->get_modulate();
			c.a = p_alpha;
			item->set_modulate(c);
		}
	}
}

void EorFlash::frame(Node *p_parent, CanvasLayer *p_ui, const core::gameplay::EorFading &p_fading,
		const host::LogicScreenEffectsFrame &p_effects) {
	if (p_parent == nullptr) {
		return;
	}
	if (white_layer_ == nullptr) {
		build(p_parent);
	}
	cover(p_parent);
	const host::LogicEnvironmentState *white = p_effects.environment(p_fading.visual_environment);
	if (white != nullptr && !white_enabled_) {
		++stats_.round_overs;
	}
	white_enabled_ = white != nullptr;
	const float visibility = white != nullptr ? std::clamp(white->visibility, 0.0f, 1.0f) : 0.0f;
	const float black = std::clamp(p_effects.screen_faded, 0.0f, 1.0f);
	const float ui_faded = std::clamp(p_effects.ui_faded, 0.0f, 1.0f);
	if (black > 0.0f && black < 1.0f && black < stats_.last_black) {
		++stats_.fade_in_frames;
	}
	stats_.last_visibility = visibility;
	stats_.last_black = black;
	stats_.last_ui_faded = ui_faded;
	if (visibility > 0.0f) {
		++stats_.white_frames;
		stats_.peak_visibility = std::max(stats_.peak_visibility, visibility);
	}
	if (black >= 1.0f) {
		++stats_.black_frames;
	}
	if (ui_faded > 0.0f) {
		++stats_.ui_faded_frames;
		stats_.peak_ui_faded = std::max(stats_.peak_ui_faded, ui_faded);
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	white_layer_->set_visible(visibility > 0.0f);
	rs->canvas_item_set_visible(white_item_, visibility > 0.0f);
	white_material_->set_shader_parameter("visibility", visibility);
	if (visibility > 0.0f) {
		white_material_->set_shader_parameter("brightness", vec3_of(p_fading.brightness));
		white_material_->set_shader_parameter("contrast", vec3_of(p_fading.contrast));
		white_material_->set_shader_parameter("saturation", vec3_of(p_fading.saturation));
	}
	fade_layer_->set_visible(black > 0.0f);
	rs->canvas_item_set_visible(fade_item_, black > 0.0f);
	if (black > 0.0f && black != drawn_black_) {
		rs->canvas_item_clear(fade_item_);
		rs->canvas_item_add_rect(fade_item_, Rect2(Vector2(), Vector2(covered_w_, covered_h_)), Color(0, 0, 0, black));
		drawn_black_ = black;
	}
	fade_ui(p_ui, 1.0f - ui_faded);
}
}
