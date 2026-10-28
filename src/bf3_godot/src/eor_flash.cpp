#include "eor_flash.h"

#include "ui_post_process.h"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>

#include <algorithm>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;

namespace {

CanvasLayer *make_layer(Node *p_parent, const char *p_name, int32_t p_layer, RID &r_item) {
	CanvasLayer *layer = memnew(CanvasLayer);
	layer->set_name(p_name);
	layer->set_layer(p_layer);
	layer->set_visible(false);
	p_parent->add_child(layer);
	r_item = RenderingServer::get_singleton()->canvas_item_create();
	RenderingServer::get_singleton()->canvas_item_set_parent(r_item, layer->get_canvas());
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

void EorFlash::frame(Node *p_parent, const core::gameplay::EorFading &p_fading,
		const core::gameplay::EorTimeline &p_timeline, const ui::HudEndOfRound &p_end_of_round, double p_now_s) {
	if (p_parent == nullptr) {
		return;
	}
	if (white_layer_ == nullptr) {
		build(p_parent);
	}
	cover(p_parent);
	if (!p_end_of_round.visible) {
		over_s_ = -1.0;
	} else if (over_s_ < 0.0) {
		over_s_ = p_now_s;
		++stats_.round_overs;
	}
	float visibility = 0.0f;
	float black = 0.0f;
	if (over_s_ >= 0.0) {
		const float since = float(p_now_s - over_s_);
		if (p_end_of_round.stage == ui::HudEorStage::None) {
			visibility = p_fading.visibility_at(since);
			black = p_fading.screen_black_at(since);
		} else if (p_end_of_round.stage == ui::HudEorStage::Fade && p_timeline.fade_s > 0.0f) {
			black = 1.0f - std::clamp(p_end_of_round.stage_seconds / p_timeline.fade_s, 0.0f, 1.0f);
			++stats_.fade_in_frames;
		}
	}
	stats_.last_visibility = visibility;
	stats_.last_black = black;
	if (visibility > 0.0f) {
		++stats_.white_frames;
		stats_.peak_visibility = std::max(stats_.peak_visibility, visibility);
	}
	if (black >= 1.0f) {
		++stats_.black_frames;
	}
	white_layer_->set_visible(visibility > 0.0f);
	if (visibility > 0.0f) {
		white_material_->set_shader_parameter("visibility", visibility);
		white_material_->set_shader_parameter("brightness", vec3_of(p_fading.brightness));
		white_material_->set_shader_parameter("contrast", vec3_of(p_fading.contrast));
		white_material_->set_shader_parameter("saturation", vec3_of(p_fading.saturation));
	}
	fade_layer_->set_visible(black > 0.0f);
	if (black > 0.0f && black != drawn_black_) {
		RenderingServer *rs = RenderingServer::get_singleton();
		rs->canvas_item_clear(fade_item_);
		rs->canvas_item_add_rect(fade_item_, Rect2(Vector2(), Vector2(covered_w_, covered_h_)), Color(0, 0, 0, black));
		drawn_black_ = black;
	}
}

}
