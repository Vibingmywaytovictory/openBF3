#include "bf3/core/ui/swf_text.h"

#include "bf3/core/util/text.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <string>

namespace bf3::core::ui {

namespace {

void append_char(SwfRichText &r_out, char32_t p_c, const SwfTextFormat &p_format) {
	const uint32_t at = uint32_t(r_out.text.size());
	r_out.text.push_back(p_c);
	if (!r_out.runs.empty() && r_out.runs.back().end == at && r_out.runs.back().format == p_format) {
		r_out.runs.back().end = at + 1;
		return;
	}
	SwfTextRun run;
	run.begin = at;
	run.end = at + 1;
	run.format = p_format;
	r_out.runs.push_back(std::move(run));
}

bool is_break(char32_t p_c) {
	return p_c == U'\r' || p_c == U'\n';
}

std::optional<float> parse_float(std::string_view p_text) {
	float v = 0.0f;
	const char *b = p_text.data();
	const char *e = p_text.data() + p_text.size();
	if (b != e && *b == '+') {
		++b;
	}
	const auto [end, ec] = std::from_chars(b, e, v);
	if (ec != std::errc() || end != e || !std::isfinite(v)) {
		return std::nullopt;
	}
	return v;
}

std::optional<SwfColor> parse_color(std::string_view p_text) {
	if (p_text.size() != 7 || p_text[0] != '#') {
		return std::nullopt;
	}
	uint32_t v = 0;
	const auto [end, ec] = std::from_chars(p_text.data() + 1, p_text.data() + 7, v, 16);
	if (ec != std::errc() || end != p_text.data() + 7) {
		return std::nullopt;
	}
	SwfColor c;
	c.r = uint8_t(v >> 16);
	c.g = uint8_t(v >> 8);
	c.b = uint8_t(v);
	c.a = 255;
	return c;
}

std::optional<uint8_t> parse_align(std::string_view p_text) {
	const std::string v = to_lower_ascii(p_text);
	if (v == "left") {
		return swf_align::kLeft;
	}
	if (v == "right") {
		return swf_align::kRight;
	}
	if (v == "center") {
		return swf_align::kCenter;
	}
	if (v == "justify") {
		return swf_align::kJustify;
	}
	return std::nullopt;
}

struct Attribute {
	std::string name;
	std::string value;
};

class HtmlParser {
public:
	HtmlParser(std::string_view p_html, const SwfTextFormat &p_base, WorkBudget &r_budget) :
			html_(p_html), budget_(r_budget) {
		stack_.push_back({ std::string(), p_base });
	}

	Result<SwfRichText> run() {
		size_t i = 0;
		while (i < html_.size()) {
			Status st = budget_.charge(1);
			if (!st) {
				return make_error("swf html: work budget exhausted (" + st.error().message + ")");
			}
			if (html_[i] == '<') {
				const size_t close = html_.find('>', i + 1);
				if (close == std::string_view::npos) {
					out_.unknown_tags++;
					BF3_TRY(text(html_.substr(i)));
					break;
				}
				BF3_TRY(tag(html_.substr(i + 1, close - i - 1)));
				i = close + 1;
				continue;
			}
			const size_t next = html_.find('<', i);
			const std::string_view chunk = html_.substr(i, next == std::string_view::npos ? std::string_view::npos : next - i);
			BF3_TRY(text(chunk));
			i += chunk.size();
		}
		return std::move(out_);
	}

private:
	struct Level {
		std::string tag;
		SwfTextFormat format;
	};

	const SwfTextFormat &current() const { return stack_.back().format; }

	Status put(char32_t p_c) {
		if (out_.text.size() >= kSwfTextMaxChars) {
			return make_error("swf html: more than " + std::to_string(kSwfTextMaxChars) + " characters");
		}
		if (pending_break_ && !is_break(p_c)) {
			pending_break_ = false;
			append_char(out_, U'\r', paragraph_format_);
		}
		append_char(out_, p_c, current());
		return Status::success();
	}

