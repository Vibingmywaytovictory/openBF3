#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

class GlowRectClass final : public HudWidgetController {};

}

void register_hud_widgets_glow_rect(HudWidgetRegistry &r_registry) {
	r_registry.add("Graphics.GlowRect", [] { return std::make_unique<GlowRectClass>(); });
}

}
