#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

constexpr int32_t kKeyChanged = 0;

class ChangeListenerClass final : public HudWidgetController {
public:
	void attach_node(HudWidgetHost &, const HudWidgetNode &p_node) override { sources_ = p_node.sources; }

	void bind(const HudModel &, const HudClientState &p_client, HudData &r_out) const override {
		hud_flow_sources(sources_, p_client, r_out);
	}

	void construct(HudWidgetHost &r_host) override { r_host.self().set_visible(false); }

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		r_host.self().set_visible(false);
		setup_refresh_data(*this, r_host, p_init.data);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Key") {
			r_host.fire_event(kKeyChanged, p_value);
		}
	}

private:
	std::vector<HudDataSource> sources_;
};

}

void register_hud_widgets_change_listener(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Listener.ChangeListener", [] { return std::make_unique<ChangeListenerClass>(); });
}

}