	Status text(std::string_view p_chunk) {
		size_t i = 0;
		while (i < p_chunk.size()) {
			if (p_chunk[i] == '&') {
				const size_t semi = p_chunk.find(';', i + 1);
				if (semi != std::string_view::npos && semi - i <= 10) {
					if (const std::optional<char32_t> c = entity(p_chunk.substr(i + 1, semi - i - 1))) {
						BF3_TRY(put(*c));
						i = semi + 1;
						continue;
					}
				}
				out_.unknown_entities++;
			}
			size_t j = i + 1;
			while (j < p_chunk.size() && p_chunk[j] != '&') {
				++j;
			}
			for (const char32_t c : swf_utf8_decode(p_chunk.substr(i, j - i))) {
				BF3_TRY(put(c));
			}
			i = j;
		}
		return Status::success();
	}

	static std::optional<char32_t> entity(std::string_view p_name) {
		if (p_name == "lt") {
			return U'<';
		}
		if (p_name == "gt") {
			return U'>';
		}
		if (p_name == "amp") {
			return U'&';
		}
		if (p_name == "quot") {
			return U'"';
		}
		if (p_name == "apos") {
			return U'\'';
		}
		if (p_name == "nbsp") {
			return char32_t(0xa0);
		}
		if (p_name.size() >= 2 && p_name[0] == '#') {
			const bool hex = p_name[1] == 'x' || p_name[1] == 'X';
			const std::string_view digits = p_name.substr(hex ? 2 : 1);
			uint32_t v = 0;
			const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), v, hex ? 16 : 10);
			if (ec == std::errc() && end == digits.data() + digits.size() && !digits.empty() && v <= 0x10ffff) {
				return char32_t(v);
			}
		}
		return std::nullopt;
	}

	static std::vector<Attribute> attributes(std::string_view p_body) {
		std::vector<Attribute> out;
		size_t i = 0;
		auto space = [&] {
			while (i < p_body.size() && (p_body[i] == ' ' || p_body[i] == '\t' || p_body[i] == '\r' || p_body[i] == '\n')) {
				++i;
			}
		};
		while (true) {
			space();
			if (i >= p_body.size() || p_body[i] == '/') {
				break;
			}
			const size_t name_begin = i;
			while (i < p_body.size() && p_body[i] != '=' && p_body[i] != ' ' && p_body[i] != '/') {
				++i;
			}
			Attribute a;
			a.name = to_lower_ascii(p_body.substr(name_begin, i - name_begin));
			space();
			if (i < p_body.size() && p_body[i] == '=') {
				++i;
				space();
				if (i < p_body.size() && (p_body[i] == '"' || p_body[i] == '\'')) {
					const char quote = p_body[i++];
					const size_t end = p_body.find(quote, i);
					const size_t stop = end == std::string_view::npos ? p_body.size() : end;
					a.value = std::string(p_body.substr(i, stop - i));
					i = stop == p_body.size() ? stop : stop + 1;
				} else {
					const size_t begin = i;
					while (i < p_body.size() && p_body[i] != ' ') {
						++i;
					}
					a.value = std::string(p_body.substr(begin, i - begin));
				}
			}
			if (a.name.empty()) {
				++i;
				continue;
			}
			out.push_back(std::move(a));
		}
		return out;
	}

	Status open(const std::string &p_tag, const SwfTextFormat &p_format) {
		if (stack_.size() > kSwfHtmlMaxNesting) {
			return make_error("swf html: tags nested deeper than " + std::to_string(kSwfHtmlMaxNesting));
		}
		stack_.push_back({ p_tag, p_format });
		return Status::success();
	}

	void close(const std::string &p_tag) {
		for (size_t i = stack_.size(); i-- > 1;) {
			if (stack_[i].tag == p_tag) {
				stack_.resize(i);
				return;
			}
		}
		out_.unknown_tags++;
	}

	void start_paragraph() {
		if (!out_.text.empty() && !is_break(out_.text.back())) {
			pending_break_ = true;
		}
	}

