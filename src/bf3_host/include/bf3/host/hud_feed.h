#pragma once
// The minimal HUD's feed (docs/design/m2a.md §11.4; owner hud; as built: docs/design/m2a-hud.md): fills ui2's
// HudModel from the presented own weapon, the own soldier and the drained presentation events. Pure and Godot-free
// (ui.hud.feed.unit tests it); the Godot controls (Bf3Hud: ammo, crosshair, hit indicator, weapon name and icon,
// health; src/bf3_godot/src/hud_controls.h) read only the HudModel. Nothing in the HUD reads the sim directly (§11.3).
//
// Fed fields (ui2's M2a-tagged sources): active_weapon_ammo, _clips_left, _clip_capacity, _is_reloading,
// _reload_percent, _critical_ammo, active_weapon_name, active_weapon_data (item id -> the _small icon through
// ItemIconIndex), dispersion (degrees, the interpolated predicted spread), zoomed, firing_mode, show_crosshair,
// show_hitindicator / hitindicator_level, the soldier's pose and health. The meaning of each field as fed is listed
// at fill_hud below (the data names the sources, not their units; the choices are this track's, [inferred]).
// Hit indicator (§16 Q7, taken 2026-10-16): shown kHitIndicatorSeconds per server-confirmed hit whose shooter is the
// own soldier, level kHitIndicatorLevel; no client-predicted markers.
//
// The second half of this header is the Godot-free layout the controls draw from: BF3's 1280 x 720 stage mapped
// into the window by min(w / 1280, h / 720) around each widget's anchor (ui-swf-fonts.md §8.3), the text slots of
// the hudmpscreen tree (Ammo_01, Health_01) read at run time with read_layout (never committed: BF3 data), and the
// crosshair gap (h / 2) tan(spread) / tan(vfov / 2).

#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/result.h"
#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/swf_layout.h"
#include "bf3/host/presentation_events.h"
#include "bf3/host/presentation_weapon.h"
#include "bf3/net/protocol.h" // M2b W2.0 (ui): net::MatchState, net::PlayerRow in HudFeedInput
#include "bf3/sim/sim_types.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace bf3::host {

inline constexpr double kHitIndicatorSeconds = 0.3; // §16 Q7
inline constexpr float kHitIndicatorLevel = 1.0f; // §16 Q7
// ActiveWeaponCriticalAmmo: the magazine at or below this fraction of its capacity (weapons with a capacity > 1)
// [inferred: BF3's low-ammo colour; the threshold is not in the data the HUD reads].
inline constexpr float kCriticalAmmoFraction = 0.25f;

enum class HudCrosshairKind : uint8_t { None = 0, Rifle = 1, Pistol = 2, Shotgun = 3, Launcher = 4, Grenade = 5, Knife = 6 };

// FiringMode / FiringModeTypes as fed: the HUD's fire-mode indicator kinds (the ammo widget's fmd single / burst /
// auto sprites). HudWeapon::firing_mode is the active mode's kind (-1: none, e.g. a grenade's hold-and-release),
// firing_mode_types one bit (1 << kind) per kind the weapon's modes hold.
enum class HudFireMode : int8_t { None = -1, Single = 0, Burst = 1, Auto = 2 };
HudFireMode hud_fire_mode(core::gameplay::FireLogic p_logic);

// HudWeaponSlot::weapon_class as fed: 1 + the index of the def's WeaponClass text in kHudWeaponClassNames, 0 when
// the class is not listed. The resolver keeps the class as its enum text (WeaponDef::weapon_class), not the EBX
// enum value, so this is openBF3's own stable numbering (docs/design/m2a-hud.md).
inline constexpr std::array<std::string_view, 16> kHudWeaponClassNames{ "wc556x45mmNATO", "wc545x45mmWP", "wc762x51mmNATO", "wc762x54mmR",
	"wc762x39mmWP", "wc9x19mm", "wc45cal", "wc44mag", "wc12gauge", "wc338Magnum", "wc50cal", "wcHgr", "wcKnife", "wcUgl", "wcRocket",
	"wcPowerTool" };
int32_t hud_weapon_class(std::string_view p_weapon_class);

