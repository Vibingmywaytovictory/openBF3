#include "hud_widget_support.h"
#include "scroll_text.h"

namespace bf3::core::ui {

namespace {

constexpr int32_t kMenuHintText = 0;
constexpr int32_t kFrameStartFadeIn = 15;
constexpr int32_t kFrameStartText = 26;
constexpr double kMenuHintQuickUpdateSeconds = 0.7;

class MenuHintClass final : public HudWidgetController {
public:
	void attach_node(HudWidgetHost &, const HudWidgetNode &p_node) override { sources_ = p_node.sources; }

	void bind(const HudModel &, const HudClientState &p_client, HudData &r_out) const override {
		hud_flow_sources(sources_, p_client, r_out);
	}

	void on_clip_load(HudWidgetHost &r_host) override {
		start_width_ = r_host.clip("hint.bg").width();
		set_text(r_host, As2Value::of(""));
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Text") {
			set_text(r_host, p_value);
		} else if (p_key == "Visible" && p_value.defined()) {
			r_host.self().set_visible(p_value.truthy());
		}
	}

	void in_event(HudWidgetHost &r_host, int32_t p_index, const As2Value &p_param) override {
		if (p_index == kMenuHintText) {
			set_text(r_host, p_param);
		}
	}

	void advance(HudWidgetHost &, double p_seconds) override { clock_ += p_seconds; }

private:
	void set_text(HudWidgetHost &r_host, const As2Value &p_data) {
		const As2Clip hint = r_host.clip("hint");
		const As2Clip bg = r_host.clip("hint.bg");
		ScrollTextWidget *field = controller_at<ScrollTextWidget>(r_host, r_host.clip("hint.text"));
		if (!p_data.defined() || p_data.to_string().empty()) {
			if (field != nullptr) {
				field->set_text("");
			}
			hint.goto_and_stop(1);
			bg.goto_and_stop(1);
			return;
		}
		if (field == nullptr) {
			return;
		}
		field->set_text(r_host.translate(p_data.to_string()));
		bg.set_width(std::max(field->text_width() + 100.0f, start_width_));
		if (last_update_ && clock_ - *last_update_ < kMenuHintQuickUpdateSeconds) {
			hint.goto_and_play(kFrameStartText);
			bg.goto_and_play(kFrameStartText);
		} else if (first_run_) {
			hint.goto_and_play(1);
			bg.goto_and_play(1);
			first_run_ = false;
		} else {
			hint.goto_and_play(kFrameStartFadeIn);
			bg.goto_and_play(kFrameStartFadeIn);
		}
		last_update_ = clock_;
	}

	std::vector<HudDataSource> sources_;
	float start_width_ = 0.0f;
	bool first_run_ = true;
	double clock_ = 0.0;
	std::optional<double> last_update_;
};

}

void register_hud_widgets_menu_hint(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Text.MenuHint", [] { return std::make_unique<MenuHintClass>(); });
}

}
