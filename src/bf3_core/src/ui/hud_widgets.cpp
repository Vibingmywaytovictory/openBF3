#include "widgets/hud_widget_support.h"

namespace bf3::core::ui {

int32_t hud_compass_frame(float p_heading_deg) {
	if (!std::isfinite(p_heading_deg)) {
		return 0;
	}
	const int32_t h = int32_t(std::fmod(std::ceil(double(p_heading_deg)), 360.0));
	return ((h % 360) + 360) % 360;
}

double hud_crosshair_dispersion(float p_spread_deg) {
	if (!(p_spread_deg > 0.0f) || !std::isfinite(p_spread_deg)) {
		return 0.0;
	}
	return double(p_spread_deg) * kHudCrosshairDispersionPerDegree;
}

double hud_crosshair_arm(float p_spread_deg, double p_base_offset) {
	return as2_round((hud_crosshair_dispersion(p_spread_deg) + p_base_offset) * 100.0);
}

float hud_crosshair_spread_for_arm(double p_arm, double p_base_offset) {
	const double d = p_arm / 100.0 - p_base_offset;
	return d > 0.0 && std::isfinite(d) ? float(d / kHudCrosshairDispersionPerDegree) : 0.0f;
}

double hud_bg_light_ramp_fraction_x() {
	const double fade = 1.0 / kHorizontalFadePercentage;
	return fade / (1.0 + fade);
}

double hud_bg_light_ramp_fraction_y() {
	const double fade = 1.0 / kVerticalFadePercentage;
	return fade / (1.0 + fade);
}

namespace {

float bg_ramp(float p_start, float p_full, float p_inward_of_fade) {
	const float span = p_full - p_start;
	if (!(span > 0.0f)) {
		return p_inward_of_fade >= p_start ? 1.0f : 0.0f;
	}
	return std::clamp((p_inward_of_fade - p_start) / span, 0.0f, 1.0f);
}

}

float hud_bg_light_ramp(const HudBgDrawLook &p_look, float p_inward_of_fade) {
	return bg_ramp(p_look.light_start_of_fade, p_look.light_full_of_fade, p_inward_of_fade);
}

float hud_bg_coverage_ramp(const HudBgDrawLook &p_look, float p_inward_of_fade) {
	return bg_ramp(p_look.coverage_start_of_fade, p_look.coverage_full_of_fade, p_inward_of_fade);
}

float hud_ekg_beat_px(const HudEkgLook &p_look, float p_distance_px) {
	const float d = p_distance_px;
	if (!(d >= 0.0f)) {
		return 0.0f;
	}
	if (d < p_look.rise_px) {
		return p_look.peak_px * d / p_look.rise_px;
	}
	const float after_rise = d - p_look.rise_px;
	if (after_rise < p_look.fall_px) {
		return p_look.peak_px - (p_look.peak_px + p_look.undershoot_px) * after_rise / p_look.fall_px;
	}
	const float after_fall = after_rise - p_look.fall_px;
	if (after_fall < p_look.recover_px) {
		return -p_look.undershoot_px + p_look.undershoot_px * after_fall / p_look.recover_px;
	}
	return 0.0f;
}

float hud_ekg_offset_px(const HudEkgLook &p_look, double p_clock_seconds, float p_screen_x) {
	if (!std::isfinite(p_clock_seconds) || !std::isfinite(p_screen_x) || !(p_look.scroll_px_per_second > 0.0f) || !(p_look.period_seconds > 0.0f)) {
		return 0.0f;
	}
	const double emitted = p_clock_seconds - double(p_look.rect_x1 - p_screen_x) / double(p_look.scroll_px_per_second);
	double phase = std::fmod(emitted, double(p_look.period_seconds));
	if (phase < 0.0) {
		phase += double(p_look.period_seconds);
	}
	return hud_ekg_beat_px(p_look, float(phase * double(p_look.scroll_px_per_second)));
}

float hud_ekg_alpha(const HudEkgLook &p_look, float p_screen_x) {
	if (!std::isfinite(p_screen_x) || !(p_look.fade_full_x > p_look.fade_start_x)) {
		return 0.0f;
	}
	return std::clamp((p_screen_x - p_look.fade_start_x) / (p_look.fade_full_x - p_look.fade_start_x), 0.0f, 1.0f);
}

}
