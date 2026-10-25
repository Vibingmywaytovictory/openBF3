#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

class HealthClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		const As2Clip holder = r_host.clip("Health.healthHolder");
		r_host.renderer(holder.child("EKGRenderTarget"), kSoldierRenderer);
		r_host.renderer(holder, "t2");
		r_host.tilt(holder, -15.0f, 15.0f, kHudPerspectiveFov);
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		const As2Clip holder = r_host.clip("Health.healthHolder");
		const std::string *prop = p_init.property("HealthData");
		As2Value health = prop != nullptr && !prop->empty() ? As2Value::of(*prop) : As2Value::undefined();
		if (!health.defined()) {
			if (const As2Value *v = p_init.value("Health")) {
				health = *v;
			}
		}
		set_health(r_host, health);
		if (HudBackgroundWidgetApi *bg = controller_at<HudBackgroundWidgetApi>(r_host, holder.child("bgHealth"))) {
			bg->initialize_without_data();
		}
		holder.goto_and_stop("us");
		const As2Value *visible = p_init.value("Visible");
		r_host.self().set_visible(visible != nullptr ? visible->truthy() : true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Health") {
			set_health(r_host, p_value);
		} else if (p_key == "Visible") {
			r_host.self().set_visible(p_value.truthy());
		}
	}

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_out) const override {
		r_out.push_back({ "Health", As2Value::of(double(p_model.soldier.health)) });
		r_out.push_back({ "Visible", As2Value::of(hud_soldier_widgets_visible(p_model, p_client)) });
	}

private:
	static void set_health(HudWidgetHost &r_host, const As2Value &p_value) {
		double n = as2_round(p_value.to_number());
		if (n < 0.0 || !p_value.defined()) {
			n = 0.0;
		}
		r_host.clip("Health.healthHolder.amtxt.txtDisplay").set_text(as2_number_string(n));
	}
};

}

void register_hud_widgets_health(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Hud.Health", [] { return std::make_unique<HealthClass>(); });
}

}