	Status tag(std::string_view p_body) {
		const bool closing = !p_body.empty() && p_body[0] == '/';
		const std::string_view rest = closing ? p_body.substr(1) : p_body;
		size_t name_end = 0;
		while (name_end < rest.size() && rest[name_end] != ' ' && rest[name_end] != '/' && rest[name_end] != '\t') {
			++name_end;
		}
		const std::string name = to_lower_ascii(rest.substr(0, name_end));
		if (closing) {
			if (name == "p" || name == "li") {
				paragraph_format_ = current();
				close(name);
				if (!out_.text.empty()) {
					pending_break_ = true;
				}
				return Status::success();
			}
			if (name == "font" || name == "b" || name == "i" || name == "u" || name == "textformat" || name == "a" || name == "span") {
				close(name);
				return Status::success();
			}
			out_.unknown_tags++;
			return Status::success();
		}
		const std::vector<Attribute> attrs = attributes(rest.substr(name_end));
		SwfTextFormat f = current();
		if (name == "br" || name == "sbr") {
			pending_break_ = false;
			if (out_.text.size() >= kSwfTextMaxChars) {
				return make_error("swf html: more than " + std::to_string(kSwfTextMaxChars) + " characters");
			}
			append_char(out_, U'\r', current());
			return Status::success();
		}
		if (name == "p" || name == "li") {
			start_paragraph();
			for (const Attribute &a : attrs) {
				if (a.name == "align") {
					if (const std::optional<uint8_t> v = parse_align(a.value)) {
						f.align = *v;
						continue;
					}
				}
				if (a.name != "class") {
					out_.unknown_tags++;
				}
			}
			return open(name, f);
		}
		if (name == "font") {
			for (const Attribute &a : attrs) {
				if (a.name == "face") {
					f.font = a.value;
				} else if (a.name == "size") {
					const std::optional<float> v = parse_float(a.value);
					if (!v) {
						out_.unknown_tags++;
					} else if (!a.value.empty() && (a.value[0] == '+' || a.value[0] == '-')) {
						f.size = std::max(0.0f, f.size + *v);
					} else {
						f.size = *v;
					}
				} else if (a.name == "color") {
					if (const std::optional<SwfColor> c = parse_color(a.value)) {
						f.color = SwfColor{ c->r, c->g, c->b, f.color.a };
					} else {
						out_.unknown_tags++;
					}
				} else if (a.name == "letterspacing") {
					if (const std::optional<float> v = parse_float(a.value)) {
						f.letter_spacing = *v;
					} else {
						out_.unknown_tags++;
					}
				} else if (a.name == "kerning") {
					f.kerning = a.value != "0" && to_lower_ascii(a.value) != "false";
				} else {
					out_.unknown_tags++;
				}
			}
			return open(name, f);
		}
		if (name == "textformat") {
			for (const Attribute &a : attrs) {
				const std::optional<float> v = parse_float(a.value);
				if (!v) {
					out_.unknown_tags++;
					continue;
				}
				if (a.name == "leftmargin") {
					f.left_margin = *v;
				} else if (a.name == "rightmargin") {
					f.right_margin = *v;
				} else if (a.name == "indent") {
					f.indent = *v;
				} else if (a.name == "blockindent") {
					f.block_indent = *v;
				} else if (a.name == "leading") {
					f.leading = *v;
				} else {
					out_.unknown_tags++;
				}
			}
			return open(name, f);
		}
		if (name == "b") {
			f.bold = true;
			return open(name, f);
		}
		if (name == "i") {
			f.italic = true;
			return open(name, f);
		}
		if (name == "u") {
			f.underline = true;
			return open(name, f);
		}
		if (name == "a" || name == "span") {
			return open(name, f);
		}
		out_.unknown_tags++;
		return Status::success();
	}

	std::string_view html_;
	WorkBudget &budget_;
	std::vector<Level> stack_;
	SwfRichText out_;
	bool pending_break_ = false;
	SwfTextFormat paragraph_format_;
};

struct Item {
	char32_t code = 0;
	const SwfTextFormat *format = nullptr;
	const SwfFontFace *face = nullptr;
	uint16_t glyph = 0;
	bool has_glyph = false;
	float advance = 0.0f;
	float size = 0.0f;
	float ascent = 0.0f;
	float descent = 0.0f;
};

bool is_space(char32_t p_c) {
	return p_c == U' ' || p_c == U'\t' || p_c == char32_t(0xa0);
}

class Layouter {
public:
	Layouter(const SwfFontSet &p_fonts, const SwfEditText &p_field, const SwfRichText &p_text, WorkBudget &r_budget) :
			fonts_(p_fonts), field_(p_field), text_(p_text), budget_(r_budget) {}

