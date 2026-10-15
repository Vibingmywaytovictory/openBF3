#pragma once
// An in-memory TrueType font built from DefineFont3 outlines by our own code (docs/design/ui-swf-fonts.md §7.2),
// for Godot's FontFile.set_data. Written from Apple's TrueType Reference Manual and the OpenType 1.9 specification
// (table layouts only); no third-party font code. Godot-free.
//
// Tables (sfnt version 0x00010000, tag order, 4-byte aligned, checksums and head.checkSumAdjustment computed):
//   OS/2 v4  usWeightClass from the name (Medium 500, Semibold/SemiBold 600, Bold 700, else 400), typo and win
//            metrics = ascent / descent, fsSelection USE_TYPO_METRICS | REGULAR or BOLD, ulUnicodeRange from cmap
//   cmap     format 4, platform 3 / encoding 1 and platform 0 / encoding 3 (both point at one subtable); codes are
//            UCS-2, so no format 12; a duplicate code maps to its first glyph
//   glyf     simple glyphs only; point flags with x/y short vectors and repeats; no instructions
//   head     unitsPerEm (2048), indexToLocFormat 1 (long), bounding box over all glyphs, macStyle
//   hhea     ascender / descender / lineGap from the font's layout, advanceWidthMax, numberOfHMetrics = numGlyphs
//   hmtx     advance + left side bearing (= glyph x_min) per glyph
//   kern     version 0, one format-0 subtable (horizontal pairs, sorted by (left, right) glyph id) when the font
//            has kerning; HarfBuzz applies it when no GPOS table is present
//   loca, maxp v1.0 (maxPoints / maxContours computed, no hinting), name (IDs 1, 2, 3, 4, 6; platform 3 UTF-16BE
//            and platform 1 Roman), post v3.0 (no glyph names)
// Glyph 0 is .notdef (a hollow box, 0.6 em x 0.7 em, so a missing character shows); source glyph i is glyph i + 1.
// Scale: unitsPerEm 2048 = kSwfFontUnitsPerEm / 10, every coordinate rounded to the nearest unit (at most
// 1/4096 em off); y negated (SWF y down -> TrueType y up).

#include "bf3/core/result.h"
#include "bf3/core/ui/swf_font.h"

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bf3::core::ui {

inline constexpr uint16_t kTrueTypeUnitsPerEm = 2048;
// Bump whenever the bytes the writer produces for the same input change (it is part of the font cache key).
inline constexpr uint32_t kTrueTypeWriterVersion = 1;

struct TrueTypeOptions {
	uint16_t units_per_em = kTrueTypeUnitsPerEm;
	std::string family; // name ID 1; default: the font's name ("Purista EA Medium")
	std::string subfamily; // name ID 2; default "Regular" or "Bold"
	bool normalise_winding = true;
	bool notdef_box = true;
	bool kerning = true;
};

struct TrueTypeStats {
	uint32_t glyphs = 0; // numGlyphs, .notdef included (444 for Purista)
	uint32_t cmap_codes = 0; // distinct codes mapped (442 for Purista)
	uint32_t duplicate_codes = 0;
	uint32_t contours = 0;
	uint32_t points = 0;
	uint32_t contours_reversed = 0; // by normalise_winding (0 on the retail fonts)
	uint32_t kerning_pairs = 0;
	uint32_t max_points = 0;
	uint32_t max_contours = 0;
	size_t bytes = 0;
};

// Builds the font. Fails for a font without outlines (NoGlyphs / CodesOnly), more than 65,534 glyphs, a glyph
// over the TrueType limits (65,535 points, 32,767 contours) or a coordinate outside int16 after scaling.
Result<std::vector<uint8_t>> write_truetype(const SwfFontOutlines &p_font, const TrueTypeOptions &p_options, WorkBudget &r_budget,
		TrueTypeStats *r_stats = nullptr);

// What read_truetype() checks and returns: the subset of a TrueType file this project writes, read back with
// every offset, length and checksum verified (the cache's accept step and the tests' round trip).
struct TrueTypeGlyph {
	uint16_t advance = 0;
	int16_t lsb = 0;
	int16_t x_min = 0;
	int16_t y_min = 0;
	int16_t x_max = 0;
	int16_t y_max = 0;
	std::vector<uint16_t> contour_ends;
	std::vector<int16_t> xs; // absolute, font units, y up
	std::vector<int16_t> ys;
	std::vector<uint8_t> on_curve; // 1 / 0 per point
};

struct TrueTypeFont {
	uint16_t units_per_em = 0;
	int16_t ascender = 0;
	int16_t descender = 0;
	int16_t line_gap = 0;
	uint16_t weight_class = 0;
	std::string family;
	std::string subfamily;
	std::vector<std::pair<uint16_t, uint16_t>> cmap; // (code, glyph), ascending code
	std::vector<TrueTypeGlyph> glyphs;
	std::vector<KerningPair> kerning; // left / right are glyph ids here
	std::vector<std::string> tables; // tags in file order
};

Result<TrueTypeFont> read_truetype(std::span<const uint8_t> p_bytes, WorkBudget &r_budget);

} // namespace bf3::core::ui
