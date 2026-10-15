#pragma once
// Bf3MinimapControl: the minimap drawn with CanvasItem calls and one canvas_item shader (docs/design/
// ui-minimap-icons.md §10.2; gap UI §6 "Minimap rendering"). Main thread only.
//
// Coordinates: the control's local canvas units (the project stretches canvas_items, so one unit is
// get_viewport()->get_final_transform() scale screen pixels). MinimapView::center is the control's centre in these units.
//
// _draw():
//   1. The map item (an inner child canvas item that carries the ShaderMaterial; a CanvasItem material applies to all
//      of its item's draws): draw_set_transform(center, rotation = +heading when rotating (clockwise, y-down canvas),
//      scale = pixels per metre x WorldSize / image side), then draw_texture_rect_region of the satellite placed so that
//      texel (u, v) lands where minimap_project puts its world point (minimap.h). The shader (kMinimapShader below)
//      tints the L8 satellite with DetailTextureTint and shades everything outside the mode's combat area with
//      CombatAreaColor: outside = smoothstep(thr - w, thr + w, sdf), thr = CombatAreaAlphaThreshold (0.496), w = 0.01 in
//      distance-field units (edge_width, design §5.5). Two or more masks (CQS) are combined into one R8 union texture
//      on the loader thread (per-texel minimum of the distance values, design D2). The combat-area and detail SDFs cover
//      the same world square as the satellite, so all three are sampled at the same UV.
//   2. The circular frame: the shader discards fragments farther than clip_radius_px from clip_center_px, both in
//      viewport pixels (FRAGCOORD), set every frame from the control's final transform (BF3's minimapmask is round).
//   3. The icon item (a second child above the map item, no material): objectives (objective_icon in hud_icons.h:
//      UIHudIcon_NeutralFlag Neutral / FriendlyFlag Friendly / EnemyFlag Enemy by owner, HQs UIHudIcon_Base Friendly /
//      Enemy, relative to kM1LocalTeam) from the hud3dicons atlas with draw_texture_rect_region (the state's frame
//      rect, hud_icon_rect), at minimap_project(objective), clamped to the rim when outside; label letters through tr().
//   4. The player marker: UIHudIcon_LocalPlayer Default (ShouldRotate) at the centre, rotated by
//      minimap_marker_angle(view).
//   5. The others (M1's walkers, team 2) through soldier_icon: UIHudIcon_Player Enemy for team 2 against local team 1.
// Everything is redrawn on queue_redraw from Bf3MinimapHud::_process; no Godot call happens off the main thread.
//
// Check hooks (minimap_check.h, gate G9 / G10): set_debug_uv(true) makes the shader write the packed texture
// coordinate (minimap.h encode_minimap_debug_uv) instead of the colour; set_icons_visible(false) hides the icon item so
// the read-back sees the map under the marker; canvas_to_viewport() maps control-local points to viewport pixels.

#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/minimap.h"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/transform2d.hpp>

#include <string_view>
#include <vector>

namespace bf3::godot_side {

// The canvas_item shader (design §10.2). Uniforms: satellite (L8), combat_area (R8 union SDF), detail (L8 SDF, unused
// on XP2), tint, area_color, threshold, edge_width, detail_tint, detail_threshold, has_detail, clip_center_px,
// clip_radius_px, debug_uv. The debug_uv packing must stay equal to encode_minimap_debug_uv (minimap.h): 12 bits per
// axis, R = u >> 4, G = (u & 15) << 4 | v >> 8, B = v & 255, alpha 1 (an opaque write, so blending keeps the bytes).
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
uniform vec2 clip_center_px = vec2(0.0);
uniform float clip_radius_px = 1.0e9;
uniform bool debug_uv = false;
void fragment() {
	if (distance(FRAGCOORD.xy, clip_center_px) > clip_radius_px) {
		discard;
	}
	if (debug_uv) {
		vec2 q = floor(clamp(UV, 0.0, 1.0) * 4095.0 + 0.5);
		float uh = floor(q.x / 16.0);
		float ul = q.x - uh * 16.0;
		float vh = floor(q.y / 256.0);
		float vl = q.y - vh * 256.0;
		COLOR = vec4(uh / 255.0, (ul * 16.0 + vh) / 255.0, vl / 255.0, 1.0);
	} else {
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
}
)";

struct MinimapIconDraw {
	const bf3::core::ui::HudIcon *icon = nullptr;
	bf3::core::ui::HudIconState state = bf3::core::ui::HudIconState::Default;
	float world_x = 0.0f;
	float world_y = 0.0f; // the world icons lift from here (VerticalOffset)
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

	// ---- check hooks (main thread) ----------------------------------------------------------------------------------
	void set_debug_uv(bool p_on); // the shader's debug_uv uniform
	void set_icons_visible(bool p_on); // the icon item (objectives, marker, others)
	// Control-local canvas units -> viewport pixels (get_viewport()->get_final_transform() * get_global_transform_with_canvas()).
	godot::Transform2D canvas_to_viewport() const;
	// Where the last _draw put the player marker and each icon (control-local; NaN for none). Self-reported: the check
	// uses them only to find where to read back, never as the evidence (design §10.4).
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
	bool icons_visible_ = true;
	bf3::core::ui::ScreenPoint last_marker_;
	std::vector<bf3::core::ui::ScreenPoint> last_icons_;
};

} // namespace bf3::godot_side
