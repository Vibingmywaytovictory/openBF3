#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

constexpr int32_t kPopupIconShown = 2;

class GlowRectClass final : public HudWidgetController {};

class PopupBoxClass final : public HudWidgetController {
public:
	void construct(HudWidgetHost &r_host) override {
		const As2Clip self = r_host.self();
		const float width = self.width();
		const float height = self.height();
		self.set_xscale(100.0f);
		self.set_yscale(100.0f);
		const As2Clip icon = r_host.clip("icon");
		const As2Clip header = r_host.clip("header");
		const As2Clip bg = r_host.clip("bg");
		const float icon_offset = self.width() - icon.x();
		bg.set_width(width);
		header.set_width(width);
		bg.set_height(height - bg.y());
		icon.set_x(width - icon_offset);
		icon.goto_and_stop(kPopupIconShown);
	}
};

}

void register_hud_widgets_graphics(HudWidgetRegistry &r_registry) {
	r_registry.add("Graphics.GlowRect", [] { return std::make_unique<GlowRectClass>(); });
	r_registry.add("Graphics.PopupBox", [] { return std::make_unique<PopupBoxClass>(); });
}

}
