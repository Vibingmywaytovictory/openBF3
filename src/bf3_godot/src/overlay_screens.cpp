#include "overlay_screens.h"

#include "native_screens.h"

#include "bf3/core/ui/hud_screen.h"

namespace bf3::godot_side {

void OverlayScreens::draw(Bf3UiScreens &r_screens) {
	r_screens.set_drawn(bf3::core::ui::kScoreboardHud32ScreenPartition, true);
	r_screens.set_drawn(bf3::core::ui::kPreRoundWaitingScreenPartition, true);
}
}
