#pragma once

#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/minimap.h"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/transform2d.hpp>

#include <string_view>
#include <vector>

namespace bf3::godot_side {

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
	float world_y = 0.0f;
	float world_z = 0.0f;
	float yaw = 0.0f;
	godot::String label;
	bool objective = true;
};

class Bf3MinimapControl : public godot::Control {
	GDCLASS(Bf3MinimapControl, godot::Control)

public:
	Bf3MinimapControl();
	~Bf3MinimapControl() override;

	void _draw() override;

	void setup(const bf3::core::ui::MinimapData &p_data, godot::Ref<godot::ImageTexture> p_satellite, godot::Ref<godot::ShaderMaterial> p_material,
			godot::Ref<godot::ImageTexture> p_atlas, const bf3::core::ui::HudIconAtlas *p_icons);

	void set_view(const bf3::core::ui::MinimapView &p_view);
	void set_icons(std::vector<MinimapIconDraw> p_icons);

	float disc_radius() const;
	float rim_radius() const;

	float icon_scale() const;

	void set_debug_uv(bool p_on);
	void set_icons_visible(bool p_on);
	void set_marker_only(bool p_on);

	godot::Transform2D canvas_to_viewport() const;

	bf3::core::ui::ScreenPoint last_marker() const { return last_marker_; }
	const std::vector<bf3::core::ui::ScreenPoint> &last_icons() const { return last_icons_; }

	const std::vector<godot::Rect2> &last_icon_rects() const { return last_icon_rects_; }
	const std::vector<bool> &last_clamped() const { return last_clamped_; }
	const std::vector<MinimapIconDraw> &icons() const { return icons_; }
	const bf3::core::ui::MinimapView &view() const { return view_; }

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	void ensure_items();
	void draw_icon(const MinimapIconDraw &p_icon, godot::Vector2 p_at, float p_angle, godot::Rect2 *r_rect);

	bf3::core::ui::MinimapData data_;
	bf3::core::ui::MinimapView view_;
	godot::Ref<godot::ImageTexture> satellite_;
	godot::Ref<godot::ShaderMaterial> material_;
	godot::Ref<godot::ImageTexture> atlas_;
	const bf3::core::ui::HudIconAtlas *icon_atlas_ = nullptr;
	std::vector<MinimapIconDraw> icons_;
	bool icons_visible_ = true;
	bool marker_only_ = false;
	bool debug_uv_ = false;
	godot::RID map_item_;
	godot::RID icon_item_;
	bf3::core::ui::ScreenPoint last_marker_;
	std::vector<bf3::core::ui::ScreenPoint> last_icons_;
	std::vector<godot::Rect2> last_icon_rects_;
	std::vector<bool> last_clamped_;
};

}
