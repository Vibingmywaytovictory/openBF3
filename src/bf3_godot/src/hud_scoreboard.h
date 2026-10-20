#pragma once

#include "hud_tdm_screen.h"

namespace bf3::godot_side {

inline constexpr float kScoreboardLeft = 120.0f;
inline constexpr float kScoreboardTop = 70.0f;
inline constexpr float kScoreboardWidth = 1040.0f;
inline constexpr float kScoreboardHeight = 580.0f;
inline constexpr float kScoreboardHeaderHeight = 64.0f;
inline constexpr float kScoreboardRowMax = 26.0f;
inline constexpr float kScoreboardRowMin = 14.0f;

class Bf3TdmScoreboard : public Bf3TdmScreen {
	GDCLASS(Bf3TdmScoreboard, Bf3TdmScreen)

public:
	void update_screen() override;
	void draw_screen() override;

protected:
	static void _bind_methods() {}

private:

	uint8_t draw_team(const core::ui::HudPlayerList &p_list, int p_team, int32_t p_kills, float p_left, float p_width, bool p_friendly);
};

}
