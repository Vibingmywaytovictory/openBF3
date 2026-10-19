#pragma once
// M2b ui: the scoreboard (docs/design/m2b.md §4: scoreboardtwoteamshud; as built: docs/design/m2b-ui.md §3.2).
//
// While HudTeam::scoreboard_enabled (Tab held): a centred panel with two columns, the friendly team on the left
// (HudTeam::friendly_team_players) and the enemy on the right (enemy_team1_players), each list already sorted by score,
// then kills, then slot (host::fill_hud_tdm). The header holds each team's name and kill count with the kill limit
// (HudKillCounter); a row holds the name, K, D, score and ping (the column SIDs of hud_tdm_text.h), bots marked "BOT",
// the dead with the iconsingame "dead" icon, the own row highlighted. Rows shrink to fit 32 per team.

#include "hud_tdm_screen.h"

namespace bf3::godot_side {

// The panel, stage pixels (centred on the 1280 x 720 stage) [inferred].
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
	// One team column; returns the rows drawn.
	uint8_t draw_team(const core::ui::HudPlayerList &p_list, int p_team, int32_t p_kills, float p_left, float p_width, bool p_friendly);
};

} // namespace bf3::godot_side
