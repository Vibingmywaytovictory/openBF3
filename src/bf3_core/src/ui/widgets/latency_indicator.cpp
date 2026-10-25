#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

class LatencyIndicatorClass final : public HudWidgetController {
public:
	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override { setup_refresh_data(*this, r_host, p_init.data); }

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Visible") {
			r_host.self().set_visible(p_value.truthy());
		}
	}

	void bind(const HudModel &, const HudClientState &p_client, HudData &r_out) const override {
		r_out.push_back({ "Visible", As2Value::of(p_client.latency_warning) });
	}
};

}

void register_hud_widgets_latency_indicator(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Misc.LatencyIndicator", [] { return std::make_unique<LatencyIndicatorClass>(); });
}

}
