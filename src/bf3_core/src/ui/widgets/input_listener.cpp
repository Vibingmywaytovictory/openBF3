#include "hud_widget_support.h"
#include "scroll_text.h"

namespace bf3::core::ui {

namespace {

constexpr int32_t kTabPressed = 1;
constexpr int32_t kTabRelease = 2;
constexpr int32_t kComPressed = 3;
constexpr int32_t kComRelease = 4;
constexpr double kTabDelay = 0.3;
constexpr double kComDelay = 0.2;

class InputListenerClass final : public HudWidgetController {
public:
	void initialize(HudWidgetHost &r_host, const HudWidgetInit &) override {
		win32_ = hud_platform_win32(r_host.client());
		r_host.self().set_visible(false);
	}

	bool input_concept(HudWidgetHost &r_host, int32_t p_concept, bool p_pressed) override {
		const UiInputConcept input = UiInputConcept(p_concept);
		if (p_pressed) {
			if (input == UiInputConcept::Tab) {
				if (win32_) {
					r_host.fire_event(kTabPressed, As2Value::of(0));
				} else if (tab_.handle() != As2Interval::Handle::Running) {
					tab_.set(kTabDelay);
				}
			} else if (input == UiInputConcept::Com && win32_) {
				com_.set(kComDelay);
			}
			return false;
		}
		if (input == UiInputConcept::Tab) {
			tab_.clear();
			r_host.fire_event(kTabRelease, As2Value::of(0));
		} else if (input == UiInputConcept::Com && win32_) {
			com_.clear();
			r_host.fire_event(kComRelease, As2Value::of(0));
		}
		return false;
	}

	void advance(HudWidgetHost &r_host, double p_seconds) override {
		if (tab_.due(p_seconds) > 0) {
			tab_.clear();
			r_host.fire_event(kTabPressed, As2Value::of(0));
		}
		if (com_.due(p_seconds) > 0) {
			com_.clear();
			r_host.fire_event(kComPressed, As2Value::of(0));
		}
	}

private:
	As2Interval tab_;
	As2Interval com_;
	bool win32_ = true;
};

}

void register_hud_widgets_input_listener(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Input.InputListener", [] { return std::make_unique<InputListenerClass>(); });
}

}
