#include "overlay_screens.h"

#include "native_screens.h"

#include "bf3/core/ui/hud_screen.h"

namespace bf3::godot_side {

void OverlayScreens::draw(Bf3UiScreens &r_screens) {
	r_screens.set_drawn(bf3::core::ui::kScoreboardHud32ScreenPartition, true);
	r_screens.set_drawn(bf3::core::ui::kPreRoundWaitingScreenPartition, true);
}

void OverlayScreens::frame(Bf3UiScreens &r_screens) {
	bf3::core::ui::HudScreen *listener = r_screens.hud_screen(kTabListenerScreenPartition);
	if (listener != nullptr && listener->initialized() && listener->focused_widget().empty()) {
		listener->focus_widget(kTabListenerWidget);
	}
}

}
