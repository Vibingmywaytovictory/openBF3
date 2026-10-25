#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

class ScreenClass final : public HudWidgetController {
public:
	bool screen_initialized() const override { return false; }
};

constexpr float kPopupStartScale = 0.8f;
constexpr double kPopupEnterSeconds = 0.4;
constexpr std::string_view kPopupXScale = "_xscale";
constexpr std::string_view kPopupYScale = "_yscale";

class PopupScreenClass final : public HudWidgetController {
public:
	bool screen_initialized() const override { return false; }

	void initialize_screen(HudWidgetHost &r_host) override {
		const As2Clip self = r_host.self();
		const float x = self.x();
		const float y = self.y();
		const HudStageRects &rects = r_host.screen().stage_rects();
		self.set_xscale(kPopupStartScale * 100.0f);
		self.set_yscale(kPopupStartScale * 100.0f);
		self.set_x(x + rects.original.width() * (1.0f - kPopupStartScale) / 2.0f);
		self.set_y(y + rects.original.height() * (1.0f - kPopupStartScale) / 2.0f);
		self.set_alpha(0.0f);
		r_host.add_tween(self, HudTweenProperty::Alpha, 100.0f, kPopupEnterSeconds, HudTweenEase::ExpoOut);
		r_host.add_custom_tween(kPopupYScale, 100.0f, kPopupEnterSeconds, HudTweenEase::ExpoOut);
		r_host.add_custom_tween(kPopupXScale, 100.0f, kPopupEnterSeconds, HudTweenEase::ExpoOut);
		r_host.add_tween(self, HudTweenProperty::Y, y, kPopupEnterSeconds, HudTweenEase::ExpoOut);
		r_host.add_tween(self, HudTweenProperty::X, x, kPopupEnterSeconds, HudTweenEase::ExpoOut);
	}

	std::optional<float> tween_property(HudWidgetHost &r_host, std::string_view p_name) override {
		if (p_name == kPopupXScale) {
			return r_host.self().xscale();
		}
		if (p_name == kPopupYScale) {
			return r_host.self().yscale();
		}
		return std::nullopt;
	}

	void set_tween_property(HudWidgetHost &r_host, std::string_view p_name, float p_value) override {
		if (p_name == kPopupXScale) {
			r_host.self().set_xscale(p_value);
		} else if (p_name == kPopupYScale) {
			r_host.self().set_yscale(p_value);
		}
	}
};
}

void register_hud_widgets_screen(HudWidgetRegistry &r_registry) {
	r_registry.add("fb.Base.UIScreen", [] { return std::make_unique<ScreenClass>(); });
	r_registry.add("fb.Base.UIPopup", [] { return std::make_unique<PopupScreenClass>(); });
	r_registry.add("Screen.Weapon.CrosshairDefault", [] { return std::make_unique<ScreenClass>(); });
}

}
