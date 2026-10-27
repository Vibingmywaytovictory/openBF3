#pragma once

#include "bf3/core/ui/swf_scene.h"

#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace bf3::core::ui {
class HudScreen;
}

namespace bf3::godot_side {

class Bf3SwfView;
struct SwfEngineTarget;

class HudBgPainter {
public:
	bool paint(Bf3SwfView &r_view, const SwfEngineTarget &p_target, std::string_view p_owner = {});
	void paint_all(Bf3SwfView &r_view, std::string_view p_owner);
	void reset() { painted_.clear(); }
	void forget(std::string_view p_owner);

private:
	godot::Ref<godot::ShaderMaterial> material_;
	std::map<std::string, std::pair<bf3::core::ui::SwfRect, godot::Vector2i>, std::less<>> painted_;
};

void apply_hud_view_effects(Bf3SwfView &r_view, const bf3::core::ui::HudScreen &p_screen);

}
