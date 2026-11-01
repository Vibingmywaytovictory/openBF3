#include "bf3/core/ui/kick_popup.h"

#include "bf3/core/ui/text_db.h"

#include <string>

namespace bf3::core::ui {

HudPopup hud_kick_popup(std::string_view p_reason, bool p_timed_out, const TextDatabase &p_text) {
	HudPopup popup;
	popup.title = std::string(p_text.find(kKickTitleHash).value_or(std::string_view()));
	const std::string_view timed_out = p_text.find(kKickTimedOutHash).value_or(std::string_view());
	popup.text = p_timed_out && !timed_out.empty() ? std::string(timed_out) + ": " + std::string(p_reason)
												   : std::string(p_reason);
	popup.buttons.push_back(HudPopupButton{ std::string(p_text.find(kKickQuitHash).value_or(std::string_view())), -1 });
	return popup;
}

}