	Status layout(float p_scale, const SwfRect &p_bounds, SwfTextLayout &r_out) {
		r_out = SwfTextLayout{};
		r_out.scale = p_scale;
		r_out.bounds = p_bounds;
		BF3_TRY(items(p_scale, r_out));
		const bool wrap = field_.has(SwfEditText::kWordWrap);
		uint32_t para_begin = 0;
		float top = p_bounds.y_min + kSwfTextGutter;
		const uint32_t n = uint32_t(items_.size());
		while (para_begin <= n) {
			uint32_t para_end = para_begin;
			while (para_end < n && !is_break(items_[para_end].code)) {
				++para_end;
			}
			const SwfTextFormat &pf = text_.format_at(para_begin < n ? para_begin : (n == 0 ? 0 : n - 1), base_);
			BF3_TRY(paragraph(para_begin, para_end, pf, p_scale, p_bounds, wrap, top, r_out));
			if (para_end >= n) {
				break;
			}
			para_begin = para_end + 1;
		}
		float height = 0.0f;
		for (size_t i = 0; i < r_out.lines.size(); ++i) {
			const SwfTextLine &l = r_out.lines[i];
			height += l.ascent + l.descent + (i + 1 < r_out.lines.size() ? l.leading : 0.0f);
			r_out.text_width = std::max(r_out.text_width, l.width);
		}
		r_out.text_height = height;
		return Status::success();
	}

	void set_base(const SwfTextFormat &p_base) { base_ = p_base; }

private:
	Status items(float p_scale, SwfTextLayout &r_out) {
		items_.clear();
		items_.reserve(text_.text.size());
		const SwfFontFace *last_face = nullptr;
		std::string last_font;
		bool last_resolved = false;
		for (uint32_t i = 0; i < text_.text.size(); ++i) {
			BF3_TRY(budget_.charge(1));
			Item it;
			it.code = text_.text[i];
			it.format = &text_.format_at(i, base_);
			if (!last_resolved || it.format->font != last_font) {
				last_font = it.format->font;
				last_face = fonts_.find(last_font);
				last_resolved = true;
			}
			it.face = last_face;
			it.size = it.format->size * p_scale;
			const float units = it.size / float(kSwfFontUnitsPerEm);
			if (it.face != nullptr) {
				it.ascent = float(it.face->outlines.ascent) * units;
				it.descent = float(it.face->outlines.descent) * units;
			}
			if (is_break(it.code)) {
				items_.push_back(it);
				continue;
			}
			if (it.face == nullptr) {
				r_out.missing_fonts++;
				items_.push_back(it);
				continue;
			}
			const std::optional<uint16_t> g = it.face->glyph(it.code == char32_t(0xa0) ? U' ' : it.code);
			if (!g) {
				r_out.missing_glyphs++;
				items_.push_back(it);
				continue;
			}
			it.glyph = *g;
			it.has_glyph = true;
			const std::vector<int16_t> &adv = it.face->outlines.advances;
			it.advance = (*g < adv.size() ? float(adv[*g]) * units : 0.0f) + it.format->letter_spacing * p_scale;
			if (!items_.empty()) {
				Item &prev = items_.back();
				if (prev.has_glyph && prev.face == it.face && prev.format->kerning && it.format->kerning && prev.size == it.size) {
					prev.advance += float(it.face->kerning(prev.code, it.code)) * units;
				}
			}
			items_.push_back(it);
		}
		return Status::success();
	}

	float width_of(uint32_t p_begin, uint32_t p_end) const {
		float w = 0.0f;
		for (uint32_t i = p_begin; i < p_end; ++i) {
			w += items_[i].advance;
		}
		return w;
	}

	uint32_t trim_end(uint32_t p_begin, uint32_t p_end) const {
		while (p_end > p_begin && is_space(items_[p_end - 1].code)) {
			--p_end;
		}
		return p_end;
	}

