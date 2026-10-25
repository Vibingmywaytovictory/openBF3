#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

class PanelMessageClass final : public HudWidgetController {
public:
	struct Spec {
		float x_rotation = 0.0f;
		float y_rotation = 0.0f;
		std::string_view background;
		std::vector<std::string_view> hide_on_initialize;
		bool shown_on_initialize = false;
	};
	explicit PanelMessageClass(Spec p_spec) :
			spec_(std::move(p_spec)) {}

	void on_clip_load(HudWidgetHost &r_host) override {
		r_host.renderer(r_host.self(), "t2");
		r_host.tilt(r_host.self(), spec_.x_rotation, spec_.y_rotation, kHudPerspectiveFov);
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		const As2Clip bg = r_host.clip(spec_.background);
		if (HudBackgroundWidgetApi *api = controller_at<HudBackgroundWidgetApi>(r_host, bg)) {
			api->initialize_without_data();
		}
		bg.set_visible(false);
		for (const std::string_view h : spec_.hide_on_initialize) {
			r_host.clip(h).set_visible(false);
		}
		if (spec_.shown_on_initialize) {
			r_host.self().set_visible(true);
		}
	}

private:
	Spec spec_;
};

}

void register_hud_widgets_panel_message(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Message.LogMessage", [] { return std::make_unique<PanelMessageClass>(PanelMessageClass::Spec{ -5.0f, -15.0f, "container.bg", {}, false }); });
	r_registry.add("Widget.Message.TooltipMessage", [] {
		return std::make_unique<PanelMessageClass>(PanelMessageClass::Spec{ -5.0f, 15.0f, "container.bg",
				{ "container.textTop.textClip.t1", "container.textTop.textClip.t2", "container.textTop.textClip.t3", "container.textTop.textClip.t4",
						"container.textBottom.textClip.t1", "container.textBottom.textClip.t2", "container.textBottom.textClip.t3",
						"container.textBottom.textClip.t4" },
				false });
	});
	r_registry.add("Widget.Message.ObjectiveMessage", [] { return std::make_unique<PanelMessageClass>(PanelMessageClass::Spec{ 5.0f, -15.0f, "header.bg", {}, true }); });
}

}
