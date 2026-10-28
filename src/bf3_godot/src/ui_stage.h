#pragma once

#include "bf3/core/ui/ui_stage.h"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>

namespace bf3::godot_side {

godot::Rect2 ui_window_rect(const godot::Node *p_node);
godot::Vector2i ui_window_pixels(const godot::Node *p_node);
bf3::core::ui::UiStage ui_stage(const godot::Node *p_node);

}
