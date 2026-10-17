#pragma once
// Shared DefineFont2/3 tag walk (swf_font.cpp) for the movie parser's header facts (swf_movie.cpp) and the full
// outline reader (parse_font_outlines). One implementation of every bound of docs §7.1 / §9.

#include "bf3/core/ui/swf_font.h"

#include <cstdint>
#include <span>

namespace bf3::core::ui::detail {

// Reads one font tag body. p_glyphs false: the glyph shapes are not decoded and no per-glyph table is stored
// (codes, advances, bounds, kerning are validated and counted only); the kind, name, flags and trailing bytes are
// always filled. p_tag_code is swf_tag::kDefineFont2 or kDefineFont3.
Status read_font_tag(std::span<const uint8_t> p_tag, uint16_t p_tag_code, bool p_glyphs, WorkBudget &r_budget, SwfFontOutlines &r_font);

} // namespace bf3::core::ui::detail
