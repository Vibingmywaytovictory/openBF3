#pragma once

#include "bf3/core/ui/hud_messages.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace bf3::core::ui {

inline constexpr float kHudPerspectiveFov = 25.0f;
inline constexpr std::string_view kBgDrawRenderer = "t2;iBgDraw";
inline constexpr std::string_view kSoldierRenderer = "t2;iUISoldierCompRender";
inline constexpr std::string_view kMinimapRenderer = "t2;iUIMinimapRender";
inline constexpr std::array<std::string_view, 6> kAs2FiringModes{ "single", "single", "auto", "burst", "auto", "single" };
inline constexpr double kHorizontalFadePercentage = 20.0;
inline constexpr double kVerticalFadePercentage = 4.25;

class HudBackgroundApi {
public:
	virtual ~HudBackgroundApi() = default;
	virtual void set_width(double p_value, std::string_view p_dir) = 0;
	virtual void set_height(double p_value, std::string_view p_dir) = 0;
	virtual double start_width() const = 0;
	virtual double start_height() const = 0;
};

class HudBackgroundWidgetApi {
public:
	virtual ~HudBackgroundWidgetApi() = default;
	virtual void initialize_without_data() = 0;
	virtual void set_width(double p_value) = 0;
	virtual void set_height(double p_value) = 0;
	virtual double width_value() const = 0;
	virtual double height_value() const = 0;
	virtual bool hardcore() const = 0;
	virtual std::string tilt_mode() const = 0;
};

class HudMessageApi {
public:
	virtual ~HudMessageApi() = default;
	virtual uint64_t rows_added() const = 0;
	virtual size_t rows() const = 0;
	virtual double max_row_count() const = 0;
	virtual HudMessageSource source() const = 0;
};

struct HudBgDrawLook {
	std::array<float, 3> premul_rgb255{ 0.91f, 31.7f, 54.12f };
	float coverage = 0.1458f;
	float ramp_start_of_fade = 0.36f;
	float ramp_full_of_fade = 1.05f;
};

inline constexpr HudBgDrawLook kHudBgDrawLook{};

struct HudEkgLook {
	float rect_x0 = 1056.0f;
	float rect_y0 = 566.0f;
	float rect_x1 = 1166.0f;
	float rect_y1 = 592.0f;
	float scroll_px_per_second = 78.7f;
	float period_seconds = 1.04f;
	float baseline_y = 578.0f;
	float rise_px = 5.0f;
	float peak_px = 9.0f;
	float fall_px = 5.0f;
	float undershoot_px = 3.0f;
	float recover_px = 8.0f;
	float line_px = 2.0f;
	std::array<uint8_t, 3> rgb{ 201, 221, 232 };
	float fade_start_x = 1055.0f;
	float fade_full_x = 1150.0f;
};

inline constexpr HudEkgLook kHudEkgLook{};

inline constexpr float kMinimapMetresPerLocalUnit = 1.375f;
inline constexpr float kMinimapHatchTileLocal = 10.6f;
inline constexpr float kMinimapSquareLocal = 170.0f;
inline constexpr std::string_view kMinimapMaskTexture = "ui/hud/minimap/minimapmask";
inline constexpr uint32_t kNametagUiColourSlot0 = 0xE4FF9Du;
inline constexpr int32_t kUiHudIconSquadLeader = 7;
inline constexpr int32_t kUiHudIconDead = 145;

int32_t hud_compass_frame(float p_heading_deg);

double hud_crosshair_dispersion(float p_spread_deg, float p_vertical_fov_deg, float p_stage_height);

double hud_bg_light_ramp_fraction_x();
double hud_bg_light_ramp_fraction_y();
float hud_bg_ramp(const HudBgDrawLook &p_look, float p_inward_of_fade);

float hud_ekg_beat_px(const HudEkgLook &p_look, float p_distance_px);
float hud_ekg_offset_px(const HudEkgLook &p_look, double p_clock_seconds, float p_screen_x);
float hud_ekg_alpha(const HudEkgLook &p_look, float p_screen_x);

}
