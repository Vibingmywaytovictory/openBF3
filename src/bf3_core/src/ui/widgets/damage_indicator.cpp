#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

class DamageIndicatorClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override { r_host.self().set_visible(false); }

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key != "Indicator" || !p_value.defined()) {
			return;
		}
		for (size_t i = 0; i < kHudDamageIndicators; ++i) {
			if (p_value.size() < i) {
				continue;
			}
			const As2Value &e = p_value.at(i);
			const As2Clip ind = r_host.clip("indicator" + std::to_string(i));
			const As2Value &angle = e.field("Angle");
			if (angle.defined()) {
				ind.goto_and_stop(int32_t(angle.to_number()));
			}
			if (e.field("New").kind == As2Value::Kind::Bool && e.field("New").boolean) {
				ind.child("arrow").goto_and_play(e.field("Scale").to_number() < kMinorScale ? kMinorHitFrame : kFullHitFrame);
			}
		}
	}

	void bind(const HudModel &, const HudClientState &p_client, HudData &r_out) const override {
		if (p_client.damage != nullptr) {
			r_out.push_back({ "Indicator", p_client.damage->indicators() });
		}
	}

private:
	static constexpr double kMinorScale = 50.0;
	static constexpr int32_t kMinorHitFrame = 31;
	static constexpr int32_t kFullHitFrame = 2;
};

}

void register_hud_widgets_damage_indicator(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Hud.DamageIndicator", [] { return std::make_unique<DamageIndicatorClass>(); });
}

}
