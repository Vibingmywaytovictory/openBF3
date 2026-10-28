#include "hud_widget_support.h"

#include "bf3/core/ui/screen_adjust.h"

namespace bf3::core::ui {

namespace {

constexpr int32_t kConceptNavigateUp = 0;
constexpr int32_t kConceptNavigateDown = 1;
constexpr int32_t kConceptNavigateLeft = 2;
constexpr int32_t kConceptNavigateRight = 3;

class ScreenAdjustClass final : public HudWidgetController {
public:
	void attach_node(HudWidgetHost &, const HudWidgetNode &p_node) override { sources_ = p_node.sources; }

	void bind(const HudModel &, const HudClientState &p_client, HudData &r_out) const override {
		hud_flow_sources(sources_, p_client, r_out);
	}

	void on_clip_load(HudWidgetHost &r_host) override {
		update_fields(r_host);
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Width") {
			fractions_.width = p_value.to_number();
		} else if (p_key == "Height") {
			fractions_.height = p_value.to_number();
		} else {
			return;
		}
		update_fields(r_host);
	}

	void update_screen_size(HudWidgetHost &r_host) override { update_fields(r_host); }

	bool input_concept(HudWidgetHost &r_host, int32_t p_concept, bool p_pressed) override {
		if (!p_pressed) {
			return false;
		}
		switch (p_concept) {
			case kConceptNavigateUp:
			case kConceptNavigateDown:
				fractions_.height = screen_adjust_step(fractions_.height, p_concept == kConceptNavigateUp);
				r_host.fire_event(kScreenHeightChanged, As2Value::of(fractions_.height));
				break;
			case kConceptNavigateLeft:
			case kConceptNavigateRight:
				fractions_.width = screen_adjust_step(fractions_.width, p_concept == kConceptNavigateRight);
				r_host.fire_event(kScreenWidthChanged, As2Value::of(fractions_.width));
				break;
			default:
				return false;
		}
		update_fields(r_host);
		return true;
	}

private:
	void update_fields(HudWidgetHost &r_host) const {
		const SwfRect &v = r_host.screen().stage_rects().visible;
		const ScreenAdjustFields f = screen_adjust_fields(v.width(), v.height(), fractions_);
		const auto place = [&](std::string_view p_field, const ScreenAdjustField &p_at) {
			const As2Clip field = r_host.clip(p_field);
			field.set_x(v.x_min + float(p_at.x));
			field.set_y(v.y_min + float(p_at.y));
			const As2Clip red = r_host.clip(std::string(p_field) + ".redField");
			red.set_height(float(p_at.red_height));
			red.set_width(float(p_at.red_width));
		};
		place("field_left", f.left);
		place("field_top", f.top);
		place("field_right", f.right);
		place("field_bottom", f.bottom);
	}

	std::vector<HudDataSource> sources_;
	ScreenAdjustFractions fractions_;
};

}

void register_hud_widgets_screen_adjust(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.ScreenSpecific.ScreenAdjust", [] { return std::make_unique<ScreenAdjustClass>(); });
}

}
