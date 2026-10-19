#pragma once
// M2b ui: the round end (docs/design/m2b.md §4: EndOfRound; as built: docs/design/m2b-ui.md §3.5).
//
// While HudEndOfRound::visible (MatchState RoundOver, until the restart): the world dimmed and a centred panel:
// "YOUR TEAM WON!" / "YOUR TEAM LOST!" (or "WINNER" and the team's name while the own team is unknown), the two team
// counts with "WINNER" over the winner's, the kill limit, the top 3 players by score (name and score) and
// "NEXT ROUND STARTS IN:" with the seconds left (rounded up) once the restart is scheduled.

#include "hud_tdm_screen.h"

namespace bf3::godot_side {

inline constexpr float kRoundEndLeft = 340.0f;
inline constexpr float kRoundEndTop = 150.0f;
inline constexpr float kRoundEndWidth = 600.0f;
inline constexpr float kRoundEndHeight = 400.0f;

class Bf3TdmRoundEnd : public Bf3TdmScreen {
	GDCLASS(Bf3TdmRoundEnd, Bf3TdmScreen)

public:
	void update_screen() override;
	void draw_screen() override;

protected:
	static void _bind_methods() {}

private:
	bool was_visible_ = false;
};

} // namespace bf3::godot_side
