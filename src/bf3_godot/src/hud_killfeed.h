#pragma once
// M2b ui: the kill feed and the team kill counter (docs/design/m2b.md §4; as built: docs/design/m2b-ui.md §3.1).
//
// Kill feed: HudMessages::kill_message, the 8-line ring the feed fills from Death events, newest on top, right-aligned
// under hudmpscreen's top-right message widget (HudMessage_02): killer, the weapon's _small icon (64 x 16), a headshot
// mark, victim. Names in the team colours (the own player yellow); a line with the own player as killer or victim
// sits on a tinted panel (LocalKillMessage). A line is shown for host::kKillFeedSeconds after this screen first drew
// its serial (HudKill carries no time), the last kKillFeedFadeSeconds fading out.
// Kill counter: while a kill limit is known and no full-window screen is up, "US 12 / 100 / 10 RU" at the top centre
// (UIKillCounterCompData Team1Kills / Team2Kills / Team1MaxKills), the friendly team on the left.
// No headshot icon was found in the atlases the HUD reads (iconsingame, hudtextures, killscreen; bf3dump swf labels,
// 2026-10-19), so the mark is drawn (a ring with a dot) [inferred].

#include "hud_tdm_screen.h"

#include <array>
#include <cstdint>

namespace bf3::godot_side {

// Stage pixels, relative to the placement's anchor point [inferred].
inline constexpr float kKillFeedRightInset = 18.0f;
inline constexpr float kKillFeedTop = 40.0f;
inline constexpr float kKillFeedLineHeight = 22.0f;
inline constexpr float kKillFeedNameSize = 14.0f;
inline constexpr float kKillFeedIconWidth = 64.0f;
inline constexpr float kKillFeedIconHeight = 16.0f;
inline constexpr float kKillFeedGap = 6.0f;
inline constexpr double kKillFeedFadeSeconds = 0.5;
inline constexpr float kKillCounterTop = 22.0f; // the counter's baseline, top centre
inline constexpr float kKillCounterSize = 20.0f;

class Bf3TdmKillFeed : public Bf3TdmScreen {
	GDCLASS(Bf3TdmKillFeed, Bf3TdmScreen)

public:
	void update_screen() override;
	void draw_screen() override;

protected:
	static void _bind_methods() {}

private:
	void draw_counter();
	struct Seen {
		uint32_t serial = 0;
		double first_s = 0;
	};
	std::array<Seen, core::ui::kHudKillFeedSize> seen_{};
};

} // namespace bf3::godot_side
