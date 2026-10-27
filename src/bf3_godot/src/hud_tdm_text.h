#pragma once

#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/swf_layout.h"

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
	DeployAs,
	Deploy,
	TimeToDeploy,
	KilledBy,
	Suicide,
	Health,
	Headshot,
	TeamWon,
	TeamLost,
	Winner,
	NextRoundIn,
	TeamUs,
	TeamRu,
	Count,
};
inline constexpr size_t kTdmTextCount = size_t(TdmText::Count);

struct TdmTextEntry {
	TdmText key;
	std::string_view sid;
	std::string_view english;
};

inline constexpr std::array<TdmTextEntry, kTdmTextCount> kTdmTexts{ {
	{ TdmText::KitAssault, "ID_M_ASSAULT", "ASSAULT" },
	{ TdmText::KitEngineer, "ID_M_ENGINEER", "ENGINEER" },
	{ TdmText::KitSupport, "ID_M_SUPPORT", "SUPPORT" },
	{ TdmText::KitRecon, "ID_M_RECON", "RECON" },
	{ TdmText::DeployAs, "ID_M_SPAWN_DEPLOY_AS", "DEPLOY AS" },
	{ TdmText::Deploy, "ID_M_SPAWN_DEPLOY", "DEPLOY" },
	{ TdmText::TimeToDeploy, "ID_M_SPAWN_TIME_TO_DEPLOY", "TIME TO DEPLOY" },
	{ TdmText::KilledBy, "ID_H_KILLEDBY", "KILLED BY" },
	{ TdmText::Suicide, "ID_H_KILLCAMSUICIDE", "YOU COMMITTED SUICIDE" },
	{ TdmText::Health, "ID_H_HEALTH", "HEALTH" },
	{ TdmText::Headshot, "ID_SCORE_HEADSHOT", "HEADSHOT {0:s}" },
	{ TdmText::TeamWon, "ID_M_EOR_TEAMWON", "YOUR TEAM WON!" },
	{ TdmText::TeamLost, "ID_M_EOR_TEAMLOST", "YOUR TEAM LOST!" },
	{ TdmText::Winner, "ID_H_SCOREBOARD_WINNER", "WINNER" },
	{ TdmText::NextRoundIn, "ID_M_EOR_NEXT_ROUND_STARTS_IN", "NEXT ROUND STARTS IN:" },
	{ TdmText::TeamUs, "ID_M_US", "US" },
	{ TdmText::TeamRu, "ID_M_RU", "RU" },
} };

constexpr std::string_view tdm_text_sid(TdmText p_key) {
	return size_t(p_key) < kTdmTextCount ? kTdmTexts[size_t(p_key)].sid : std::string_view();
}

inline constexpr std::array<TdmText, core::ui::kHudKitCount> kTdmKitNames{ TdmText::KitAssault, TdmText::KitEngineer, TdmText::KitSupport,
	TdmText::KitRecon };

struct TdmOwnTexts {
	std::string_view revive_possible = "REVIVE POSSIBLE";
	std::string_view bot = "BOT";
	std::string_view metres = "M";
};
inline constexpr TdmOwnTexts kTdmOwnTexts{};

inline constexpr std::string_view kTdmNumberFont = "$HUD_medium";
inline constexpr std::string_view kTdmTextFont = "$Menu_medium";
inline constexpr std::string_view kTdmTitleFont = "$Menu_bold";
inline constexpr std::array<std::string_view, 3> kTdmFonts{ kTdmNumberFont, kTdmTextFont, kTdmTitleFont };

inline constexpr std::string_view kTdmIconMovie = "ui/assets/iconsingame";
inline constexpr std::array<std::string_view, core::ui::kHudKitCount> kTdmKitIcons{ "mc_iconsIngameSmall/KitAssault", "mc_iconsIngameSmall/KitEngineer",
	"mc_iconsIngameSmall/KitSupport", "mc_iconsIngameSmall/KitRecon" };
inline constexpr std::string_view kTdmDeadIcon = "mc_iconsIngameSmall/dead";
inline constexpr size_t kTdmIconCount = core::ui::kHudKitCount + 1;

struct TdmPlacement {
	std::string_view movie;
	std::string_view instance;
	float x = 0;
	float y = 0;
	core::ui::HudAnchor anchor = core::ui::HudAnchor::Centre;
};
inline constexpr TdmPlacement kTdmKitBarPlacement{ "ui/assets/spawnscreenpc", "KitBar_01", 96.0f, 557.0f, core::ui::HudAnchor::BottomLeft };
inline constexpr TdmPlacement kTdmSpawnCounterPlacement{ "ui/assets/spawnscreenpc", "TicketCounterSpawnScreen_01", 769.0f, 97.0f, core::ui::HudAnchor::Top };
inline constexpr std::array<TdmPlacement, 2> kTdmPlacements{ kTdmKitBarPlacement, kTdmSpawnCounterPlacement };

}
