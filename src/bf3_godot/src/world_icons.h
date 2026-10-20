#pragma once

#include "minimap_control.h"

#include <godot_cpp/classes/control.hpp>

#include <vector>

namespace godot {
class Camera3D;
}

namespace bf3::godot_side {

struct WorldIconStyle {
	float icon_size = 30.0f;
	float scale_distance = 500.0f;
	float max_scale_mod = 0.4f;
	float fade_distance = 200.0f;
	float fade_end_distance = 800.0f;
	float max_far_fade = 0.5f;
	float lift = 5.5f;
	float pixel_offset_y = -28.0f;
};

class Bf3WorldIcons : public godot::Control {
	GDCLASS(Bf3WorldIcons, godot::Control)

public:
	void _draw() override;

	void setup(godot::Ref<godot::ImageTexture> p_atlas, const bf3::core::ui::HudIconAtlas *p_icons, const WorldIconStyle &p_style);

	void update(const godot::Camera3D *p_camera, std::vector<MinimapIconDraw> p_icons);

	const std::vector<bf3::core::ui::ScreenPoint> &last_points() const { return last_points_; }

	const std::vector<godot::Rect2> &last_rects() const { return last_rects_; }
	const WorldIconStyle &style() const { return style_; }

protected:
	static void _bind_methods();

private:
	godot::Ref<godot::ImageTexture> atlas_;
	const bf3::core::ui::HudIconAtlas *icon_atlas_ = nullptr;
	WorldIconStyle style_;
	const godot::Camera3D *camera_ = nullptr;
	std::vector<MinimapIconDraw> icons_;
	std::vector<bf3::core::ui::ScreenPoint> last_points_;
	std::vector<godot::Rect2> last_rects_;
};

}
