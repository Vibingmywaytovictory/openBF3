#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

class CrosshairClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		for (const char *n : { "crosshair", "circle", "dot", "grenade" }) {
			r_host.clip(n).set_visible(false);
		}
		run_initialize(r_host, nullptr);
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override { run_initialize(r_host, &p_init); }

	void attach_node(HudWidgetHost &, const HudWidgetNode &p_node) override { base_offset_ = p_node.dispersion_base_offset.value_or(0.0); }

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Visible") {
			r_host.self().set_visible(string_true(p_value));
		} else if (p_key == "BaseValue") {
			base_ = p_value.to_number();
			update_dispersion(r_host, current_ + base_);
		} else if (p_key == "Dispersion") {
			current_ = p_value.to_number();
			update_dispersion(r_host, current_ + base_);
		} else if (p_key == "Zoomed") {
			zoomed_ = p_value.truthy();
			if (zoomed_ != was_zoomed_) {
				const As2Clip c = r_host.clip(type_);
				if (hide_on_zoom_) {
					if (zoomed_) {
						r_host.add_tween(c, HudTweenProperty::Alpha, 0.0f, kFadeOutSeconds, HudTweenEase::Linear);
					} else {
						r_host.remove_tweens(c);
						update_dispersion(r_host, current_ + base_);
						r_host.add_tween(c, HudTweenProperty::Alpha, 100.0f, kFadeInSeconds, HudTweenEase::Linear);
					}
				}
				was_zoomed_ = zoomed_;
			}
		}
	}

	void bind(const HudModel &p_model, const HudClientState &, HudData &r_out) const override {
		const HudWeapon &w = p_model.weapon;
		r_out.push_back({ "Visible",
				As2Value::of(p_model.soldier.is_alive && w.active_weapon_crosshair != 0 && !p_model.user.is_in_menu) });
		r_out.push_back({ "BaseValue", As2Value::of(base_offset_) });
		r_out.push_back({ "Dispersion", As2Value::of(hud_crosshair_dispersion(w.dispersion)) });
		r_out.push_back({ "Zoomed", As2Value::of(w.zoomed) });
	}

	bool hide_on_zoom() const { return hide_on_zoom_; }

private:
	void run_initialize(HudWidgetHost &r_host, const HudWidgetInit *p_init) {
		if (p_init != nullptr) {
			if (const std::string *h = p_init->property("HideOnZoom"); h != nullptr && !h->empty()) {
				hide_on_zoom_ = to_lower_ascii(*h) == "true";
			}
		}
		type_ = param_string(r_host, "m_crosshairType");
		if (type_.empty() || type_ == "undefined") {
			type_ = "crosshair";
		}
		const As2Clip c = r_host.clip(type_);
		c.set_visible(true);
		r_host.renderer(c, "x1");
		if (p_init != nullptr) {
			setup_refresh_data(*this, r_host, p_init->data);
		}
	}

	void update_dispersion(HudWidgetHost &r_host, double p_d) {
		const double r = as2_round(p_d * 100.0);
		if (std::isnan(r)) {
			return;
		}
		const As2Clip c = r_host.clip(type_);
		if (type_ == "crosshair") {
			c.child("up").set_y(float(-r));
			c.child("down").set_y(float(r));
			c.child("left").set_x(float(-r));
			c.child("right").set_x(float(r));
		} else if (type_ == "circle") {
			c.set_xscale(float(64.0 + r / 3.0));
			c.set_yscale(float(64.0 + r / 3.0));
		}
	}

	static constexpr double kFadeOutSeconds = 0.2;
	static constexpr double kFadeInSeconds = 0.1;
	std::string type_ = "crosshair";
	bool hide_on_zoom_ = false;
	double base_offset_ = 0.0;
	double base_ = 0.0;
	double current_ = 0.0;
	bool zoomed_ = false;
	bool was_zoomed_ = false;
};

class HitindicatorClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		r_host.self().set_visible(false);
		r_host.clip("hitindicator").goto_and_stop("idle");
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
		r_host.clip("hitindicator").set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "IsInVehicle") {
			in_vehicle_ = p_value.truthy();
		} else if (p_key == "IndicatorValue") {
			if (in_vehicle_) {
				return;
			}
			const As2Clip hit = r_host.clip("hitindicator");
			const double d = p_value.to_number();
			if (!p_value.defined() || d == 0.0) {
				hit.goto_and_stop(0);
				return;
			}
			if (d >= 0.0 && d < double(kHitLevelFrames.size()) && hit.current_frame() != uint32_t(kHitLevelFrames[size_t(d)])) {
				hit.child("HitController").goto_and_stop(1);
				hit.goto_and_stop(kHitLevelFrames[size_t(d)]);
			}
		}
	}

	void advance(HudWidgetHost &r_host, double) override { hit_.watch(r_host.clip("hitindicator")); }

	void bind(const HudModel &p_model, const HudClientState &, HudData &r_out) const override {
		const HudWeapon &w = p_model.weapon;
		const double level = w.show_hitindicator ? std::clamp(std::round(double(w.hitindicator_level)), 1.0, 3.0) : 0.0;
		r_out.push_back({ "IndicatorValue", As2Value::of(level) });
		r_out.push_back({ "IsInVehicle", As2Value::of(false) });
	}

private:
	bool in_vehicle_ = false;
	HitAnimation hit_;
};

}

void register_hud_widgets_crosshair(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Crosshair.Crosshair", [] { return std::make_unique<CrosshairClass>(); });
	r_registry.add("Widget.Crosshair.Hitindicator", [] { return std::make_unique<HitindicatorClass>(); });
}

}
