#pragma once
// Bf3MinimapControl: the minimap drawn with CanvasItem calls and one canvas_item shader (docs/design/
// ui-minimap-icons.md §10.2; gap UI §6 "Minimap rendering"). Main thread only.
//
// _draw():
//   1. draw_set_transform(center, rotation = +heading when rotating (clockwise, y-down canvas), scale = pixels per
//      metre x WorldSize / image side), then draw_texture_rect_region of the satellite placed so that texel (u, v)
//      lands where minimap_project puts its world point (minimap.h); the ShaderMaterial (kMinimapShader below) tints
//      the L8 satellite with DetailTextureTint and shades everything outside the mode's combat area with
//      CombatAreaColor: alpha = smoothstep(thr - w, thr + w, sdf), thr = CombatAreaAlphaThreshold (0.496), w = the
//      SDF's texel-to-metre width (1.5 texels of the 128² field, design §5.5). Two or more masks (CQS: one per team)
//      are combined into one R8 union texture on the loader thread (minimum of the distance values).
//   2. the circular frame: the control clips to a circle (shader discard past radius) like BF3's minimapmask.
//   3. objectives: flags (UIHudIcon_NeutralFlag / FriendlyFlag / EnemyFlag by initial owner) and HQs (UIHudIcon_Base
//      Friendly / Enemy) from the hud3dicons atlas with draw_texture_rect_region (the state's frame rect,
//      hud_icon_rect), at minimap_project(objective), clamped to the rim when outside; label letters through tr().
//   4. the player marker: UIHudIcon_LocalPlayer (Default state, ShouldRotate) at the centre, rotated by
//      minimap_marker_angle(view).
//   5. the others (M1's walkers) as UIHudIcon_Player Neutral dots (the M1 game has no teams yet).
// Everything is redrawn on queue_redraw from Bf3MinimapHud::_process; no Godot call happens off the main thread.

#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/minimap.h"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/shader_material.hpp>

#include <string_view>
#include <vector>

namespace bf3::godot_side {

// The canvas_item shader (design §10.2). Uniforms: satellite (L8), combat_area (R8 union SDF), detail (L8 SDF, unused
// on XP2), tint, area_color, threshold, edge_width, detail_tint, detail_threshold, radius_px, size_px.
inline constexpr std::string_view kMinimapShader = R"(shader_type canvas_item;
uniform sampler2D satellite : filter_linear_mipmap, repeat_disable;
uniform sampler2D combat_area : filter_linear, repeat_disable;
uniform sampler2D detail : filter_linear, repeat_disable;
uniform vec4 tint = vec4(0.3, 0.6, 0.8, 1.0);
uniform vec4 area_color = vec4(0.991, 0.706, 0.513, 0.45);
uniform float threshold = 0.496;
uniform float edge_width = 0.01;
uniform vec4 detail_tint = vec4(0.9, 1.0, 1.0, 0.3);
uniform float detail_threshold = 0.5;
uniform bool has_detail = false;
void fragment() {
	float l = texture(satellite, UV).r;
	vec3 rgb = l * tint.rgb;
	if (has_detail) {
		float d = texture(detail, UV).r;
		rgb = mix(rgb, detail_tint.rgb, detail_tint.a * (1.0 - smoothstep(detail_threshold - edge_width, detail_threshold + edge_width, d)));
	}
	float sdf = texture(combat_area, UV).r;
	float outside = smoothstep(threshold - edge_width, threshold + edge_width, sdf);
	rgb = mix(rgb, area_color.rgb, outside * area_color.a);
	COLOR = vec4(rgb, 1.0);
}
)";

struct MinimapIconDraw {
	const bf3::core::ui::HudIcon *icon = nullptr;
	bf3::core::ui::HudIconState state = bf3::core::ui::HudIconState::Default;
	float world_x = 0.0f;
	float world_z = 0.0f;
	float yaw = 0.0f;
	godot::String label;
};

class Bf3MinimapControl : public godot::Control {
	GDCLASS(Bf3MinimapControl, godot::Control)

public:
	void _draw() override;

	// Set once by Bf3MinimapHud::build_godot_side.
	void setup(const bf3::core::ui::MinimapData &p_data, godot::Ref<godot::ImageTexture> p_satellite, godot::Ref<godot::ShaderMaterial> p_material,
			godot::Ref<godot::ImageTexture> p_atlas, const bf3::core::ui::HudIconAtlas *p_icons);
	// Every frame.
	void set_view(const bf3::core::ui::MinimapView &p_view);
	void set_icons(std::vector<MinimapIconDraw> p_icons);

	// For the check: where the last _draw put the player marker and each icon (canvas pixels, control-local).
	bf3::core::ui::ScreenPoint last_marker() const { return last_marker_; }
	const std::vector<bf3::core::ui::ScreenPoint> &last_icons() const { return last_icons_; }

protected:
	static void _bind_methods();

private:
	bf3::core::ui::MinimapData data_;
	bf3::core::ui::MinimapView view_;
	godot::Ref<godot::ImageTexture> satellite_;
	godot::Ref<godot::ShaderMaterial> material_;
	godot::Ref<godot::ImageTexture> atlas_;
	const bf3::core::ui::HudIconAtlas *icon_atlas_ = nullptr;
	std::vector<MinimapIconDraw> icons_;
	bf3::core::ui::ScreenPoint last_marker_;
	std::vector<bf3::core::ui::ScreenPoint> last_icons_;
};

} // namespace bf3::godot_side
