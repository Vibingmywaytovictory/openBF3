#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

class ScreenClass final : public HudWidgetController {
public:
	bool screen_initialized() const override { return false; }
};

}

void register_hud_widgets_screen(HudWidgetRegistry &r_registry) {
	r_registry.add("fb.Base.UIScreen", [] { return std::make_unique<ScreenClass>(); });
	r_registry.add("Screen.Weapon.CrosshairDefault", [] { return std::make_unique<ScreenClass>(); });
}

}
