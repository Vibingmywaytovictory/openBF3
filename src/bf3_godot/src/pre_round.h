#pragma once
// M2b ui: the pre-round (docs/design/m2b.md §4: PreRound; as built: docs/design/m2b-ui.md §3.4).
//
// While HudPreRound::visible (MatchState PreRound): a box at the top centre where hudmpscreen places its alert
// messages (HudAlertMessage at 640, 156): "ROUND WILL START IN" over the seconds left (rounded up) while the countdown
// runs, else "WAITING FOR PLAYERS". The freeze is shown by the box's edge: orange while input_restricted (the
// MatchState restriction holds the own soldier), the panel line colour once it lifts [inferred].

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

} // namespace bf3::godot_side
