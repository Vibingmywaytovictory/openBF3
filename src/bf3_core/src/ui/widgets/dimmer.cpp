#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

constexpr int32_t kFadeIn = 0;
constexpr int32_t kFadeOut = 1;
constexpr float kOverscanWidth = 256.0f;
constexpr float kOverscanHeight = 144.0f;

class DimmerClass final : public HudWidgetController {
public:
	void attach_node(HudWidgetHost &, const HudWidgetNode &p_node) override { sources_ = p_node.sources; }

	void on_clip_load(HudWidgetHost &r_host) override {
		r_host.self().set_visible(false);
		r_host.set_mouse_target(r_host.self(), true);
	}

	void bind(const HudModel &, const HudClientState &p_client, HudData &r_out) const override {
		if (p_client.popup == nullptr) {
			return;
		}
		for (const HudDataSource &src : sources_) {
			if (src.name == "Visible") {
				r_out.push_back({ src.name, As2Value::of(p_client.popup->show_dimmer), src.on_initialize });
			}
		}
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		resize_to_screen(r_host);
		r_host.self().set_visible(true);
		setup_refresh_data(*this, r_host, p_init.data);
		const std::string *from_start = p_init.property("DimFromStart");
		const std::string *instant = p_init.property("InstantDim");
		dim_from_start_ = from_start != nullptr && *from_start == "true";
		instant_dim_ = instant != nullptr && *instant == "true";
		if (dim_from_start_) {
			if (instant_dim_) {
				r_host.clip("dimmer").set_visible(true);
				r_host.clip("dimmer").goto_and_stop("idle");
			} else {
				fade_in(r_host);
			}
		}
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Visible" && p_value.defined()) {
			r_host.self().set_visible(p_value.truthy());
		}
	}

	bool mouse_move(HudWidgetHost &r_host, const HudMouse &p_mouse) override { return p_mouse.target == r_host.path(); }

	void in_event(HudWidgetHost &r_host, int32_t p_index, const As2Value &) override {
		if (p_index == kFadeIn) {
			fade_in(r_host);
		} else if (p_index == kFadeOut) {
			fade_out(r_host);
		}
	}

private:
	static void resize_to_screen(HudWidgetHost &r_host) {
		const SwfRect &visible = r_host.screen().stage_rects().visible;
		r_host.self().set_width(visible.width() + kOverscanWidth);
		r_host.self().set_height(visible.height() + kOverscanHeight);
	}

	void fade_in(HudWidgetHost &r_host) const {
		const As2Clip dimmer = r_host.clip("dimmer");
		if (instant_dim_) {
			dimmer.set_visible(true);
			dimmer.goto_and_stop("idle");
		} else {
			dimmer.goto_and_play("fadeIn");
		}
	}

	void fade_out(HudWidgetHost &r_host) const {
		const As2Clip dimmer = r_host.clip("dimmer");
		if (instant_dim_) {
			dimmer.set_visible(false);
			dimmer.goto_and_stop(1);
		} else {
			dimmer.goto_and_play("fadeOut");
		}
	}

	std::vector<HudDataSource> sources_;
	bool dim_from_start_ = false;
	bool instant_dim_ = false;
};

}

void register_hud_widgets_dimmer(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Image.Dimmer", [] { return std::make_unique<DimmerClass>(); });
}

}