// Per weapon def (index = def id of the match's WeaponTable).
struct HudStaticRow {
	uint32_t name_sid_hash = 0; // text_hash of UIWeaponDescription.Name (WeaponDef::name_sid_hash), or of the
	                            // UIKitItemDescription.Name the item index gives (the M67: apply_item_names)
	uint32_t item_id = 0; // the unlock identifier (ItemIconIndex -> the _small icon)
	int32_t weapon_class = 0; // hud_weapon_class (HudWeaponSlot::weapon_class)
	int32_t clip_capacity = 0;
	uint32_t fire_modes = 0; // bit (1 << HudFireMode) per mode of the def (HudWeaponSlot::fire_modes, FiringModeTypes)
	HudCrosshairKind crosshair = HudCrosshairKind::None;
	bool show_ammo = true; // false for the knife
	// Added by the implementation (hud's own header): the HudFireMode of each WeaponSimDef::modes entry (None past
	// mode_count), the spare magazines at spawn and the unlimited flag (the knife).
	std::array<HudFireMode, 4> mode_kinds{ HudFireMode::None, HudFireMode::None, HudFireMode::None, HudFireMode::None };
	int32_t magazines = 0;
	bool unlimited_ammo = false;
};
static_assert(std::is_trivially_copyable_v<HudStaticRow>);

struct HudStatic {
	std::vector<HudStaticRow> rows;
	const HudStaticRow *row(uint16_t p_def) const { return p_def < rows.size() ? &rows[p_def] : nullptr; }
};

// Built from the match's table and the WeaponDefs it was made from (p_defs[i] is the source of p_table.defs[i]).
// Fails when the spans differ in length, or a def's unlock identifier differs from its table entry's.
// Crosshair kinds: wcKnife Knife; wcHgr Grenade; a missile projectile Launcher; more than one pellet Shotgun; the
// pistol animation set (wabsPstl) Pistol; else Rifle.
core::Result<HudStatic> make_hud_static(const sim::weapons::WeaponTable &p_table, std::span<const core::gameplay::WeaponDef> p_defs);
// Fills the name of every row whose WeaponDef had none (name_sid_hash 0) from the item index's description of its
// item id (UIKitItemDescription.Name: the M67's ID_P_INAME_M67 = 0x26436abd "M67 GRENADE" [verified 2026-10-16]).
// Returns the number of rows filled.
size_t apply_item_names(HudStatic &r_static, const core::ui::ItemIconIndex &p_items);

struct KitTable; // M2b W2.0 (ui): bf3/host/kits.h
// M2b W2.0 (ui, docs/design/m2b.md §4): the kill card's inputs that are not in the events or the frame. gx sets them in
// the display frame that drains the own Death event (the killer's presented position is gone a few frames later).
struct HudKillCardSource {
	bool killer_seen = false; // the killer was drawn (present_others) when the own Death event was drained
	float distance_m = -1.0f; // own position to the killer's presented position then (metres); < 0 unknown
};

struct HudFeedInput {
	PresentedWeapon weapon; // present_weapon of this display frame
	bool alive = false;
	float health = 0; // the own soldier's presented health
	sim::Pose pose = sim::Pose::Stand;
	double now_s = 0; // presentation clock (seconds), for the hit indicator's timer
	// M2b W2.0 (docs/design/m2b.md §2.6 #16, §4; ui reads them in fill_hud_tdm, gx sets them per display frame from
	// the PresentationFrame; fill_hud ignores them, so M2a is unchanged). See the M2b W2.0 block at the end of this file.
	bool m2b = false; // M2b W2.0 (ui): feed the M2b screens (false: fill_hud_tdm hides them)
	bool match_valid = false; // M2b W2.0 (ui): PresentationFrame::match_valid
	net::MatchState match; // M2b W2.0 (ui): PresentationFrame::match
	std::span<const net::PlayerRow> players; // M2b W2.0 (ui): PresentationFrame::players[0, player_count), ascending slot
	uint8_t own_slot = 0xFF; // M2b W2.0 (ui): ClientStats::slot once spawned (0xFF: none yet)
	uint8_t own_team = 0; // M2b W2.0 (ui): PresentationFrame::own_team (0 unknown, 1, 2)
	sim::Tick server_tick = 0; // M2b W2.0 (ui): the newest server tick received (PresentationFrame::newest_tick)
	sim::Tick own_respawn_tick = 0; // M2b W2.0 (ui): PresentationFrame::own_respawn_tick
	sim::Tick own_revive_until = 0; // M2b W2.0 (ui): PresentationFrame::own_revive_until
	bool own_unspawned = false; // M2b W2.0 (ui): PresentationFrame::own_unspawned
	bool spawned_this_round = false; // M2b W2.0 (ui): the own soldier had a life since the round (re)started
	uint8_t spawn_kit = 0; // M2b W2.0 (ui): the spawn screen's selected kit (the game keeps it: keys, clicks)
	bool scoreboard_held = false; // M2b W2.0 (ui): Tab held -> HudTeam::scoreboard_enabled
	const KitTable *kits = nullptr; // M2b W2.0 (ui): the match's kits (ClientGunplayInputs), the kit bar's items
	HudKillCardSource kill_card_source; // M2b W2.0 (ui): what only the game knows at the own death
};

