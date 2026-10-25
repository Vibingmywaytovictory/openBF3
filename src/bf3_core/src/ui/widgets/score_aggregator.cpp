#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

class ScoreAggregatorClass final : public HudWidgetController {
public:
	void attach_node(HudWidgetHost &, const HudWidgetNode &p_node) override {
		if (const HudDataSource *src = p_node.source(kValue)) {
			on_initialize_ = src->on_initialize;
		}
	}

	void construct(HudWidgetHost &) override {
		ready_ = true;
		prev_value_ = -1.0;
	}

	void on_clip_load(HudWidgetHost &r_host) override { r_host.self().set_visible(false); }

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
	}

	void frame_script(HudWidgetHost &r_host, const HudFrameScript &p_fs) override {
		if (p_fs.path == r_host.path() + ".aggHolder" && p_fs.calls("ready")) {
			ready_ = true;
		}
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key != kValue || !p_value.defined()) {
			return;
		}
		const As2Clip holder = r_host.clip("aggHolder");
		const double v = p_value.to_number();
		if (v == 0.0) {
			holder.goto_and_play("out");
			prev_value_ = 0.0;
			return;
		}
		if (ready_ && prev_value_ > 0.0) {
			ready_ = false;
			holder.goto_and_play("start");
		}
		prev_value_ = v;
		const std::string text = p_value.to_string();
		r_host.clip("aggHolder.scoringMessage.txt").set_text(text);
		r_host.clip("aggHolder.scoringMessage.txtBg").set_text(text);
	}

	void bind(const HudModel &, const HudClientState &p_client, HudData &r_out) const override {
		if (p_client.messages != nullptr) {
			r_out.push_back({ std::string(kValue), p_client.messages->score_aggregate(), on_initialize_ });
		}
	}

private:
	static constexpr std::string_view kValue = "ScoreAgregateValue";
	bool on_initialize_ = true;
	bool ready_ = true;
	double prev_value_ = -1.0;
};

}

void register_hud_widgets_score_aggregator(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Message.ScoreAggregator", [] { return std::make_unique<ScoreAggregatorClass>(); });
}

}
