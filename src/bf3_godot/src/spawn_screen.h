#pragma once
// M2b ui: the spawn screen (docs/design/m2b.md §4: the spawnscreenpc flow; as built: docs/design/m2b-ui.md §3.6).
//
// While HudSpawnScreen::visible (at join, and after each own death once the kill card is gone): the world dimmed,
// "DEPLOY AS" over the kit bar at spawnscreenpc's KitBar_01 placement: four kit cards (the kit's iconsingame icon and
// name, the _small icons of weapon slots 0, 1, 2, 6, 7 of the own team's loadout; assault's slot 2 is the
// defibrillator with TDM kits), the selected one highlighted; the team counts at TicketCounterSpawnScreen_01; at the
// bottom right the Deploy button ("DEPLOY", or "TIME TO DEPLOY" and the seconds left while it waits, disabled) and,
// after a death while a medic can still reach the corpse, "REVIVE POSSIBLE" and its seconds. TDM spawns are random:
// there is no point choice.
//
// Input: the cards and Deploy are Godot Buttons (focus none, so Enter stays the game's key); a click is kept until
// Bf3TdmHud::take_kit_click / take_deploy_click takes it. The selection drawn is HudSpawnScreen::selected_kit: the game
// keeps it (keys and the clicks it takes) and feeds it back through HudFeedInput::spawn_kit.

#include "hud_tdm_screen.h"

#include <array>
#include <string>

namespace godot {
class Button;
} // namespace godot

namespace bf3::godot_side {

// Stage pixels: the kit cards relative to KitBar_01 (bottom-left anchor), Deploy at the bottom right [inferred].
inline constexpr float kSpawnCardWidth = 190.0f;
inline constexpr float kSpawnCardHeight = 140.0f;
inline constexpr float kSpawnCardGap = 10.0f;
inline constexpr float kSpawnDeployLeft = 914.0f;
inline constexpr float kSpawnDeployTop = 622.0f;
inline constexpr float kSpawnDeployWidth = 270.0f;
inline constexpr float kSpawnDeployHeight = 56.0f;

class Bf3TdmSpawnScreen : public Bf3TdmScreen {
	GDCLASS(Bf3TdmSpawnScreen, Bf3TdmScreen)

public:
	void build_screen() override;
	void update_screen() override;
	void draw_screen() override;

	int take_kit_click();
	bool take_deploy_click();

protected:
	static void _bind_methods() {}

private:
	void on_kit(int64_t p_kit);
	void on_deploy();
	void style_buttons();
	godot::Rect2 card_rect(size_t p_kit) const; // window pixels
	godot::Rect2 deploy_rect() const;

	std::array<godot::Button *, core::ui::kHudKitCount> kits_{};
	godot::Button *deploy_ = nullptr;
	bool styled_ = false;
	int32_t styled_size_ = 0;
	std::string deploy_text_;
	int pending_kit_ = -1;
	bool pending_deploy_ = false;
	bool was_visible_ = false;
};

} // namespace bf3::godot_side
