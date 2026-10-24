#pragma once

#include "bf3/core/result.h"
#include "bf3/core/ui/swf_font.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/ui/swf_movie.h"
#include "bf3/core/ui/ui_fonts.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ui {

inline constexpr float kSwfTextGutter = 2.0f;
inline constexpr uint8_t kSwfGridFitNone = 0;
inline constexpr uint8_t kSwfGridFitPixel = 1;
inline constexpr uint8_t kSwfGridFitSubpixel = 2;

double swf_grid_fit_baseline_shift(uint8_t p_grid_fit, double p_window_y, double p_window_per_local_y);
inline constexpr uint32_t kSwfTextMaxChars = 1u << 16;
inline constexpr uint32_t kSwfHtmlMaxNesting = 64;
inline constexpr uint32_t kSwfTextFitSteps = 24;
inline constexpr float kSwfTextFitMaxScale = 16.0f;
inline constexpr size_t kSwfTextBudgetUnits = size_t(1) << 22;

enum class SwfTextAutoSize : uint8_t {
	None,
	Left,
	Center,
	Right,
};
const char *swf_text_auto_size_name(SwfTextAutoSize p_mode);

enum class SwfTextFit : uint8_t {
	None,
	Shrink,
	Fit,
};
const char *swf_text_fit_name(SwfTextFit p_mode);

namespace swf_align {
constexpr uint8_t kLeft = 0;
constexpr uint8_t kRight = 1;
constexpr uint8_t kCenter = 2;
constexpr uint8_t kJustify = 3;
}

struct SwfTextFormat {
	std::string font;
	float size = 12.0f;
	SwfColor color;
	uint8_t align = swf_align::kLeft;
	float letter_spacing = 0.0f;
	bool kerning = false;
	bool bold = false;
	bool italic = false;
	bool underline = false;
	float left_margin = 0.0f;
	float right_margin = 0.0f;
	float indent = 0.0f;
	float block_indent = 0.0f;
	float leading = 0.0f;

	bool operator==(const SwfTextFormat &) const = default;
};

std::string swf_edit_text_font_name(const SwfMovie &p_movie, const SwfEditText &p_text);

SwfTextFormat swf_edit_text_format(const SwfMovie &p_movie, const SwfEditText &p_text);

struct SwfTextRun {
	uint32_t begin = 0;
	uint32_t end = 0;
	SwfTextFormat format;
};

struct SwfRichText {
	std::u32string text;
	std::vector<SwfTextRun> runs;
	uint32_t unknown_tags = 0;
	uint32_t unknown_entities = 0;

	const SwfTextFormat &format_at(uint32_t p_index, const SwfTextFormat &p_fallback) const;
};

std::u32string swf_utf8_decode(std::string_view p_utf8);

Result<SwfRichText> swf_plain_text(std::string_view p_text, const SwfTextFormat &p_format);

Result<SwfRichText> swf_parse_html(std::string_view p_html, const SwfTextFormat &p_base, WorkBudget &r_budget);

struct SwfFontFace {
	std::string long_name;
	SwfFontOutlines outlines;
	std::vector<std::pair<uint16_t, uint16_t>> code_to_glyph;
	std::vector<KerningPair> kerning_sorted;

	std::optional<uint16_t> glyph(char32_t p_code) const;
	int32_t kerning(char32_t p_left, char32_t p_right) const;
};

class SwfFontSet {
public:
	static Result<SwfFontSet> load(const SwfLibrary &p_library, const FontMappingCollection &p_mapping, WorkBudget &r_budget);

	static SwfFontSet from_outlines(FontMappingCollection p_mapping, std::vector<SwfFontOutlines> p_fonts);

	const SwfFontFace *find(std::string_view p_used_name) const;
	const FontMappingCollection &mapping() const { return mapping_; }
	const std::vector<SwfFontFace> &faces() const { return faces_; }

private:
	FontMappingCollection mapping_;
	std::vector<SwfFontFace> faces_;
};

struct SwfTextOptions {
	SwfTextAutoSize auto_size = SwfTextAutoSize::None;
	SwfTextFit fit = SwfTextFit::None;
};

SwfTextOptions swf_edit_text_options(const SwfEditText &p_field);

struct SwfGlyphPlacement {
	const SwfFontFace *face = nullptr;
	uint16_t glyph = 0;
	char32_t code = 0;
	float x = 0.0f;
	float baseline = 0.0f;
	float size = 0.0f;
	float advance = 0.0f;
	SwfColor color;
	bool underline = false;
	uint32_t line = 0;
};

struct SwfTextLine {
	float x = 0.0f;
	float top = 0.0f;
	float baseline = 0.0f;
	float width = 0.0f;
	float ascent = 0.0f;
	float descent = 0.0f;
	float leading = 0.0f;
	uint8_t align = swf_align::kLeft;
	uint32_t first_glyph = 0;
	uint32_t glyph_count = 0;
};

struct SwfTextLayout {
	SwfRect bounds;
	float text_width = 0.0f;
	float text_height = 0.0f;
	float scale = 1.0f;
	std::vector<SwfGlyphPlacement> glyphs;
	std::vector<SwfTextLine> lines;
	uint32_t missing_glyphs = 0;
	uint32_t missing_fonts = 0;
};

Result<SwfTextLayout> swf_layout_text(const SwfFontSet &p_fonts, const SwfEditText &p_field, const SwfRichText &p_text, const SwfTextOptions &p_options,
		WorkBudget &r_budget);

}
