#include "bf3/core/ui/screen_adjust.h"

#include <cmath>

namespace bf3::core::ui {

namespace {

double as2_round(double p_v) {
	return std::floor(p_v + 0.5);
}

}

double screen_adjust_step(double p_fraction, bool p_grow) {
	if (p_grow) {
		const double v = p_fraction + kScreenAdjustStep;
		return v > kScreenAdjustMax ? kScreenAdjustMax : v;
	}
	const double v = p_fraction - kScreenAdjustStep;
	return v < kScreenAdjustMin ? kScreenAdjustMin : v;
}

double screen_safe_margin(double p_size, double p_fraction) {
	return as2_round(p_size * (1.0 - p_fraction) / 2.0);
}

float hud_safe_offset(float p_footage_offset, float p_size, double p_fraction) {
	if (!std::isfinite(p_fraction)) {
		return p_footage_offset;
	}
	return p_footage_offset + float(double(p_size) * (kScreenAdjustDefault - p_fraction) / 2.0);
}

ScreenAdjustFields screen_adjust_fields(double p_width, double p_height, const ScreenAdjustFractions &p_fractions) {
	const double left = as2_round(screen_safe_margin(p_width, p_fractions.width));
	const double right = as2_round(p_width - left);
	const double top = as2_round(screen_safe_margin(p_height, p_fractions.height));
	const double bottom = as2_round(p_height - top);
	const double mid_x = as2_round(p_width / 2.0);
	const double mid_y = as2_round(p_height / 2.0);
	ScreenAdjustFields f;
	f.left = { left, mid_y, left, bottom - top };
	f.top = { mid_x, top, top, p_width };
	f.right = { right, mid_y, left, bottom - top };
	f.bottom = { mid_x, bottom, top, p_width };
	return f;
}

}
