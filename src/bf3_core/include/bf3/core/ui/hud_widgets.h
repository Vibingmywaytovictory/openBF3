#pragma once

#include <array>
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

int32_t hud_compass_frame(float p_heading_deg);

double hud_crosshair_dispersion(float p_spread_deg, float p_vertical_fov_deg, float p_stage_height);

}
