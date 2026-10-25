#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

class HiddenUntilDataClass final : public HudWidgetController {
public:
	struct Spec {
		std::vector<std::string_view> hide;
		bool shake = false;
		bool tilt = false;
	};
	explicit HiddenUntilDataClass(Spec p_spec) :
			spec_(std::move(p_spec)) {}

	bool screen_initialized() const override { return false; }

	void on_clip_load(HudWidgetHost &r_host) override {
		for (const std::string_view h : spec_.hide) {
			r_host.clip(h).set_visible(false);
		}
		if (spec_.shake) {
			r_host.renderer(r_host.self(), "t2");
		}
		if (spec_.tilt) {
			r_host.tilt(r_host.self(), -15.0f, 15.0f, kHudPerspectiveFov);
		}
		r_host.self().set_visible(false);
	}

private:
	Spec spec_;
};

}

void register_hud_widgets_hidden_until_data(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Hud.PassangerList", [] { return std::make_unique<HiddenUntilDataClass>(HiddenUntilDataClass::Spec{ {}, true, false }); });
	r_registry.add("Widget.Hud.VehicleHealth", [] { return std::make_unique<HiddenUntilDataClass>(HiddenUntilDataClass::Spec{ {}, true, true }); });
}

}