// What the feed keeps between frames (the hit indicator's timer, counters for the gates).
struct HudFeedState {
	double hit_indicator_until_s = 0;
	uint64_t hits_shown = 0; // server hits by the own soldier that raised the indicator (E3, E10)
	// Added by the implementation: Hit events drained that were not the own soldier's (not shown), and every publish.
	uint64_t hits_ignored = 0;
	uint64_t frames = 0;
};

// One display frame. As fed (docs/design/m2a-hud.md §3):
//   weapon: with a valid weapon and a live soldier, active_weapon_ammo = magazine, active_weapon_ammo_in_bag =
//     reserve (rounds), active_weapon_ammo_total = magazine + reserve, _clips_left = ceil(reserve / capacity),
//     _clips_total = the row's magazines, _clip_capacity, _is_reloading (phase Reloading or ReloadShell),
//     _reload_percent 0..100 (phase progress), _critical_ammo, _show_ammo (the row's, false without a weapon),
//     active_weapon_name (SID hash), active_weapon_data, active_weapon_crosshair (HudCrosshairKind),
//     active_weapon_index (slot), dispersion = absolute_dispersion = the presented spread (degrees), zoomed and
//     active_weapon_zoomed = the predicted kZoomWasDown, firing_mode / firing_mode_types (HudFireMode), can_fire
//     (phase Ready), show_crosshair (not zoomed, a crosshair kind). Without a weapon or a live soldier the weapon
//     fields are cleared (show_crosshair and the ammo off).
//   hit indicator: each drained Hit with present_flags::kOwnShooter raises it until now_s + kHitIndicatorSeconds;
//     show_hitindicator while now_s is before that, hitindicator_level kHitIndicatorLevel then, else 0.
//   soldier: is_alive, health, pose, first_person, show_hud_ammo.
//   serial: one more than the model's previous serial.
void fill_hud(const HudFeedInput &p_input, std::span<const PresentEvent> p_drained, const HudStatic &p_static, HudFeedState &r_state,
		core::ui::HudModel &r_model);

// ---- layout (Godot-free; the controls draw from it) ------------------------------------------------------------------

inline constexpr float kHudStageWidth = 1280.0f; // BF3's HUD stage (every hud*screen movie)
inline constexpr float kHudStageHeight = 720.0f;
inline constexpr std::string_view kHudScreenMovie = "ui/assets/hudmpscreen";
inline constexpr std::string_view kHitIndicatorMovie = "ui/assets/hitindicator";
// The hit indicator's art per level: the #4 sprite's frame labels level1..level3 (four mirrored pieces of one 15 x 15
// sub-image at +-18 px; bf3dump swf labels hitindicator, 2026-10-16). M2a shows level 1 only (§16 Q7).
inline constexpr std::array<std::string_view, 3> kHitIndicatorRegions{ "#4/level1", "#4/level2", "#4/level3" };
inline constexpr std::string_view kHudNumberFont = "$HUD_medium"; // the ammo and health digits (the layout's own font)
inline constexpr std::string_view kHudTextFont = "$Menu_medium"; // the weapon name (the ammo widget's labels' font)

// min(w / 1280, h / 720) (ui-swf-fonts.md §8.3); 0 for an empty window.
float hud_scale(float p_width, float p_height);

struct HudPoint {
	float x = 0;
	float y = 0;
};
// A stage point of a widget anchored at p_anchor, in a p_width x p_height window: the window's anchor point plus the
// point's offset from the stage's anchor point, times hud_scale. On a 16:9 window this is the stage point times the
// scale for every anchor.
HudPoint hud_stage_to_window(core::ui::HudAnchor p_anchor, float p_stage_x, float p_stage_y, float p_width, float p_height);

