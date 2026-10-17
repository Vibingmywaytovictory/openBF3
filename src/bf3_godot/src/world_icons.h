#pragma once
// Bf3WorldIcons: flag and HQ icons in the 3D view (docs/design/ui-minimap-icons.md §10.3; gap UI §6). Main thread.
//
// Each frame, for each objective: p = objective position + (0, lift, 0); skipped when
// Camera3D::is_position_behind(p); else the icon centre is Camera3D::unproject_position(p) + (0, pixel_offset_y) in
// canvas units, where the hud3dicons frame (objective_icon in hud_icons.h: UIHudIcon_NeutralFlag / FriendlyFlag /
// EnemyFlag, UIHudIcon_Base) is drawn with draw_texture_rect_region, scaled
// with distance like UICapturepointtagCompData (IconSize 30, ScaleDistance 500, MaxScaleMod 0.4, FadeDistance 200 -
// FadeEndDistance 800, MaxFarFade 0.5, VerticalOffset 5.5, PixelOffset (0, -28): the component's own fields, read from
// ui/uicomponents/uicapturepointtagcomp [values verified, semantics inferred]), with the label letter and the distance
// in metres under it. Off-screen icons are not clamped
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
	float lift = 5.5f; // VerticalOffset: metres above the capture point's origin [inferred]
	float pixel_offset_y = -28.0f; // PixelOffset.y, canvas pixels
};

class Bf3WorldIcons : public godot::Control {
	GDCLASS(Bf3WorldIcons, godot::Control)

public:
	void _draw() override;

	void setup(godot::Ref<godot::ImageTexture> p_atlas, const bf3::core::ui::HudIconAtlas *p_icons, const WorldIconStyle &p_style);
	// Every frame: the camera that renders (first person or free) and the icons.
	void update(const godot::Camera3D *p_camera, std::vector<MinimapIconDraw> p_icons);

	// For the check: icon centres drawn last frame (unproject_position(lifted) + pixel offset; NaN for skipped ones).
	// Gate G10 compares them with the check's own pinhole projection of the same lifted point (the camera's
	// get_camera_projection() and global transform, multiplied out in the check) plus the same offset, so a wrong lift,
	// offset or camera shows; and a pixel test that the icon is in the viewport image there (minimap_check.h).
	const std::vector<bf3::core::ui::ScreenPoint> &last_points() const { return last_points_; }
	// Parallel to last_points: the drawn icon rect in canvas units (empty for skipped ones).
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

} // namespace bf3::godot_side
