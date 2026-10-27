#pragma once

#include <string_view>

namespace bf3::godot_side {

class Bf3UiScreens;

inline constexpr std::string_view kTabListenerScreenPartition = "ui/flow/screen/tablistenerscreen";
inline constexpr std::string_view kTabListenerWidget = "instance1.InputListener_01";

class OverlayScreens {
public:
	static void draw(Bf3UiScreens &r_screens);
	static void frame(Bf3UiScreens &r_screens);
};

}
