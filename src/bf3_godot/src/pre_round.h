#pragma once

#include "hud_tdm_screen.h"

namespace bf3::godot_side {

inline constexpr float kPreRoundCentreX = 640.0f;
inline constexpr float kPreRoundTop = 132.0f;
inline constexpr float kPreRoundWidth = 360.0f;
inline constexpr float kPreRoundHeight = 92.0f;

class Bf3TdmPreRound : public Bf3TdmScreen {
	GDCLASS(Bf3TdmPreRound, Bf3TdmScreen)

public:
	void update_screen() override;
	void draw_screen() override;

protected:
	static void _bind_methods() {}
};

}
