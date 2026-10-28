#pragma once

#include "bf3/core/ui/swf_movie.h"

namespace bf3::core::ui {

inline constexpr SwfRect kUiScreenStage{ 0.0f, 1280.0f, 0.0f, 720.0f };

struct UiStage {
	float width_px = kUiScreenStage.width();
	float height_px = kUiScreenStage.height();
	double scale = 1.0;
	double offset_x = 0.0;
	double offset_y = 0.0;
	SwfRect stage = kUiScreenStage;
	SwfRect visible = kUiScreenStage;
	bool operator==(const UiStage &) const = default;
};

UiStage ui_stage_show_all(const SwfRect &p_stage, float p_width_px, float p_height_px);
UiStage ui_stage_show_all(float p_width_px, float p_height_px);
SwfPoint ui_stage_to_window(const UiStage &p_stage, SwfPoint p_stage_point);
SwfPoint ui_window_to_stage(const UiStage &p_stage, SwfPoint p_window_point);

}
