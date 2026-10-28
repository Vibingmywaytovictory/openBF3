#pragma once

#include <cstdint>

namespace bf3::core::ui {

inline constexpr int32_t kScreenWidthChanged = 0;
inline constexpr int32_t kScreenHeightChanged = 1;
inline constexpr double kScreenAdjustDefault = 0.95;
inline constexpr double kScreenAdjustMin = 0.85;
inline constexpr double kScreenAdjustMax = 1.0;
inline constexpr double kScreenAdjustStep = 0.01;

struct ScreenAdjustFractions {
	double width = kScreenAdjustDefault;
	double height = kScreenAdjustDefault;
};

struct ScreenAdjustField {
	double x = 0.0;
	double y = 0.0;
	double red_width = 0.0;
	double red_height = 0.0;
};

struct ScreenAdjustFields {
	ScreenAdjustField left;
	ScreenAdjustField top;
	ScreenAdjustField right;
	ScreenAdjustField bottom;
};

double screen_adjust_step(double p_fraction, bool p_grow);
ScreenAdjustFields screen_adjust_fields(double p_width, double p_height, const ScreenAdjustFractions &p_fractions);
double screen_safe_margin(double p_size, double p_fraction);
float hud_safe_offset(float p_footage_offset, float p_size, double p_fraction);

}