	Status paragraph(uint32_t p_begin, uint32_t p_end, const SwfTextFormat &p_format, float p_scale, const SwfRect &p_bounds, bool p_wrap, float &r_top,
			SwfTextLayout &r_out) {
		const float left = p_bounds.x_min + kSwfTextGutter + (p_format.left_margin + p_format.block_indent) * p_scale;
		const float right = p_bounds.x_max - kSwfTextGutter - p_format.right_margin * p_scale;
		uint32_t line_begin = p_begin;
		bool first = true;
		do {
			BF3_TRY(budget_.charge(1));
			const float indent = first ? p_format.indent * p_scale : 0.0f;
			const float avail = right - left - indent;
			uint32_t line_end = p_end;
			if (p_wrap) {
				float w = 0.0f;
				uint32_t last_break = line_begin;
				uint32_t i = line_begin;
				for (; i < p_end; ++i) {
					const Item &it = items_[i];
					if (is_space(it.code)) {
						last_break = i + 1;
						w += it.advance;
						continue;
					}
					if (w + it.advance > avail && i > line_begin) {
						break;
					}
					w += it.advance;
				}
				if (i < p_end) {
					line_end = last_break > line_begin ? last_break : std::max(i, line_begin + 1);
				}
			}
			const uint32_t visible_end = trim_end(line_begin, line_end);
			emit_line(line_begin, line_end, visible_end, line_end >= p_end, p_format, p_scale, left + indent, right, r_top, r_out);
			line_begin = line_end;
			first = false;
		} while (line_begin < p_end);
		return Status::success();
	}

	void emit_line(uint32_t p_begin, uint32_t p_end, uint32_t p_visible_end, bool p_last, const SwfTextFormat &p_format, float p_scale, float p_left, float p_right,
			float &r_top, SwfTextLayout &r_out) {
		SwfTextLine line;
		line.align = p_format.align;
		line.leading = p_format.leading * p_scale;
		for (uint32_t i = p_begin; i < p_end; ++i) {
			line.ascent = std::max(line.ascent, items_[i].ascent);
			line.descent = std::max(line.descent, items_[i].descent);
		}
		if (p_begin == p_end) {
			const SwfFontFace *face = fonts_.find(p_format.font);
			if (face != nullptr) {
				const float units = p_format.size * p_scale / float(kSwfFontUnitsPerEm);
				line.ascent = float(face->outlines.ascent) * units;
				line.descent = float(face->outlines.descent) * units;
			}
		}
		line.width = width_of(p_begin, p_visible_end);
		float x = p_left;
		float gap = 0.0f;
		const float slack = p_right - p_left - line.width;
		if (p_format.align == swf_align::kRight) {
			x = p_left + slack;
		} else if (p_format.align == swf_align::kCenter) {
			x = p_left + slack / 2.0f;
		} else if (p_format.align == swf_align::kJustify && !p_last && slack > 0.0f) {
			uint32_t spaces = 0;
			for (uint32_t i = p_begin; i < p_visible_end; ++i) {
				spaces += is_space(items_[i].code) ? 1 : 0;
			}
			gap = spaces > 0 ? slack / float(spaces) : 0.0f;
		}
		line.x = x;
		line.top = r_top;
		line.baseline = r_top + line.ascent;
		line.first_glyph = uint32_t(r_out.glyphs.size());
		for (uint32_t i = p_begin; i < p_visible_end; ++i) {
			const Item &it = items_[i];
			if (it.has_glyph) {
				SwfGlyphPlacement g;
				g.face = it.face;
				g.glyph = it.glyph;
				g.code = it.code;
				g.x = x;
				g.baseline = line.baseline;
				g.size = it.size;
				g.advance = it.advance;
				g.color = it.format->color;
				g.underline = it.format->underline;
				g.line = uint32_t(r_out.lines.size());
				r_out.glyphs.push_back(g);
			}
			x += it.advance + (is_space(it.code) ? gap : 0.0f);
		}
		if (gap > 0.0f) {
			line.width = p_right - p_left;
		}
		line.glyph_count = uint32_t(r_out.glyphs.size()) - line.first_glyph;
		r_top += line.ascent + line.descent + line.leading;
		r_out.lines.push_back(line);
	}

	const SwfFontSet &fonts_;
	const SwfEditText &field_;
	const SwfRichText &text_;
	WorkBudget &budget_;
	SwfTextFormat base_;
	std::vector<Item> items_;
};

