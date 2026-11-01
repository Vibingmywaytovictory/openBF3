#pragma once

#include "bf3/core/ui/hud_screen.h"

#include <cstdint>
#include <string_view>

namespace bf3::core::ui {

class TextDatabase;

inline constexpr uint32_t kKickTitleHash = 0xf9dc710au;
inline constexpr uint32_t kKickTimedOutHash = 0x74dc0c2du;
inline constexpr uint32_t kKickQuitHash = 0x1f272fe7u;
inline constexpr std::string_view kKickPopupButtons = "PopupButtonsManager_01";

HudPopup hud_kick_popup(std::string_view p_reason, bool p_timed_out, const TextDatabase &p_text);

}