// The crosshair's gap from the screen centre in pixels: (h / 2) tan(spread) / tan(vfov / 2), spread the cone's
// half-angle and vfov the camera's vertical field of view, both degrees. 0 when the inputs are not usable
// (h <= 0, spread < 0 or not finite, vfov outside (0, 180)); spread is capped at 89 degrees.
double crosshair_gap_px(double p_height, double p_spread_deg, double p_vfov_deg);

// One edit text of the screen tree, in stage pixels.
struct HudTextSlot {
	bool found = false;
	std::string path; // "Ammo_01/Ammo/clip/cltxt/txtDisplay"
	float x = 0; // the text field's origin (its stage matrix translation)
	float y = 0;
	float scale_x = 1; // the stage matrix's axis lengths
	float scale_y = 1;
	core::ui::SwfRect bounds; // the field's rectangle in its own space
	float size = 0; // font size in stage pixels (before scale_y)
	uint8_t align = 0; // SWF EditText: 0 left, 1 right, 2 centre, 3 justify
	std::string font; // "$HUD_medium"
	core::ui::SwfColor color;
	std::string initial_text;
};
// A widget the screen places, in stage pixels.
struct HudWidgetSlot {
	bool found = false;
	std::string instance; // "Ammo_01"
	float x = 0;
	float y = 0;
	core::ui::HudAnchor anchor = core::ui::HudAnchor::Centre; // hud_anchor_for on the 1280 x 720 stage
};

struct HudScreenLayout {
	HudWidgetSlot ammo; // Ammo_01 (1194, 669)
	HudWidgetSlot health; // Health_01 (1193, 605)
	HudWidgetSlot hit_indicator; // Hitindicator_01 (640, 360)
	HudTextSlot ammo_clip; // the magazine: $HUD_medium 43 px, right-aligned
	HudTextSlot ammo_reserve; // the reserve: $HUD_medium 21 px, right-aligned
	HudTextSlot ammo_slash; // the "/" between them, 18 px
	HudTextSlot health_value; // $HUD_medium 21 px, right-aligned
	HudTextSlot health_percent; // "%", 16 px
	std::vector<std::string> missing; // paths not found (the control falls back to its own placement, logged)
	size_t nodes = 0; // nodes visited
};

// The slots of the hudmpscreen tree (read_layout of kHudScreenMovie, imports followed): the root's instance1 holds
// the widgets; text slots by instance path, the unnamed "/" and "%" by their initial text inside the widget. Every
// visited node charges p_budget; a node count above core::ui::kSwfLayoutMaxNodes or a depth above
// core::ui::kSwfLayoutMaxDepth * 4 fails. Missing slots are listed, not an error; a tree without instance1 is.
core::Result<HudScreenLayout> make_hud_screen_layout(const core::ui::SwfLayoutNode &p_root, core::gameplay::WorkBudget &r_budget);

// Where an edit text draws one line (Flash's 2 px gutter, ascent from the font): x of the line's start for left
// alignment, of its end for right alignment, of its centre for centred text, and the baseline, in stage pixels.
struct HudTextAnchor {
	float x = 0;
	float baseline = 0;
	uint8_t align = 0;
	float size = 0; // stage pixels (size x scale_y)
};
HudTextAnchor hud_text_anchor(const HudTextSlot &p_slot, float p_ascent_per_px);

} // namespace bf3::host