SwfRect auto_sized(const SwfRect &p_bounds, const SwfTextLayout &p_measured, SwfTextAutoSize p_mode, bool p_wrap, float p_margins) {
	SwfRect b = p_bounds;
	if (p_mode == SwfTextAutoSize::None) {
		return b;
	}
	if (!p_wrap) {
		const float w = p_measured.text_width + 2.0f * kSwfTextGutter + p_margins;
		if (p_mode == SwfTextAutoSize::Left) {
			b.x_max = b.x_min + w;
		} else if (p_mode == SwfTextAutoSize::Right) {
			b.x_min = b.x_max - w;
		} else {
			const float c = (p_bounds.x_min + p_bounds.x_max) / 2.0f;
			b.x_min = c - w / 2.0f;
			b.x_max = c + w / 2.0f;
		}
	}
	b.y_max = b.y_min + p_measured.text_height + 2.0f * kSwfTextGutter;
	return b;
}

bool fits(const SwfTextLayout &p_l, const SwfRect &p_bounds) {
	const float w = p_bounds.width() - 2.0f * kSwfTextGutter;
	const float h = p_bounds.height() - 2.0f * kSwfTextGutter;
	return p_l.text_width <= w + 1e-4f && p_l.text_height <= h + 1e-4f;
}

}

double swf_grid_fit_baseline_shift(uint8_t p_grid_fit, double p_window_y, double p_window_per_local_y) {
	if (p_grid_fit != kSwfGridFitPixel || !std::isfinite(p_window_y) || !std::isfinite(p_window_per_local_y) || std::fabs(p_window_per_local_y) < 1e-6) {
		return 0.0;
	}
	return (std::floor(p_window_y) - p_window_y) / p_window_per_local_y;
}

const char *swf_text_auto_size_name(SwfTextAutoSize p_mode) {
	switch (p_mode) {
		case SwfTextAutoSize::None: return "none";
		case SwfTextAutoSize::Left: return "left";
		case SwfTextAutoSize::Center: return "center";
		case SwfTextAutoSize::Right: return "right";
	}
	return "?";
}

const char *swf_text_fit_name(SwfTextFit p_mode) {
	switch (p_mode) {
		case SwfTextFit::None: return "none";
		case SwfTextFit::Shrink: return "shrink";
		case SwfTextFit::Fit: return "fit";
	}
	return "?";
}

SwfTextOptions swf_edit_text_options(const SwfEditText &p_field) {
	SwfTextOptions o;
	if (!p_field.has(SwfEditText::kAutoSize)) {
		return o;
	}
	if (p_field.align == swf_align::kRight) {
		o.auto_size = SwfTextAutoSize::Right;
	} else if (p_field.align == swf_align::kCenter) {
		o.auto_size = SwfTextAutoSize::Center;
	} else {
		o.auto_size = SwfTextAutoSize::Left;
	}
	return o;
}

std::string swf_edit_text_font_name(const SwfMovie &p_movie, const SwfEditText &p_text) {
	if (!p_text.font_class.empty()) {
		return p_text.font_class;
	}
	if (!p_text.has(SwfEditText::kHasFont)) {
		return {};
	}
	if (const SwfFontInfo *f = p_movie.find_font(p_text.font_id)) {
		return f->name;
	}
	const SwfExport *sym = nullptr;
	if (p_movie.find_import(p_text.font_id, &sym) != nullptr && sym != nullptr) {
		return sym->name;
	}
	return {};
}

SwfTextFormat swf_edit_text_format(const SwfMovie &p_movie, const SwfEditText &p_text) {
	SwfTextFormat f;
	f.font = swf_edit_text_font_name(p_movie, p_text);
	f.size = p_text.height;
	f.color = p_text.has(SwfEditText::kHasTextColor) ? p_text.color : SwfColor{ 0, 0, 0, 255 };
	if (p_text.has(SwfEditText::kHasLayout)) {
		f.align = p_text.align;
		f.left_margin = p_text.left_margin;
		f.right_margin = p_text.right_margin;
		f.indent = p_text.indent;
		f.leading = p_text.leading;
	}
	return f;
}

const SwfTextFormat &SwfRichText::format_at(uint32_t p_index, const SwfTextFormat &p_fallback) const {
	auto it = std::upper_bound(runs.begin(), runs.end(), p_index, [](uint32_t i, const SwfTextRun &r) { return i < r.end; });
	if (it != runs.end() && it->begin <= p_index) {
		return it->format;
	}
	return runs.empty() ? p_fallback : runs.back().format;
}

