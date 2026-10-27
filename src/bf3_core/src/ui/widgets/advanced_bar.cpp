#include "deploy_widgets.h"

namespace bf3::core::ui {

namespace {

class AdvancedBarClass final : public HudWidgetController, public HudAdvancedBar {
public:
	void construct(HudWidgetHost &r_host) override {
		host_ = &r_host;
		set_start_size(r_host);
		add_bar_holder(r_host);
		r_host.self().set_visible(false);
	}

	void initialize_bars(const std::vector<HudBarSpec> &p_bars) override {
		data_.clear();
		for (const HudBarSpec &b : p_bars) {
			add_bar(b);
		}
		host_->self().set_visible(true);
	}

	void set_value_on_index(size_t p_index, const As2Value &p_value) override {
		if (p_index >= data_.size()) {
			return;
		}
		host_->clip("loader").set_visible(p_value.defined() && p_value.to_number() == -1.0);
		const std::string name = tween_name(p_index);
		host_->remove_custom_tween(name);
		if (!p_value.defined()) {
			bar(p_index).set_width(0.0f);
			return;
		}
		const double v = p_value.to_number();
		if (data_[p_index].animate) {
			host_->add_custom_tween(name, float(v), kAnimSeconds, HudTweenEase::EaseInOutSine);
		} else {
			data_[p_index].value = v;
			tween_update(p_index);
			if (use_glow_) {
				glow().set_width(bar_holder().width());
			}
			if (glow().exists()) {
				glow().set_visible(v != 0.0);
			}
		}
	}

	std::optional<float> tween_property(HudWidgetHost &, std::string_view p_name) override {
		if (const std::optional<size_t> i = index_of(p_name)) {
			return float(data_[*i].value);
		}
		return std::nullopt;
	}

	void set_tween_property(HudWidgetHost &, std::string_view p_name, float p_value) override {
		if (const std::optional<size_t> i = index_of(p_name)) {
			data_[*i].value = p_value;
			tween_update(*i);
			if (use_glow_) {
				glow().set_width(bar_holder().width());
			}
		}
	}

	void set_use_glow(bool p_use) override {
		use_glow_ = p_use;
		if (p_use) {
			if (!glow().exists()) {
				add_glow();
			}
			if (glow_color_ != kWhite) {
				set_glow_color(glow_color_);
			}
		} else if (glow().exists()) {
			host_->remove_movie_clip(glow());
		}
	}

	void set_glow_color(uint32_t p_rgb) override {
		glow_color_ = p_rgb;
		if (use_glow_) {
			glow().set_rgb(glow_color_);
		}
	}

	size_t bar_count() const override { return data_.size(); }
	double value(size_t p_index) const override { return p_index < data_.size() ? data_[p_index].value : 0.0; }
	float bar_width(size_t p_index) const override { return p_index < data_.size() ? bar(p_index).width() : 0.0f; }
	float start_width() const override { return start_w_; }

private:
	static constexpr uint32_t kWhite = 0xFFFFFFu;
	static constexpr double kAnimSeconds = 1.0;
	static constexpr std::string_view kTweenPrefix = "barValue_";

	void set_start_size(HudWidgetHost &r_host) {
		const As2Clip self = r_host.self();
		start_w_ = float(as2_round(self.width()));
		start_h_ = float(as2_round(self.height()));
		self.set_x(float(as2_round(self.x())));
		self.set_y(float(as2_round(self.y())));
		self.set_xscale(100.0f);
		self.set_yscale(100.0f);
		const As2Clip bg = r_host.clip("bg");
		bg.set_width(0.0f);
		bg.set_height(0.0f);
		bg.set_visible(false);
	}

	void add_bar_holder(HudWidgetHost &r_host) {
		const As2Clip self = r_host.self();
		r_host.create_empty_movie_clip(self, "barHolder", self.next_highest_depth());
		r_host.attach_movie(self, "loader", "loader", self.next_highest_depth());
	}

	void add_glow() {
		const As2Clip self = host_->self();
		const As2Clip g = host_->attach_movie(self, "mc_glow_4px", "glow_mc", self.next_highest_depth());
		g.set_height(start_h_);
		g.set_width(bar_holder().width());
		g.set_alpha(glow_alpha_);
		g.set_visible(bar_holder().width() != 0.0f);
	}

	void add_bar(const HudBarSpec &p_spec) {
		const size_t n = data_.size();
		data_.push_back(p_spec);
		const As2Clip b = host_->attach_movie(bar_holder(), "mc_bar", "bar_" + std::to_string(n), int32_t(n));
		b.set_height(start_h_);
		b.set_width(0.0f);
		data_[n].value = 0.0;
		b.set_visible(p_spec.visible);
		b.set_alpha(p_spec.alpha);
		b.set_rgb(p_spec.color);
		set_value_on_index(n, As2Value::of(p_spec.value));
	}

	void tween_update(size_t p_index) const {
		const HudBarSpec &d = data_[p_index];
		const double f = std::min(d.value / d.target_value, 1.0);
		bar(p_index).set_width(float(as2_round(double(start_w_) * f)));
	}

	std::optional<size_t> index_of(std::string_view p_name) const {
		if (!p_name.starts_with(kTweenPrefix)) {
			return std::nullopt;
		}
		const size_t i = size_t(std::atoi(std::string(p_name.substr(kTweenPrefix.size())).c_str()));
		return i < data_.size() ? std::optional<size_t>(i) : std::nullopt;
	}

	static std::string tween_name(size_t p_index) { return std::string(kTweenPrefix) + std::to_string(p_index); }
	As2Clip bar_holder() const { return host_->clip("barHolder"); }
	As2Clip bar(size_t p_index) const { return host_->clip("barHolder.bar_" + std::to_string(p_index)); }
	As2Clip glow() const { return host_->clip("glow_mc"); }

	HudWidgetHost *host_ = nullptr;
	std::vector<HudBarSpec> data_;
	float start_w_ = 0.0f;
	float start_h_ = 0.0f;
	bool use_glow_ = false;
	uint32_t glow_color_ = kWhite;
	float glow_alpha_ = 100.0f;
};

}

void register_hud_widgets_advanced_bar(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Bar.AdvancedBar", [] { return std::make_unique<AdvancedBarClass>(); });
}

}
