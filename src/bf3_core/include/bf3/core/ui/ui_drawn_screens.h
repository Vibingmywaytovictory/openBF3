#pragma once

#include "bf3/core/ui/hud_screen.h"

#include <array>
#include <string_view>

namespace bf3::core::ui {

inline constexpr std::string_view kIngameMenuPartition = "ui/flow/screen/ingamemenump";
inline constexpr std::string_view kMenuScoreboardPartition = "ui/flow/screen/scoreboards/scoreboardtwoteamsscreen";
inline constexpr std::string_view kMenuScoreboardBackPartition = "ui/flow/screen/scoreboardbackscreen";
inline constexpr std::string_view kPopupGenericPartition = "ui/flow/screen/popups/popupgeneric";
inline constexpr std::array<std::string_view, 4> kMenuDrawnPartitions{ kIngameMenuPartition, kMenuScoreboardPartition,
	kMenuScoreboardBackPartition, kPopupGenericPartition };

inline constexpr std::string_view kSpawnButtonScreenPartition = "ui/flow/screen/spawnbuttonscreen";
inline constexpr std::string_view kSpawnTicketCounterPartition = "ui/flow/screen/spawnscreenticketcountertdmscreen";
inline constexpr std::array<std::string_view, 3> kDeployDrawnPartitions{ kSpawnScreenPcPartition,
	kSpawnButtonScreenPartition, kSpawnTicketCounterPartition };
inline constexpr std::array<std::string_view, 3> kCustomizeScreenPartitions{ "ui/flow/screen/customizesoldierscreen",
	"ui/flow/screen/customizesoldiersubscreen", "ui/flow/screen/customizeaccessoriesscreen" };

inline constexpr std::string_view kEorHud64Partition = "ui/flow/screen/scoreboards/scoreboardtwoteamshud64screen";
inline constexpr std::string_view kEorTabsPartition = "ui/flow/screen/eordetailstabsscreen";
inline constexpr std::string_view kEorScoreboardPartition = "ui/flow/screen/eordetailsscoreboardscreen";
inline constexpr std::string_view kEorTwoTeamsPartition = kMenuScoreboardPartition;
inline constexpr std::string_view kEorStatsPartition = "ui/flow/screen/eordetailsstatsscreen";
inline constexpr std::string_view kEorPopupPartition = kPopupGenericPartition;
inline constexpr std::array<std::string_view, 7> kNativeEorScreens{ kEorWinningTeamScreenPartition, kEorHud64Partition,
	kEorTabsPartition, kEorScoreboardPartition, kEorTwoTeamsPartition, kEorStatsPartition, kEorPopupPartition };

inline constexpr std::string_view kPopupBindKeyPartition = "ui/flow/screen/popups/popupbindkey";
inline constexpr std::string_view kPopupDelayPartition = "ui/flow/screen/popups/popupdelay";
inline constexpr std::string_view kScreenAdjustPartition = "ui/flow/screen/screenadjustscreen";

inline constexpr std::array<std::string_view, 2> kOverlayDrawnPartitions{ kScoreboardHud32ScreenPartition,
	kPreRoundWaitingScreenPartition };
inline constexpr std::array<std::string_view, 4> kHudDrawnPartitions{ kHudMpScreenPartition, kHudTdmScreenPartition,
	kCrosshairDefaultPartition, kHudSharedScreenPartition };

}