std::u32string swf_utf8_decode(std::string_view p_utf8) {
	std::u32string out;
	out.reserve(p_utf8.size());
	size_t i = 0;
	while (i < p_utf8.size()) {
		const uint8_t c = uint8_t(p_utf8[i]);
		uint32_t cp = 0;
		size_t len = 0;
		if (c < 0x80) {
			cp = c;
			len = 1;
		} else if ((c & 0xe0) == 0xc0) {
			cp = c & 0x1f;
			len = 2;
		} else if ((c & 0xf0) == 0xe0) {
			cp = c & 0x0f;
			len = 3;
		} else if ((c & 0xf8) == 0xf0) {
			cp = c & 0x07;
			len = 4;
		}
		bool ok = len != 0 && i + len <= p_utf8.size();
		for (size_t k = 1; ok && k < len; ++k) {
			const uint8_t cc = uint8_t(p_utf8[i + k]);
			ok = (cc & 0xc0) == 0x80;
			cp = (cp << 6) | (cc & 0x3f);
		}
		static constexpr uint32_t kMin[5] = { 0, 0, 0x80, 0x800, 0x10000 };
		ok = ok && cp >= kMin[len] && cp <= 0x10ffff && (cp < 0xd800 || cp > 0xdfff);
		if (!ok) {
			out.push_back(char32_t(0xfffd));
			i += 1;
			continue;
		}
		out.push_back(char32_t(cp));
		i += len;
	}
	return out;
}

Result<SwfRichText> swf_plain_text(std::string_view p_text, const SwfTextFormat &p_format) {
	SwfRichText out;
	const std::u32string decoded = swf_utf8_decode(p_text);
	if (decoded.size() > kSwfTextMaxChars) {
		return make_error("swf text: more than " + std::to_string(kSwfTextMaxChars) + " characters");
	}
	for (size_t i = 0; i < decoded.size(); ++i) {
		if (decoded[i] == U'\r' && i + 1 < decoded.size() && decoded[i + 1] == U'\n') {
			continue;
		}
		append_char(out, decoded[i] == U'\n' ? U'\r' : decoded[i], p_format);
	}
	return out;
}

Result<SwfRichText> swf_parse_html(std::string_view p_html, const SwfTextFormat &p_base, WorkBudget &r_budget) {
	return HtmlParser(p_html, p_base, r_budget).run();
}

std::optional<uint16_t> SwfFontFace::glyph(char32_t p_code) const {
	if (p_code > 0xffff) {
		return std::nullopt;
	}
	auto it = std::lower_bound(code_to_glyph.begin(), code_to_glyph.end(), uint16_t(p_code),
			[](const std::pair<uint16_t, uint16_t> &e, uint16_t c) { return e.first < c; });
	if (it == code_to_glyph.end() || it->first != p_code) {
		return std::nullopt;
	}
	return it->second;
}

int32_t SwfFontFace::kerning(char32_t p_left, char32_t p_right) const {
	if (p_left > 0xffff || p_right > 0xffff) {
		return 0;
	}
	const uint32_t key = (uint32_t(p_left) << 16) | uint32_t(p_right);
	auto it = std::lower_bound(kerning_sorted.begin(), kerning_sorted.end(), key,
			[](const KerningPair &k, uint32_t v) { return ((uint32_t(k.left) << 16) | k.right) < v; });
	if (it == kerning_sorted.end() || it->left != p_left || it->right != p_right) {
		return 0;
	}
	return it->adjust;
}

