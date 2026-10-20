#pragma once

#include "hud_tdm_screen.h"

namespace bf3::godot_side {

inline constexpr float kKillCardLeft = 430.0f;
inline constexpr float kKillCardTop = 470.0f;
inline constexpr float kKillCardWidth = 420.0f;
inline constexpr float kKillCardHeight = 150.0f;

class Bf3TdmKillCard : public Bf3TdmScreen {
	GDCLASS(Bf3TdmKillCard, Bf3TdmScreen)

public:
	void update_screen() override;
	void draw_screen() override;

protected:
	static void _bind_methods() {}
};

}
