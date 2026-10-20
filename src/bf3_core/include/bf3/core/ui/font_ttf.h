#pragma once

#include "bf3/core/result.h"
#include "bf3/core/ui/swf_font.h"

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bf3::core::ui {

inline constexpr uint16_t kTrueTypeUnitsPerEm = 2048;

inline constexpr uint32_t kTrueTypeWriterVersion = 1;

inline constexpr size_t kTrueTypeReadBudgetUnits = size_t(1) << 22;

struct TrueTypeOptions {
	uint16_t units_per_em = kTrueTypeUnitsPerEm;
	std::string family;
	std::string subfamily;
	bool normalise_winding = true;
	bool notdef_box = true;
	bool kerning = true;
};

struct TrueTypeStats {
	uint32_t glyphs = 0;
	uint32_t cmap_codes = 0;
	uint32_t duplicate_codes = 0;
	uint32_t contours = 0;
	uint32_t points = 0;
	uint32_t contours_reversed = 0;
	uint32_t kerning_pairs = 0;
	uint32_t max_points = 0;
	uint32_t max_contours = 0;
	size_t bytes = 0;
};

Result<std::vector<uint8_t>> write_truetype(const SwfFontOutlines &p_font, const TrueTypeOptions &p_options, WorkBudget &r_budget,
		TrueTypeStats *r_stats = nullptr);

struct TrueTypeGlyph {
	uint16_t advance = 0;
	int16_t lsb = 0;
	int16_t x_min = 0;
	int16_t y_min = 0;
	int16_t x_max = 0;
	int16_t y_max = 0;
	std::vector<uint16_t> contour_ends;
	std::vector<int16_t> xs;
	std::vector<int16_t> ys;
	std::vector<uint8_t> on_curve;
};

struct TrueTypeFont {
	uint16_t units_per_em = 0;
	int16_t ascender = 0;
	int16_t descender = 0;
	int16_t line_gap = 0;
	uint16_t weight_class = 0;
	std::string family;
	std::string subfamily;
	std::vector<std::pair<uint16_t, uint16_t>> cmap;
	std::vector<TrueTypeGlyph> glyphs;
	std::vector<KerningPair> kerning;
	std::vector<std::string> tables;
};

Result<TrueTypeFont> read_truetype(std::span<const uint8_t> p_bytes, WorkBudget &r_budget);

}