SwfFontSet SwfFontSet::from_outlines(FontMappingCollection p_mapping, std::vector<SwfFontOutlines> p_fonts) {
	SwfFontSet set;
	set.mapping_ = std::move(p_mapping);
	for (SwfFontOutlines &o : p_fonts) {
		SwfFontFace face;
		face.long_name = o.name;
		for (size_t g = 0; g < o.codes.size(); ++g) {
			face.code_to_glyph.emplace_back(o.codes[g], uint16_t(g));
		}
		std::stable_sort(face.code_to_glyph.begin(), face.code_to_glyph.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
		face.code_to_glyph.erase(std::unique(face.code_to_glyph.begin(), face.code_to_glyph.end(), [](const auto &a, const auto &b) { return a.first == b.first; }),
				face.code_to_glyph.end());
		face.kerning_sorted = o.kerning;
		std::stable_sort(face.kerning_sorted.begin(), face.kerning_sorted.end(),
				[](const KerningPair &a, const KerningPair &b) { return ((uint32_t(a.left) << 16) | a.right) < ((uint32_t(b.left) << 16) | b.right); });
		face.outlines = std::move(o);
		set.faces_.push_back(std::move(face));
	}
	return set;
}

Result<SwfFontSet> SwfFontSet::load(const SwfLibrary &p_library, const FontMappingCollection &p_mapping, WorkBudget &r_budget) {
	const std::string where = "swf font set " + p_mapping.font_library;
	Result<std::shared_ptr<const SwfMovie>> movie = p_library.load(p_mapping.font_library);
	if (!movie) {
		return error_at(where, movie.error());
	}
	std::vector<SwfFontOutlines> fonts;
	for (const FontMapping &m : p_mapping.fonts) {
		const SwfFontInfo *info = nullptr;
		for (const SwfFontInfo &f : (*movie)->fonts) {
			if (f.name == m.font_long_name && f.kind == SwfFontKind::Outlines) {
				info = &f;
				break;
			}
		}
		if (info == nullptr) {
			return make_error(where + ": no DefineFont3 with outlines named " + m.font_long_name);
		}
		Result<SwfFontOutlines> o = load_font_outlines(**movie, *info, r_budget);
		if (!o) {
			return error_at(where + " " + m.font_long_name, o.error());
		}
		fonts.push_back(std::move(*o));
	}
	return from_outlines(p_mapping, std::move(fonts));
}

const SwfFontFace *SwfFontSet::find(std::string_view p_used_name) const {
	const ResolvedFont r = resolve_font_name(mapping_, p_used_name);
	const std::string_view want = r.font_long_name.empty() ? p_used_name : std::string_view(r.font_long_name);
	for (const SwfFontFace &f : faces_) {
		if (f.long_name == want) {
			return &f;
		}
	}
	return nullptr;
}

Result<SwfTextLayout> swf_layout_text(const SwfFontSet &p_fonts, const SwfEditText &p_field, const SwfRichText &p_text, const SwfTextOptions &p_options,
		WorkBudget &r_budget) {
	if (p_text.text.size() > kSwfTextMaxChars) {
		return make_error("swf text: more than " + std::to_string(kSwfTextMaxChars) + " characters");
	}
	Layouter l(p_fonts, p_field, p_text, r_budget);
	SwfTextFormat base;
	if (!p_text.runs.empty()) {
		base = p_text.runs.front().format;
	}
	l.set_base(base);
	const bool wrap = p_field.has(SwfEditText::kWordWrap);
	SwfTextLayout out;
	Status st = l.layout(1.0f, p_field.bounds, out);
	if (!st) {
		return error_at("swf text layout", st.error());
	}
	if (p_options.fit != SwfTextFit::None && !p_text.text.empty() && out.text_width > 0.0f) {
		const bool overflow = !fits(out, p_field.bounds);
		if (p_options.fit == SwfTextFit::Fit || overflow) {
			float lo = 0.0f;
			float hi = p_options.fit == SwfTextFit::Fit ? kSwfTextFitMaxScale : 1.0f;
			SwfTextLayout best;
			bool have_best = false;
			for (uint32_t step = 0; step < kSwfTextFitSteps; ++step) {
				const float mid = (lo + hi) / 2.0f;
				SwfTextLayout trial;
				st = l.layout(mid, p_field.bounds, trial);
				if (!st) {
					return error_at("swf text layout", st.error());
				}
				if (fits(trial, p_field.bounds)) {
					lo = mid;
					best = std::move(trial);
					have_best = true;
				} else {
					hi = mid;
				}
			}
			if (have_best) {
				out = std::move(best);
			} else {
				st = l.layout(std::max(lo, 1.0f / 1024.0f), p_field.bounds, out);
				if (!st) {
					return error_at("swf text layout", st.error());
				}
			}
		}
	}
	if (p_options.auto_size != SwfTextAutoSize::None) {
		const SwfTextFormat &f = p_text.format_at(0, base);
		const SwfRect sized = auto_sized(p_field.bounds, out, p_options.auto_size, wrap, (f.left_margin + f.right_margin + f.block_indent) * out.scale);
		const float scale = out.scale;
		st = l.layout(scale, sized, out);
		if (!st) {
			return error_at("swf text layout", st.error());
		}
	}
	return out;
}

}
