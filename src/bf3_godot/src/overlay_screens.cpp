#include "overlay_screens.h"

#include "native_screens.h"

#include "bf3/core/ui/ui_drawn_screens.h"

namespace bf3::godot_side {

void OverlayScreens::draw(Bf3UiScreens &r_screens) {
	for (const std::string_view p : bf3::core::ui::kOverlayDrawnPartitions) {
		r_screens.set_drawn(p, true);
	}
}
}
