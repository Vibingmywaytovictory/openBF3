#pragma once

#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/result.h"
#include "bf3/core/ui/swf_movie.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace bf3::core {
class EbxResolver;
}

namespace bf3::core::ui {

inline constexpr std::string_view kAlerttagCompPartition = "ui/uicomponents/uialerttagcomp";
inline constexpr std::string_view kTeamSupportTagCompPartition = "ui/uicomponents/uiteamsupporttagcomp";
inline constexpr std::string_view kMapmarkerTagCompPartition = "ui/uicomponents/uimapmarkertagcomp";

struct Icon3dStyle {
	float draw_distance = 15.0f;
	float minimum_draw_distance = 0.0f;
	float fade_distance = 8.0f;
	float fade_end_distance = 15.0f;
	float max_far_fade = 0.0f;
	float minimum_fade_distance = 0.0f;
	float max_close_fade = 0.0f;
	float scale_distance = 15.0f;
	float max_scale_mod = 0.85f;
	float vertical_offset = 0.0f;
	std::array<float, 2> pixel_offset{ 0.0f, 0.0f };
	bool snap_icons = true;
	bool circular_snap = false;
	bool only_show_snapped = false;
	float snap_safe_zone = 0.95f;
	float snap_center_y_offset = 0.15f;
	float tracker_radius_x = 0.85f;
	float tracker_radius_y = 0.6f;
	bool from_data = false;
	uint32_t clamped = 0;
};

Result<Icon3dStyle> read_icon3d_style(
		const EbxResolver &p_level, std::string_view p_partition, gameplay::WorkBudget *p_budget = nullptr);

struct Icon3dPlacement {
	bool shown = false;
	bool snapped = false;
	float x = 0.0f;
	float y = 0.0f;
	float scale = 1.0f;
	float alpha = 1.0f;
};

float icon3d_scale(const Icon3dStyle &p_style, float p_distance_m);
float icon3d_alpha(const Icon3dStyle &p_style, float p_distance_m);
SwfPoint icon3d_snap_origin(const Icon3dStyle &p_style, const SwfRect &p_visible);
SwfRect icon3d_snap_box(const Icon3dStyle &p_style, const SwfRect &p_visible);
Icon3dPlacement place_icon3d(
		const Icon3dStyle &p_style, float p_distance_m, bool p_in_front, SwfPoint p_stage, const SwfRect &p_visible);

}
