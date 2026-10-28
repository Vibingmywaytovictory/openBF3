#include "bf3/core/ui/ui_stage.h"

#include <algorithm>
#include <cmath>

namespace bf3::core::ui {

UiStage ui_stage_show_all(const SwfRect &p_stage, float p_width_px, float p_height_px) {
	UiStage s;
	s.stage = p_stage;
	s.width_px = std::isfinite(p_width_px) ? std::max(1.0f, p_width_px) : 1.0f;
	s.height_px = std::isfinite(p_height_px) ? std::max(1.0f, p_height_px) : 1.0f;
	const double w = std::max(1.0, double(p_stage.width()));
	const double h = std::max(1.0, double(p_stage.height()));
	s.scale = std::min(double(s.width_px) / w, double(s.height_px) / h);
	s.offset_x = (double(s.width_px) - w * s.scale) * 0.5 - double(p_stage.x_min) * s.scale;
	s.offset_y = (double(s.height_px) - h * s.scale) * 0.5 - double(p_stage.y_min) * s.scale;
	s.visible = SwfRect{ float(-s.offset_x / s.scale), float((double(s.width_px) - s.offset_x) / s.scale),
		float(-s.offset_y / s.scale), float((double(s.height_px) - s.offset_y) / s.scale) };
	return s;
}

UiStage ui_stage_show_all(float p_width_px, float p_height_px) {
	return ui_stage_show_all(kUiScreenStage, p_width_px, p_height_px);
}

SwfPoint ui_stage_to_window(const UiStage &p_stage, SwfPoint p_stage_point) {
	return SwfPoint{ float(double(p_stage_point.x) * p_stage.scale + p_stage.offset_x),
		float(double(p_stage_point.y) * p_stage.scale + p_stage.offset_y) };
}

SwfPoint ui_window_to_stage(const UiStage &p_stage, SwfPoint p_window_point) {
	return SwfPoint{ float((double(p_window_point.x) - p_stage.offset_x) / p_stage.scale),
		float((double(p_window_point.y) - p_stage.offset_y) / p_stage.scale) };
}

}
