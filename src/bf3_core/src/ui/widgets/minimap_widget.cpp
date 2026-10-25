#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

class MinimapClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		r_host.renderer(r_host.clip("map"), kMinimapRenderer);
		r_host.tilt(r_host.self(), -15.0f, -15.0f, kHudPerspectiveFov);
		orig_w_ = r_host.self().width();
		orig_h_ = r_host.self().height();
		orig_x_ = r_host.self().x();
		orig_y_ = r_host.self().y();
		r_host.clip("hitindicator").goto_and_stop("idle");
	}

	void pre_update_screen_size(HudWidgetHost &r_host) override { update_size(r_host, true, true); }

	void update_screen_size(HudWidgetHost &r_host) override {
		orig_w_ = r_host.self().width();
		orig_h_ = r_host.self().height();
		orig_x_ = r_host.self().x();
		orig_y_ = r_host.self().y();
		update_size(r_host, small_, true);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		const As2Value *visible = p_init.value("Visible");
		r_host.self().set_visible(visible != nullptr ? visible->truthy() : true);
		r_host.clip("hitindicator").set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Size") {
			update_size(r_host, p_value.truthy(), false);
		} else if (p_key == "Zoom") {
			zoom_ = p_value.to_number();
		} else if (p_key == "ShowCross") {
			r_host.clip("testCross").set_visible(p_value.truthy());
			r_host.clip("hitindicator").set_visible(p_value.truthy());
		} else if (p_key == "HitIndicatorValue") {
			const As2Clip cross = r_host.clip("testCross");
			if (!cross.visible()) {
				return;
			}
			const As2Clip hit = r_host.clip("hitindicator");
			const double d = p_value.to_number();
			if (!p_value.defined() || d == 0.0) {
				hit.goto_and_stop(0);
			} else if (d >= 0.0 && d < double(kHitLevelFrames.size()) && hit.current_frame() != uint32_t(kHitLevelFrames[size_t(d)])) {
				hit.child("HitController").goto_and_stop(1);
				hit.goto_and_stop(kHitLevelFrames[size_t(d)]);
			}
		} else if (p_key == "ShowAirRadar") {
			if (p_value.truthy()) {
				r_host.clip("tint").set_visible(false);
				r_host.self().set_y(float(orig_y_ + 20.0));
			} else {
				r_host.clip("tint").set_visible(true);
				r_host.self().set_y(float(orig_y_));
			}
		}
	}

	void advance(HudWidgetHost &r_host, double) override { hit_.watch(r_host.clip("hitindicator")); }

	void tween_complete(HudWidgetHost &, uint32_t) override {}

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_out) const override {
		r_out.push_back({ "Size", As2Value::of(p_client.minimap_small) });
		r_out.push_back({ "Zoom", As2Value::of(double(p_client.minimap_zoom)) });
		r_out.push_back({ "ShowCross", As2Value::of(p_client.show_minimap_cross) });
		r_out.push_back({ "Visible", As2Value::of(!p_client.hardcore) });
		r_out.push_back({ "ShowAirRadar", As2Value::of(p_model.minimap.air_radar_mask_enabled || p_client.show_air_radar) });
	}

	double zoom() const { return zoom_; }
	bool small() const { return small_; }

private:
	void update_size(HudWidgetHost &r_host, bool p_small, bool p_always) {
		if (p_small == small_ && !p_always) {
			return;
		}
		const double w = p_small ? orig_w_ : kLargeSize;
		const double h = p_small ? orig_h_ : kLargeSize;
		const double x = p_small ? orig_x_ : orig_x_ - 40.0;
		const double y = p_small ? orig_y_ : orig_y_ + 50.0 + (orig_h_ - kLargeSize);
		r_host.remove_tweens(r_host.self());
		if (!p_always) {
			r_host.clip("tint").goto_and_play(p_small ? "MaxOut" : "MaxIn");
			r_host.add_tween(r_host.self(), HudTweenProperty::Width, float(w), kTweenSeconds, HudTweenEase::EaseOutBack, 1);
			r_host.add_tween(r_host.self(), HudTweenProperty::Height, float(h), kTweenSeconds, HudTweenEase::EaseOutBack);
			r_host.add_tween(r_host.self(), HudTweenProperty::X, float(x), kTweenSeconds, HudTweenEase::EaseOutBack);
			r_host.add_tween(r_host.self(), HudTweenProperty::Y, float(y), kTweenSeconds, HudTweenEase::EaseOutBack);
			small_ = p_small;
		} else {
			r_host.self().set_width(float(w));
			r_host.self().set_height(float(h));
			r_host.self().set_x(float(orig_x_));
			r_host.self().set_y(float(p_small ? orig_y_ : orig_y_ + (orig_h_ - kLargeSize)));
		}
	}

	static constexpr double kLargeSize = 600.0;
	static constexpr double kTweenSeconds = 0.05;
	double orig_w_ = 0.0;
	double orig_h_ = 0.0;
	double orig_x_ = 0.0;
	double orig_y_ = 0.0;
	double zoom_ = 0.0;
	bool small_ = true;
	HitAnimation hit_;
};

}

void register_hud_widgets_minimap_widget(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Hud.Minimap", [] { return std::make_unique<MinimapClass>(); });
}

}
