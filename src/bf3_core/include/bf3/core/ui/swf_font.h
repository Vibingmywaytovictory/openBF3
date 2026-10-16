#pragma once
// DefineFont3 (and DefineFont2) glyph outlines of BF3's Scaleform font libraries (docs/design/ui-swf-fonts.md §7),
// clean-room from the public SWF specification and the retail data. Godot-free.
//
// Retail fonts (ui/static/fontcollection_en_fontlib, measured 2026-10-15): Purista EA Medium and Purista EA
// Semibold (443 glyphs each, 442 distinct codes: glyphs 0 and 1 are both U+0020), Venice-Medium and
// Venice-SemiBold (33 glyphs, 32 codes: U+0020 twice, then ! " # $ % & ' ( ) * + , - . / 0-9 : ; < = > ?). Flags
// 0x8C (HasLayout, WideOffsets, WideCodes) on all 63 outline tags (all of them in the 33 ui/static font
// libraries; the other 23 DefineFont3 tags are glyphless, flags 0x04); the code table is UCS-2, sorted ascending
// in all 63; Purista covers all of printable Latin-1 (U+00C9 is there for "MÉTRO") plus Latin Extended-A,
// Cyrillic and punctuation up to U+2588. Venice has digits and punctuation only, no letters.
//
// Units: glyph coordinates are SWF font units, 1024 em-square x 20 twips = kSwfFontUnitsPerEm per em, y down,
// baseline at 0 (cap height ~ -14337). Ascent 22240, descent 4320, leading 0 on all four. Advances and the
// kerning table (Purista: 335 / 324 pairs, -3482..1229) are in the same units.
// Winding (measured): outer contours have positive shoelace area in SWF's y-down coordinates and holes negative
// (Purista 676 / 158 contours, Venice 19 / 9, 0 exceptions), which is TrueType's clockwise-outer once y is
// negated. normalise_winding() enforces that rule by nesting depth anyway, so a font with mixed orientations
// cannot render with inverted holes under the non-zero fill rule.
//
// Glyph shape records: SHAPE with one fill bit and no line style; StraightEdge / CurvedEdge (quadratic) /
// StyleChange moveTo, all closed in retail. A contour is stored TrueType-style: on-curve and off-curve points,
// closed implicitly (the repeated end point is dropped).

#include "bf3/core/result.h"
#include "bf3/core/ui/swf_movie.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bf3::core::ui {

inline constexpr int32_t kSwfFontUnitsPerEm = 20480;
// Caps (retail maxima over the 63 outline fonts of the 33 libraries in brackets; the en fonts' in parentheses):
// glyphs per font [28,427] (443), points per glyph [281] (63), contours per glyph [24] (5).
inline constexpr uint32_t kSwfFontMaxGlyphs = 65535;
inline constexpr uint32_t kSwfFontMaxPointsPerGlyph = 16384;
inline constexpr uint32_t kSwfFontMaxContoursPerGlyph = 4096;

struct GlyphPoint {
	int32_t x = 0; // font units
	int32_t y = 0; // font units, y down (SWF)
	bool on_curve = true;
	bool operator==(const GlyphPoint &) const = default;
};

struct GlyphOutline {
	std::vector<GlyphPoint> points;
	std::vector<uint32_t> contour_ends; // index of each contour's last point (inclusive), ascending
	uint32_t edges = 0; // edge records read
};

struct GlyphBounds {
	int32_t x_min = 0;
	int32_t x_max = 0;
	int32_t y_min = 0;
	int32_t y_max = 0;
};

struct KerningPair {
	uint16_t left = 0; // codes
	uint16_t right = 0;
	int16_t adjust = 0; // font units
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
	std::vector<uint16_t> codes; // per glyph, the font's order (ascending in retail; duplicates possible)
	std::vector<GlyphOutline> glyphs; // empty for NoGlyphs / CodesOnly
	std::vector<int16_t> advances; // per glyph (layout only)
	std::vector<GlyphBounds> bounds; // per glyph (layout only; SWF stores them, the TTF writer recomputes)
	std::vector<KerningPair> kerning;
	uint32_t duplicate_codes = 0; // glyphs whose code an earlier glyph already has
	uint32_t trailing_bytes = 0;
};

// Reads one DefineFont2/3 tag body. Checks every offset (monotonic, inside the shape table), every shape record,
// the code table, the layout block and the kerning table against the tag length; charges one unit per edge,
// point and table entry.
Result<SwfFontOutlines> parse_font_outlines(std::span<const uint8_t> p_tag, uint16_t p_tag_code, WorkBudget &r_budget);
// The same for a font of a parsed movie (SwfMovie::font_tag).
Result<SwfFontOutlines> load_font_outlines(const SwfMovie &p_movie, const SwfFontInfo &p_font, WorkBudget &r_budget);

struct WindingStats {
	uint32_t contours = 0;
	uint32_t outer = 0; // even nesting depth
	uint32_t holes = 0; // odd nesting depth
	uint32_t reversed = 0; // contours whose direction normalise_winding changed (0 on the retail fonts)
	uint32_t degenerate = 0; // fewer than 3 points or zero area: left as they are
};

// Sets each contour's direction by its nesting depth (how many other contours contain one of its points, curves
// flattened at 4 steps): even depth positive shoelace area in SWF y-down units (TrueType clockwise after the
// y flip), odd depth negative. O(contours^2 x points) per glyph, charged.
Status normalise_winding(GlyphOutline &r_glyph, WorkBudget &r_budget, WindingStats *r_stats = nullptr);

} // namespace bf3::core::ui
