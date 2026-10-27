#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

class HudBackgroundClass final : public HudWidgetController, public HudBackgroundApi {
public:
	bool screen_initialized() const override { return false; }

	void construct(HudWidgetHost &r_host) override {
		host_ = &r_host;
		const As2Clip render = r_host.clip("renderHolder");
		const As2Clip guide = render.child("guide_mc");
		if (guide.exists()) {
			guide.set_visible(false);
		}
		start_x_ = render.x();
		start_y_ = render.y();
		start_w_ = r_host.self().width();
		start_h_ = r_host.self().height();
		horizontal_ = param_string(r_host, "alignHorizontal");
		vertical_ = param_string(r_host, "alignVertical");
		set_width(start_w_, "");
		set_height(start_h_, "");
		render.goto_and_play("us");
	}

	void on_clip_load(HudWidgetHost &r_host) override { r_host.renderer(r_host.clip("renderHolder"), kBgDrawRenderer); }

	void set_width(double p_value, std::string_view p_dir) override {
		const As2Clip render = host_->clip("renderHolder");
		const double fade = p_value / kHorizontalFadePercentage;
		render.set_width(float(as2_round(p_value + fade)));
		const std::string dir = p_dir.empty() ? std::string("left") : std::string(p_dir);
		if (dir == "left") {
			render.set_x(float(-fade / 2.0));
		} else if (dir == "right") {
			render.set_x(float(start_w_ - render.width() + fade / 2.0));
		} else if (dir == "center") {
			render.set_x(float(start_w_ / 2.0 - render.width() / 2.0));
		}
	}

	void set_height(double p_value, std::string_view p_dir) override {
		const As2Clip render = host_->clip("renderHolder");
		const double fade = p_value / kVerticalFadePercentage;
		render.set_height(float(as2_round(p_value + fade)));
		const std::string dir = p_dir.empty() ? std::string("top") : std::string(p_dir);
		if (dir == "top") {
			render.set_y(float(-fade / 2.0));
		} else if (dir == "bottom") {
			render.set_y(float(start_h_ - render.height() + fade / 2.0));
		} else if (dir == "center") {
			render.set_y(float(start_h_ / 2.0 - render.height() / 2.0));
		}
	}

	double start_width() const override { return start_w_; }
	double start_height() const override { return start_h_; }

private:
	HudWidgetHost *host_ = nullptr;
	double start_x_ = 0.0;
	double start_y_ = 0.0;
	double start_w_ = 0.0;
	double start_h_ = 0.0;
	std::string horizontal_;
	std::string vertical_;
};

class HudBackgroundWidgetClass final : public HudWidgetController, public HudBackgroundWidgetApi {
public:
	void construct(HudWidgetHost &r_host) override {
		host_ = &r_host;
		start_w_ = r_host.self().width();
		start_h_ = r_host.self().height();
		start_y_ = r_host.self().y();
		r_host.self().set_visible(false);
	}

	void on_clip_load(HudWidgetHost &r_host) override {
		if (const As2Value *shake = r_host.construct_param("m_enableShake"); shake != nullptr && shake->truthy()) {
			r_host.renderer(r_host.self(), "t2");
		}
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override { run_initialize(r_host, &p_init); }

	void initialize_without_data() override { run_initialize(*host_, nullptr); }

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Visible") {
			if (!hardcore_) {
				r_host.self().set_visible(p_value.truthy());
			}
		} else if (p_key == "ShowAirRadar") {
			set_height(p_value.truthy() ? kAirRadarHeight : height_);
		}
	}

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_out) const override {
		r_out.push_back({ "Visible", As2Value::of(p_client.minimap_small && !p_model.user.is_in_menu) });
		r_out.push_back({ "HideForHardcore", As2Value::of(p_client.hardcore) });
		r_out.push_back({ "ShowAirRadar", As2Value::of(p_model.minimap.air_radar_mask_enabled || p_client.show_air_radar) });
	}

	void set_width(double p_value) override {
		if (p_value != 0.0 && !std::isnan(p_value)) {
			if (HudBackgroundApi *bg = background()) {
				bg->set_width(p_value, "");
			}
		}
	}

	void set_height(double p_value) override {
		if (p_value != 0.0 && !std::isnan(p_value)) {
			if (HudBackgroundApi *bg = background()) {
				bg->set_height(p_value, "");
			}
		}
	}

	double width_value() const override { return width_; }
	double height_value() const override { return height_; }
	bool hardcore() const override { return hardcore_; }
	std::string tilt_mode() const override { return tilt_; }

private:
	HudBackgroundApi *background() { return controller_at<HudBackgroundClass>(*host_, host_->clip("Background")); }

	void run_initialize(HudWidgetHost &r_host, const HudWidgetInit *p_init) {
		const std::string *tilt = p_init != nullptr ? p_init->property("m_enableTilt") : nullptr;
		tilt_ = tilt != nullptr ? *tilt : std::string("undefined");
		if (tilt_ != "none") {
			if (tilt_ == "left") {
				r_host.tilt(r_host.self(), -15.0f, -15.0f, kHudPerspectiveFov);
			} else if (tilt_ == "right") {
				r_host.tilt(r_host.self(), -15.0f, 15.0f, kHudPerspectiveFov);
			}
		}
		const std::string *w = p_init != nullptr ? p_init->property("m_width") : nullptr;
		const std::string *h = p_init != nullptr ? p_init->property("m_height") : nullptr;
		const double wn = w != nullptr ? As2Value::of(*w).to_number() : std::nan("");
		const double hn = h != nullptr ? As2Value::of(*h).to_number() : std::nan("");
		width_ = std::isnan(wn) ? start_w_ : wn;
		height_ = std::isnan(hn) ? start_h_ : hn;
		const As2Value *hide = p_init != nullptr ? p_init->value("HideForHardcore") : nullptr;
		hardcore_ = hide != nullptr ? hide->truthy() : false;
		if (hardcore_) {
			height_ = start_h_ - kMinimapSize;
			r_host.self().set_y(r_host.self().y() + float(kMinimapSize));
		}
		r_host.self().set_xscale(100.0f);
		r_host.self().set_yscale(100.0f);
		set_width(width_);
		set_height(height_);
		if (p_init != nullptr) {
			setup_refresh_data(*this, r_host, p_init->data);
		}
		r_host.self().set_visible(true);
	}

	static constexpr double kAirRadarHeight = 80.0;
	static constexpr double kMinimapSize = 170.0;
	HudWidgetHost *host_ = nullptr;
	double start_w_ = 0.0;
	double start_h_ = 0.0;
	double start_y_ = 0.0;
	double width_ = 0.0;
	double height_ = 0.0;
	bool hardcore_ = false;
	std::string tilt_;
};

}

void register_hud_widgets_hud_background(HudWidgetRegistry &r_registry) {
	r_registry.add("Shared.HudBackground", [] { return std::make_unique<HudBackgroundClass>(); });
	r_registry.add("Widget.Hud.HudBackgroundWidget", [] { return std::make_unique<HudBackgroundWidgetClass>(); });
}

}
