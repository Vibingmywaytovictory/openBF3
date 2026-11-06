#include "hud_widget_support.h"
#include "squad_data.h"

#include <optional>
#include <vector>

namespace bf3::core::ui {

namespace {

constexpr int32_t kNavigateUp = 0;
constexpr int32_t kNavigateDown = 1;
constexpr int32_t kNavigateLeft = 2;
constexpr int32_t kNavigateRight = 3;
constexpr int32_t kChanged = 4;
constexpr int32_t kFocus = 5;
constexpr int32_t kUnselectedFrame = 1;
constexpr int32_t kSelectedFrame = 2;
constexpr uint32_t kFocusedText = 11184810u;
constexpr float kEnabledAlpha = 100.0f;
constexpr float kDisabledAlpha = 50.0f;

class CheckBoxClass final : public HudWidgetController {
public:
	void attach_node(HudWidgetHost &, const HudWidgetNode &p_node) override { sources_ = p_node.sources; }

	void on_clip_load(HudWidgetHost &r_host) override {
		const As2Clip self = r_host.self();
		const As2Clip field = r_host.clip("textField");
		orig_color_ = field.text_color();
		self.set_xscale(100.0f);
		self.set_yscale(100.0f);
		field.set_auto_size(SwfTextAutoSize::Left);
		r_host.set_mouse_target(self, true);
		self.set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		if (const std::string *text = p_init.property("StaticText"); text != nullptr && !text->empty()) {
			update(r_host, "Text", As2Value::of(*text));
		}
		r_host.self().set_visible(true);
		setup_refresh_data(*this, r_host, p_init.data);
	}

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_out) const override {
		for (const HudDataSource &src : sources_) {
			if (std::optional<As2Value> v = hud_squad_source_value(src, p_model, p_client)) {
				r_out.push_back({ src.name, std::move(*v), src.on_initialize });
			}
		}
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Selected") {
			set_selected(r_host, p_value.truthy(), false);
			return;
		}
		if (!p_value.defined()) {
			return;
		}
		if (p_key == "Text") {
			r_host.clip("textField").set_text(r_host.translate(p_value.to_string()));
		} else if (p_key == "Visible") {
			r_host.self().set_visible(p_value.truthy());
		} else if (p_key == "Enabled") {
			enabled_ = p_value.truthy();
			r_host.self().set_alpha(enabled_ ? kEnabledAlpha : kDisabledAlpha);
		}
	}

	bool mouse_release(HudWidgetHost &r_host, const HudMouse &p_mouse) override {
		if (p_mouse.target != r_host.path() || p_mouse.kind == HudMouseKind::ReleaseOutside) {
			return false;
		}
		toggle(r_host);
		return true;
	}

	bool input_concept(HudWidgetHost &r_host, int32_t p_concept, bool p_pressed) override {
		if (!p_pressed) {
			if (UiInputConcept(p_concept) != UiInputConcept::Activate) {
				return false;
			}
			toggle(r_host);
			return true;
		}
		switch (UiInputConcept(p_concept)) {
			case UiInputConcept::NavigateUp:
				r_host.fire_event(kNavigateUp, As2Value::of(0));
				return true;
			case UiInputConcept::NavigateDown:
				r_host.fire_event(kNavigateDown, As2Value::of(0));
				return true;
			case UiInputConcept::NavigateLeft:
				r_host.fire_event(kNavigateLeft, As2Value::of(0));
				return true;
			case UiInputConcept::NavigateRight:
				r_host.fire_event(kNavigateRight, As2Value::of(0));
				return true;
			default:
				return false;
		}
	}

	void in_event(HudWidgetHost &r_host, int32_t p_index, const As2Value &) override {
		if (p_index == kFocus) {
			r_host.request_focus();
		}
	}

	void focus_gained(HudWidgetHost &r_host) override { r_host.clip("textField").set_text_color(kFocusedText); }

	void focus_lost(HudWidgetHost &r_host) override {
		if (orig_color_) {
			r_host.clip("textField").set_text_color(*orig_color_);
		}
	}

private:
	void toggle(HudWidgetHost &r_host) {
		if (enabled_) {
			set_selected(r_host, !selected_, true);
		}
	}

	void set_selected(HudWidgetHost &r_host, bool p_selected, bool p_send_event) {
		selected_ = p_selected;
		if (p_send_event) {
			r_host.fire_query(kSoundSelect, As2Value::of(false));
			r_host.fire_event(kChanged, As2Value::of(selected_));
		}
		r_host.clip("box").goto_and_stop(selected_ ? kSelectedFrame : kUnselectedFrame);
	}

	std::vector<HudDataSource> sources_;
	std::optional<uint32_t> orig_color_;
	bool selected_ = false;
	bool enabled_ = true;
};

}

void register_hud_widgets_check_box(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.CheckBox.CheckBox", [] { return std::make_unique<CheckBoxClass>(); });
}

}
