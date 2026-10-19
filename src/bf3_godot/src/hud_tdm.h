#pragma once
// Bf3TdmHud: the M2b HUD screens (docs/design/m2b.md §4, §2.6 #16; owner ui; W2.0 contract, docs/design/m2b-w2.md).
// A plain class owned by the M2b game (gx), like Bf3GameCrowd: it builds its Godot Controls under a parent node and
// draws core::ui::HudModel's M2b parts (host::fill_hud_tdm fills them) and nothing else; it never reads the sim. The
// screens are ui's (src/bf3_godot/src/hud_killfeed, hud_scoreboard, spawn_screen, kill_card, round_end, pre_round
// .{h,cpp}, each a Control; ui registers its classes in its own register_types.cpp block):
//
//   kill feed     HudMessages::kill_message (8 lines): killer name, the _small weapon icon, a headshot icon, victim;
//                 own kills (local_kill_message) highlighted; a line fades host::kKillFeedSeconds after it first drew
//   scoreboard    while HudTeam::scoreboard_enabled (Tab held): two columns (scoreboardtwoteamshud), rows sorted by
//                 score, bots marked, the team counts and the kill limit from HudKillCounter
//   spawn screen  HudSpawnScreen: the kit bar (4 kits, kit_items), Deploy (enabled while can_deploy), the timer,
//                 "revive possible"; a kit or Deploy clicked is taken by the game (take_kit_click, take_deploy_click)
//   kill card     HudKillCard while visible: killer name, kit, weapon icon and name, distance, the killer's health
//   pre-round     HudPreRound: "round starts in N", the freeze shown
//   round end     HudEndOfRound: winner, team scores, top 3, "next round in N"
//
// Drawn with ui1's assets: fonts through Bf3UiAssets (get_font("$HUD_medium")), icons get_region("ui/assets/iconsingame",
// ...), kill-feed weapon icons from ItemIconIndex's _small DxTextures, strings through Bf3Localisation (the SIDs of
// ui-swf-fonts.md §12.3). Every lookup that fails is counted in TdmHudRecords (E9 gates them at 0) and drawn with a
// fallback, never an error that stops the frame.
//
// Godot-free header (forward declarations only), so m2b.contract2 compiles it; the body (hud_tdm.cpp) is ui's. gx
// builds it only when the body is in the tree (OPENBF3_M2B_HAVE_UI, the "# M2b W2.0" block of bf3_godot's CMake).

#include "bf3/core/ui/hud_model.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>

namespace godot {
class Node;
} // namespace godot

namespace bf3::godot_side {

class Bf3Localisation;
class Bf3UiAssets;

// What the screens drew last (main thread), for the M2b check (E9) and the screenshots.
struct TdmHudRecords {
	uint64_t model_serial = 0; // HudModel::serial of the model drawn last
	uint64_t draws = 0;
	// kill feed
	uint8_t kill_feed_lines = 0; // lines drawn (not yet faded)
	uint32_t kill_feed_newest_serial = 0; // the newest kill_message serial drawn
	uint64_t kill_feed_lines_started = 0; // distinct serials ever drawn (E9: = the deaths received)
	// scoreboard
	bool scoreboard_visible = false;
	uint8_t scoreboard_rows = 0;
	std::array<int32_t, 2> scoreboard_team_kills{}; // the counts drawn at the top (E9: = MatchState::kills)
	// spawn screen
	bool spawn_screen_visible = false;
	bool deploy_enabled = false;
	uint8_t selected_kit = 0;
	uint64_t spawn_screen_shown = 0; // times it became visible (E9: >= 1 at join and after each own death)
	// kill card
	bool kill_card_visible = false;
	uint32_t kill_card_serial = 0;
	core::ui::HudPlayerName kill_card_killer{}; // the name drawn (UTF-8, NUL padded)
	uint32_t kill_card_item = 0; // the weapon icon's item id drawn (0: none)
	bool kill_card_weapon_named = false; // a localised weapon name was drawn
	// pre-round and round end
	bool pre_round_visible = false;
	bool round_end_visible = false;
	uint64_t round_end_shown = 0; // times it became visible
	// lookups (E9: 0 each)
	uint64_t missing_fonts = 0;
	uint64_t missing_icons = 0;
	uint64_t missing_strings = 0;
};
static_assert(std::is_trivially_copyable_v<TdmHudRecords>);

class Bf3TdmHud {
public:
	Bf3TdmHud();
	~Bf3TdmHud(); // frees its nodes if still in the tree (main thread)
	Bf3TdmHud(const Bf3TdmHud &) = delete;
	Bf3TdmHud &operator=(const Bf3TdmHud &) = delete;

	// Main thread, once the session is Done: adds the screens under p_parent (a CanvasLayer or a full-window Control),
	// drawing with the scene's localisation and UI assets (both may still be loading: the screens wait for them and
	// draw fallbacks until then). False with r_error when a screen cannot be made.
	bool build(godot::Node *p_parent, Bf3Localisation *p_localisation, Bf3UiAssets *p_ui_assets, std::string *r_error);
	bool built() const;
	// The model drawn from the next frame on (main thread, every display frame after fill_hud_tdm).
	void set_model(const core::ui::HudModel &p_model);
	// The spawn screen's mouse input since the last call (main thread): a kit button clicked (-1 none) and Deploy
	// clicked (the game then sends the SpawnRequest with its selected kit; keys are the game's input mapper).
	int take_kit_click();
	bool take_deploy_click();
	// Whether the mouse should be visible (the spawn screen or the round end is up): the game sets the mouse mode.
	bool wants_mouse() const;
	TdmHudRecords records() const;
	void shutdown(); // idempotent; main thread

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::godot_side
