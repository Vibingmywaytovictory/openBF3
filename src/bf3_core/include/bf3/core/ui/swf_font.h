#pragma once

#include "bf3/core/result.h"
#include "bf3/core/ui/swf_movie.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bf3::core::ui {

inline constexpr int32_t kSwfFontUnitsPerEm = 20480;

inline constexpr uint32_t kSwfFontMaxGlyphs = 65535;
inline constexpr uint32_t kSwfFontMaxPointsPerGlyph = 16384;
inline constexpr uint32_t kSwfFontMaxContoursPerGlyph = 4096;

struct GlyphPoint {
	int32_t x = 0;
	int32_t y = 0;
	bool on_curve = true;
	bool operator==(const GlyphPoint &) const = default;
};

struct GlyphOutline {
	std::vector<GlyphPoint> points;
	std::vector<uint32_t> contour_ends;
	uint32_t edges = 0;
};

struct GlyphBounds {
	int32_t x_min = 0;
	int32_t x_max = 0;
	int32_t y_min = 0;
	int32_t y_max = 0;
};

struct KerningPair {
	uint16_t left = 0;
	uint16_t right = 0;
	int16_t adjust = 0;
};

struct SwfFontOutlines {
	uint16_t id = 0;
	std::string name;
	SwfFontKind kind = SwfFontKind::Outlines;
	bool bold = false;
	bool italic = false;
	bool small_text = false;
	uint8_t language = 0;
	bool has_layout = false;
	int32_t ascent = 0;
	int32_t descent = 0;
	int32_t leading = 0;
	std::vector<uint16_t> codes;
	std::vector<GlyphOutline> glyphs;
	std::vector<int16_t> advances;
	std::vector<GlyphBounds> bounds;
	std::vector<KerningPair> kerning;
	uint32_t duplicate_codes = 0;
	uint32_t trailing_bytes = 0;
};

Result<SwfFontOutlines> parse_font_outlines(std::span<const uint8_t> p_tag, uint16_t p_tag_code, WorkBudget &r_budget);

Result<SwfFontOutlines> load_font_outlines(const SwfMovie &p_movie, const SwfFontInfo &p_font, WorkBudget &r_budget);

struct WindingStats {
	uint32_t contours = 0;
	uint32_t outer = 0;
	uint32_t holes = 0;
	uint32_t reversed = 0;
	uint32_t degenerate = 0;
};

Status normalise_winding(GlyphOutline &r_glyph, WorkBudget &r_budget, WindingStats *r_stats = nullptr);

}
