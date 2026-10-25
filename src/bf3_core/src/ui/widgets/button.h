#pragma once

#include "hud_widget_support.h"

#include <functional>

namespace bf3::core::ui {

using HudButtonCallback = std::function<void(const std::string &)>;

struct HudButtonCallbacks {
	HudButtonCallback press;
	HudButtonCallback release;
	HudButtonCallback over;
	HudButtonCallback out;
};

class HudButton {
public:
	virtual ~HudButton() = default;
	virtual void set_button_type(std::string p_type) = 0;
	virtual void set_button_text(std::string p_text) = 0;
	virtual void set_align_text(std::string p_align) = 0;
	virtual void set_resize_if_too_wide(bool p_resize) = 0;
	virtual void set_callbacks(HudButtonCallbacks p_callbacks) = 0;
	virtual void set_send_event_with_function(bool p_send) = 0;
	virtual float button_width() = 0;
	virtual void set_button_width(float p_width) = 0;
	virtual void set_selected(bool p_selected) = 0;
	virtual bool selected() = 0;
	virtual void set_highlighted(bool p_highlighted) = 0;
	virtual void set_active(bool p_active) = 0;
	virtual void set_plus() = 0;
	virtual void remove_plus() = 0;
	virtual const std::string &current_label() const = 0;
};

}
