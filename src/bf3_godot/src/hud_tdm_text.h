#pragma once
// M2b ui: every string, font, icon and placement the TDM HUD screens look up (hud_tdm.h; docs/design/m2b.md §4; as
// built: docs/design/m2b-ui.md §3). Godot-free, so ui.hud.tdm.install resolves the very same lists against the install
// (the text database, the en font mapping, the iconsingame atlas, the screens' placements) that the screens use.
//
// Strings are SIDs of the install's text database, found in the screens' SWF layouts (killscreen, spawnscreenpc,
// matchpreroundinformation) or by their English text, every one [verified 2026-10-19: bf3dump text, swf layout]; the
// English column is what the install row compares the database's text with. Three short texts have no SID that this
// track found and are openBF3's own (kTdmOwnTexts, [inferred]); they are not lookups and never count as missing.

#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/swf_layout.h" // HudAnchor

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace bf3::godot_side {

enum class TdmText : uint8_t {
	KitAssault,
	KitEngineer,
	KitSupport,
	KitRecon,
	DeployAs, // the kit bar's header
	Deploy, // the Deploy button
	TimeToDeploy, // the button's text while it waits
	KilledBy, // the kill card
	Suicide,
	Health,
	Headshot, // "HEADSHOT {0:s}": the score message's text, drawn without its argument
	RoundWillStartIn, // the pre-round countdown
	WaitingForPlayers,
	TeamWon, // the round end
	TeamLost,
	Winner,
	NextRoundIn,
	TeamUs, // HudTeam::team1_name / team2_name (the feed puts the same hashes there)
	TeamRu,
	Name, // the scoreboard's columns
	Kills,
	Deaths,
	Score,
	Ping,
	Count,
};
inline constexpr size_t kTdmTextCount = size_t(TdmText::Count);

struct TdmTextEntry {
	TdmText key;
	std::string_view sid;
	std::string_view english; // the en database's text [verified 2026-10-19]
};

inline constexpr std::array<TdmTextEntry, kTdmTextCount> kTdmTexts{ {
	{ TdmText::KitAssault, "ID_M_ASSAULT", "ASSAULT" },
	{ TdmText::KitEngineer, "ID_M_ENGINEER", "ENGINEER" },
	{ TdmText::KitSupport, "ID_M_SUPPORT", "SUPPORT" },
	{ TdmText::KitRecon, "ID_M_RECON", "RECON" },
	{ TdmText::DeployAs, "ID_M_SPAWN_DEPLOY_AS", "DEPLOY AS" }, // spawnscreenpc / kitbar layout
	{ TdmText::Deploy, "ID_M_SPAWN_DEPLOY", "DEPLOY" },
	{ TdmText::TimeToDeploy, "ID_M_SPAWN_TIME_TO_DEPLOY", "TIME TO DEPLOY" },
	{ TdmText::KilledBy, "ID_H_KILLEDBY", "KILLED BY" }, // killscreen layout
	{ TdmText::Suicide, "ID_H_KILLCAMSUICIDE", "YOU COMMITTED SUICIDE" }, // killscreen layout
	{ TdmText::Health, "ID_H_HEALTH", "HEALTH" }, // killscreen layout
	{ TdmText::Headshot, "ID_SCORE_HEADSHOT", "HEADSHOT {0:s}" },
	{ TdmText::RoundWillStartIn, "ID_H_PREROUND_COUNTDOWN", "ROUND WILL START IN" },
	{ TdmText::WaitingForPlayers, "ID_H_PREROUND_WAITING_FOR_PLAYERS_TITLE", "WAITING FOR PLAYERS" },
	{ TdmText::TeamWon, "ID_M_EOR_TEAMWON", "YOUR TEAM WON!" },
	{ TdmText::TeamLost, "ID_M_EOR_TEAMLOST", "YOUR TEAM LOST!" },
	{ TdmText::Winner, "ID_H_SCOREBOARD_WINNER", "WINNER" },
	{ TdmText::NextRoundIn, "ID_M_EOR_NEXT_ROUND_STARTS_IN", "NEXT ROUND STARTS IN:" },
	{ TdmText::TeamUs, "ID_M_US", "US" },
	{ TdmText::TeamRu, "ID_M_RU", "RU" },
	{ TdmText::Name, "ID_M_SB_NAME", "NAME" },
	{ TdmText::Kills, "ID_H_SCOREBOARD_KILLS", "K" },
	{ TdmText::Deaths, "ID_H_SCOREBOARD_DEATH", "D" },
	{ TdmText::Score, "ID_H_SCOREBOARD_SCORE", "SCORE" },
	{ TdmText::Ping, "ID_M_SB_CONNECTION", "PING" },
} };

constexpr std::string_view tdm_text_sid(TdmText p_key) {
	return size_t(p_key) < kTdmTextCount ? kTdmTexts[size_t(p_key)].sid : std::string_view();
}
// The kit names by HudPlayer::kit (assault, engineer, support, recon).
inline constexpr std::array<TdmText, core::ui::kHudKitCount> kTdmKitNames{ TdmText::KitAssault, TdmText::KitEngineer, TdmText::KitSupport,
	TdmText::KitRecon };

// openBF3's own texts (no SID found for them) [inferred].
struct TdmOwnTexts {
	std::string_view revive_possible = "REVIVE POSSIBLE";
	std::string_view bot = "BOT";
	std::string_view metres = "M";
};
inline constexpr TdmOwnTexts kTdmOwnTexts{};

// Fonts (Bf3UiAssets::get_font; each must resolve to a converted font, not Device / Unresolved).
inline constexpr std::string_view kTdmNumberFont = "$HUD_medium"; // digits: counts, timers
inline constexpr std::string_view kTdmTextFont = "$Menu_medium"; // names, labels
inline constexpr std::string_view kTdmTitleFont = "$Menu_bold"; // headers: DEPLOY, the winner, the killer
inline constexpr std::array<std::string_view, 3> kTdmFonts{ kTdmNumberFont, kTdmTextFont, kTdmTitleFont };

// Atlas icons (Bf3UiAssets::get_region; the movie is in Bf3UiAssets' default atlas set) [verified 2026-10-19: bf3dump
// swf labels ui/assets/iconsingame]. The kill feed's and the kill card's weapon icons are the items' _small icons
// (ItemIconIndex, through the M2a HUD's loaded icons); the install row resolves those of the TDM kits.
inline constexpr std::string_view kTdmIconMovie = "ui/assets/iconsingame";
inline constexpr std::array<std::string_view, core::ui::kHudKitCount> kTdmKitIcons{ "mc_iconsIngameSmall/KitAssault", "mc_iconsIngameSmall/KitEngineer",
	"mc_iconsIngameSmall/KitSupport", "mc_iconsIngameSmall/KitRecon" };
inline constexpr std::string_view kTdmDeadIcon = "mc_iconsIngameSmall/dead"; // the scoreboard's dead players
inline constexpr size_t kTdmIconCount = core::ui::kHudKitCount + 1;

// Placements read from the screens' SWF trees (Bf3UiAssets::hud_placements), with the retail values as the fallback
// [verified 2026-10-19: bf3dump swf hud]. The kill feed sits at hudmpscreen's top-right message widget.
struct TdmPlacement {
	std::string_view movie;
	std::string_view instance;
	float x = 0; // the retail stage position (the fallback)
	float y = 0;
	core::ui::HudAnchor anchor = core::ui::HudAnchor::Centre;
};
inline constexpr TdmPlacement kTdmKillFeedPlacement{ "ui/assets/hudmpscreen", "HudMessage_02", 1280.0f, 0.0f, core::ui::HudAnchor::TopRight };
inline constexpr TdmPlacement kTdmKitBarPlacement{ "ui/assets/spawnscreenpc", "KitBar_01", 96.0f, 557.0f, core::ui::HudAnchor::BottomLeft };
inline constexpr TdmPlacement kTdmSpawnCounterPlacement{ "ui/assets/spawnscreenpc", "TicketCounterSpawnScreen_01", 769.0f, 97.0f, core::ui::HudAnchor::Top };
inline constexpr std::array<TdmPlacement, 3> kTdmPlacements{ kTdmKillFeedPlacement, kTdmKitBarPlacement, kTdmSpawnCounterPlacement };

} // namespace bf3::godot_side
