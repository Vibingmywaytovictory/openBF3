#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

class OverviewMapClass final : public HudWidgetController {
public:
	static constexpr int32_t kShow = 0;
	static constexpr int32_t kHide = 1;

	void on_clip_load(HudWidgetHost &r_host) override { r_host.renderer(r_host.self(), kMinimapRenderer); }

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &) override { r_host.self().set_visible(true); }

	void in_event(HudWidgetHost &r_host, int32_t p_index, const As2Value &) override {
		if (p_index == kShow) {
			r_host.self().set_visible(true);
		} else if (p_index == kHide) {
			r_host.self().set_visible(false);
		}
	}
};

}

void register_hud_widgets_overview_map_widget(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Image.OverviewMap", [] { return std::make_unique<OverviewMapClass>(); });
}

}
