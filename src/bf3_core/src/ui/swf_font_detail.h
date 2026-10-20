#pragma once

#include "bf3/core/ui/swf_font.h"

#include <cstdint>
#include <span>

namespace bf3::core::ui::detail {

Status read_font_tag(std::span<const uint8_t> p_tag, uint16_t p_tag_code, bool p_glyphs, WorkBudget &r_budget, SwfFontOutlines &r_font);

}
