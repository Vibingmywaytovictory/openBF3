#pragma once
// M2b ui: the kill card (docs/design/m2b.md §4: UIKillCardCompData, the killscreen flow; as built: docs/design/
// m2b-ui.md §3.3).
//
// While HudKillCard::visible (host::kKillCardSeconds after the own death, with cam's death camera): a panel at the
// bottom centre, where killscreen places its messages (HudMessage_01 at 640, 514): "KILLED BY" and the killer's name in
// the enemy colour (or "YOU COMMITTED SUICIDE"), the killer's kit icon and name, the weapon's _small icon at twice its
// size and its localised name, a headshot line, "HEALTH" with the killer's health left and the distance in metres.

#include "hud_tdm_screen.h"

namespace bf3::godot_side {

// The panel, stage pixels (bottom-anchored) [inferred].
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

} // namespace bf3::godot_side
