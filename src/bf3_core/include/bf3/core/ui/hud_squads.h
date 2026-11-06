#pragma once

#include "bf3/core/result.h"
#include "bf3/core/ui/hud_model.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace bf3::core {
class EbxResolver;
}

namespace bf3::core::ui {

struct HudClientState;

inline constexpr std::string_view kUiSquadCompPartition = "ui/uicomponents/uisquadcomp";
inline constexpr int32_t kSquadAvailableSquadsKey = 1663005790;
inline constexpr int32_t kSquadMembersKey = -487423817;
inline constexpr int32_t kSquadSelectedSquadKey = -164313827;
inline constexpr int32_t kSquadSelectedSquadIndexKey = -966791869;
inline constexpr int32_t kSquadSelectedPlayerKey = -859667140;
inline constexpr int32_t kSquadNoSelection = -1;
inline constexpr int32_t kSquadJoinButtonKey = -259424110;
inline constexpr int32_t kSquadCreateButtonKey = 1426133474;
inline constexpr int32_t kSquadKickButtonKey = -1283593862;
inline constexpr int32_t kSquadFlowActionKey = -1497091703;
inline constexpr int32_t kSwitchTeamFlowActionKey = 1283842800;
inline constexpr int32_t kSquadPrivacyKey = -1385101326;
inline constexpr int32_t kSquadPrivacyEnabledKey = 1518455639;
inline constexpr int32_t kSquadPrivacyVisibleKey = -331214896;
inline constexpr int32_t kSquadHeaderNameKey = -917467149;
inline constexpr std::string_view kSquadJoinParam = "join";
inline constexpr std::string_view kSquadCreateParam = "create";
inline constexpr std::string_view kSquadKickParam = "kick";
inline constexpr std::string_view kSquadJoinSid = "ID_M_JOIN";
inline constexpr std::string_view kSquadLeaveSid = "ID_M_SQUAD_LEAVE";
inline constexpr std::string_view kSquadCreateSid = "ID_M_SQUAD_CREATE";
inline constexpr std::string_view kSquadKickSid = "ID_M_KICK";

using HudSquadNames = std::array<std::string, kHudMaxSquads>;

Result<HudSquadNames> read_squad_names(const EbxResolver &p_db);

int32_t hud_selected_squad(const HudModel &p_model, const HudClientState &p_client);
uint8_t hud_selected_squad_player(const HudModel &p_model, const HudClientState &p_client);

}