// ---- M2b W2.0 (docs/design/m2b.md §2.6 #16, §4; owner ui; contract: docs/design/m2b-w2.md) -------------------------
// The M2b half of the feed: the kill feed, the scoreboard rows, the kill counter, the spawn screen, the kill card, the
// pre-round and the round end, from HudFeedInput's M2b fields and the drained events. Pure and Godot-free like
// fill_hud (ui.hud.tdm.unit tests it from recorded frames). gx calls it right after fill_hud in the same display
// frame with the same input and the same drained span (the body is ui's: src/bf3_host/src/hud_feed_tdm.cpp).
//
// As fed (the rules ui's body follows; m2b.contract2's ui rows check the pinned ones once kUiLanded):
//   players       HudModel::players[slot] from each PlayerRow (name, team, kit, alive, bot, kills, deaths, score, ping;
//                 local = the own slot), player_count = the highest row slot + 1, local_player = own_slot,
//                 local_team = own_team; slots without a row are cleared
//   team          team1_players / team2_players / sorted_players by score (then kills, then slot), friendly_team /
//                 enemy_team1 from own_team (1 when unknown), the friendly / enemy lists, top_player1..3_name,
//                 the winner flags on RoundOver, scoreboard_enabled = scoreboard_held
//   kill_counter  team1_kills / team2_kills = MatchState::kills, team1_max_kills = team2_max_kills = kill_limit, the
//                 player / leading / enemy team values from own_team
//   messages      each drained Death event (slot = killer, aux = victim) is one kill_message line: serial + 1, killer
//                 and victim = their slots (killer = victim: suicide, a fall), item_id from p_static's row of the def
//                 (0 without one), headshot (kHeadshot), team_kill from the two rows' teams; with kOwnShooter (and not
//                 a suicide) also a local_kill_message line. The screen fades a line kKillFeedSeconds after it first
//                 drew its serial (HudKill has no time: the control's own clock)
//   kill_card     each drained Death event with kOwnVictim starts a card: serial + 1, visible for kKillCardSeconds of
//                 now_s, killer / killer_kit / headshot / suicide, item_id and weapon_name_sid_hash from the def's row,
//                 killer_health = the event's value, distance_m from kill_card_source; seconds_left counts down
//   spawn_screen  visible while m2b, match_valid, the own soldier is not alive (or own_unspawned), no kill card is
//                 visible and the phase is not RoundOver; at_join = !spawned_this_round; selected_kit = spawn_kit;
//                 can_deploy = server_tick >= own_respawn_tick and the phase is PreRound or InRound; the seconds from
//                 (own_respawn_tick - server_tick) / 60; revive_possible = server_tick < own_revive_until (and not at
//                 join); kit_items from p_input.kits' loadouts of the own team (team 1 when unknown), slot order
//                 kHudSpawnSlots, each def's item_id from p_static (0 for kNoWeapon or a def without a row)
//   soldier       show_medic_revive = spawn_screen.revive_possible, time_until_revive = seconds_revive_left,
//                 time_until_death = 0 (no interactive man-down, §3.6)
//   spawn_logic   time_to_respawn = seconds_to_deploy, start_time_to_respawn = the own death's (respawn_tick - death
//                 tick) / 60 when known
//   pre_round     visible in PreRound; counting_down = countdown_end_tick != 0; seconds_left from it; input_restricted
//                 = restrict_until_tick != 0 and server_tick < restrict_until_tick
//   end_of_round  visible in RoundOver: winner, local_won, team_kills, kill_limit, top_players by score,
//                 seconds_to_next_round from restart_tick (0 while not scheduled)
// With m2b false every M2b screen is hidden (visible false) and the M2a fields fill_hud wrote are left as they are.
// Hostile or odd inputs (a slot >= 64 in a row or an event, a phase past RoundOver, ticks in the past) are clamped or
// skipped and counted in HudTdmFeedState::bad_inputs, never trusted as indices.
namespace bf3::host {

inline constexpr double kKillFeedSeconds = 5.0; // a kill-feed line's display time (m2b.md §4: the comp data's, default 5 s) [inferred]
inline constexpr double kKillCardSeconds = 4.0; // the kill card and the death camera (m2b.md §4) [inferred]
// The kit bar's weapon slots: kKitSlots plus the gadget slot kKitGadgetSlot (the defibrillator with TDM kits).
inline constexpr std::array<uint8_t, core::ui::kHudSpawnKitItems> kHudSpawnSlots{ 0, 1, 2, 6, 7 };

// What fill_hud_tdm keeps between frames.
struct HudTdmFeedState {
	uint32_t kill_serial = 0; // the last kill_message serial
	uint32_t local_kill_serial = 0; // the last local_kill_message serial
	uint32_t kill_card_serial = 0;
	double kill_card_until_s = 0; // now_s at which the card hides
	sim::Tick own_death_respawn_tick = 0; // the last own Death event's extra (respawn_tick), for start_time_to_respawn
	sim::Tick own_death_tick = 0; // the server tick of that death (the event's time)
	// Counters for the gates (E9) and the unit rows.
	uint64_t deaths_fed = 0; // Death events made into kill-feed lines
	uint64_t own_deaths = 0; // kill cards started
	uint64_t unknown_defs = 0; // a Death's def without a HudStatic row (no icon)
	uint64_t bad_inputs = 0; // rows / events / phases skipped or clamped
	uint64_t frames = 0;
};
static_assert(std::is_trivially_copyable_v<HudTdmFeedState>);

void fill_hud_tdm(const HudFeedInput &p_input, std::span<const PresentEvent> p_drained, const HudStatic &p_static, HudTdmFeedState &r_state,
		core::ui::HudModel &r_model);

} // namespace bf3::host
