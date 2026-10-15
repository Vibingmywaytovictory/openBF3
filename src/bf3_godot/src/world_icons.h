#pragma once
// Bf3WorldIcons: flag and HQ icons in the 3D view (docs/design/ui-minimap-icons.md §10.3; gap UI §6). Main thread.
//
// Each frame, for each objective: p = objective position + (0, kIconLift, 0); skipped when
// Camera3D::is_position_behind(p); else Camera3D::unproject_position(p) gives the canvas point, where the hud3dicons
// frame (UIHudIcon_NeutralFlag / _Friendly / _Enemy, UIHudIcon_Base) is drawn with draw_texture_rect_region, scaled
// with distance like UICapturepointtagCompData (IconSize 30, ScaleDistance 500, MaxScaleMod 0.4, FadeDistance 200 -
// FadeEndDistance 800, MaxFarFade 0.5: the component's own fields, read from ui/uicomponents/uicapturepointtagcomp
// [semantics inferred]), with the label letter and the distance in metres under it. Off-screen icons are not clamped
// to the edge in this wave (SnapIcons: M2b).

#include "minimap_control.h"

#include <godot_cpp/classes/control.hpp>

#include <vector>

namespace godot {
class Camera3D;
}

namespace bf3::godot_side {

struct WorldIconStyle {
	float icon_size = 30.0f; // IconSize
	float scale_distance = 500.0f; // ScaleDistance
	float max_scale_mod = 0.4f; // MaxScaleMod
	float fade_distance = 200.0f; // FadeDistance
	float fade_end_distance = 800.0f; // FadeEndDistance
	float max_far_fade = 0.5f; // MaxFarFade
	float lift = 3.0f; // metres above the capture point's origin [inferred]
};

class Bf3WorldIcons : public godot::Control {
	GDCLASS(Bf3WorldIcons, godot::Control)

public:
	void _draw() override;

	void setup(godot::Ref<godot::ImageTexture> p_atlas, const bf3::core::ui::HudIconAtlas *p_icons, const WorldIconStyle &p_style);
	// Every frame: the camera that renders (first person or free) and the icons.
	void update(const godot::Camera3D *p_camera, std::vector<MinimapIconDraw> p_icons);

	// For the check: canvas points of the icons drawn last frame (NaN for skipped ones).
	const std::vector<bf3::core::ui::ScreenPoint> &last_points() const { return last_points_; }

protected:
	static void _bind_methods();

private:
	godot::Ref<godot::ImageTexture> atlas_;
	const bf3::core::ui::HudIconAtlas *icon_atlas_ = nullptr;
	WorldIconStyle style_;
	const godot::Camera3D *camera_ = nullptr;
	std::vector<MinimapIconDraw> icons_;
	std::vector<bf3::core::ui::ScreenPoint> last_points_;
};

} // namespace bf3::godot_side
