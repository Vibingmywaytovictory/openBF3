#include "ui_stage.h"

#include <godot_cpp/classes/viewport.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace bf3::godot_side {

using namespace godot;

namespace ui = bf3::core::ui;

namespace {

const Viewport *viewport_of(const Node *p_node) {
	return p_node != nullptr && p_node->is_inside_tree() ? p_node->get_viewport() : nullptr;
}

}

Rect2 ui_window_rect(const Node *p_node) {
	const Viewport *vp = viewport_of(p_node);
	return vp != nullptr ? vp->get_visible_rect()
						 : Rect2(0.0f, 0.0f, ui::kUiScreenStage.width(), ui::kUiScreenStage.height());
}

Vector2i ui_window_pixels(const Node *p_node) {
	const Viewport *vp = viewport_of(p_node);
	const Rect2 r = ui_window_rect(p_node);
	const Vector2 k = vp != nullptr ? vp->get_final_transform().get_scale() : Vector2(1.0f, 1.0f);
	return Vector2i(std::max(1, int32_t(std::lround(r.size.x * std::fabs(k.x)))),
			std::max(1, int32_t(std::lround(r.size.y * std::fabs(k.y)))));
}

ui::UiStage ui_stage(const Node *p_node) {
	const Vector2 size = ui_window_rect(p_node).size;
	return ui::ui_stage_show_all(size.x, size.y);
}

}
