#pragma once

#include "bf3/core/ui/hud_screen.h"

#include <cstdint>
#include <string_view>

namespace bf3::core::ui {

class TextDatabase;

// SOURCE: bf3dump text: 0xf9dc710a YOU HAVE BEEN DISCONNECTED, 0x74dc0c2d TIMED OUT, 0x1f272fe7 QUIT.
// STAND-IN: the pick of these texts for a kick popup. Settle: footage of a retail kick popup.
inline constexpr uint32_t kKickTitleHash = 0xf9dc710au;
inline constexpr uint32_t kKickTimedOutHash = 0x74dc0c2du;
inline constexpr uint32_t kKickQuitHash = 0x1f272fe7u;
inline constexpr std::string_view kKickPopupButtons = "PopupButtonsManager_01";

HudPopup hud_kick_popup(std::string_view p_reason, bool p_timed_out, const TextDatabase &p_text);

}
