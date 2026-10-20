#include "bf3/core/ui/font_ttf.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>

namespace bf3::core::ui {

namespace {

constexpr uint32_t tag4(const char *p_tag) {
	return (uint32_t(uint8_t(p_tag[0])) << 24) | (uint32_t(uint8_t(p_tag[1])) << 16) | (uint32_t(uint8_t(p_tag[2])) << 8) | uint32_t(uint8_t(p_tag[3]));
}

struct Be {
	std::vector<uint8_t> b;
	void u8(uint32_t v) { b.push_back(uint8_t(v)); }
	void u16(uint32_t v) {
		b.push_back(uint8_t(v >> 8));
		b.push_back(uint8_t(v));
	}
	void s16(int32_t v) { u16(uint32_t(uint16_t(int16_t(v)))); }
	void u32(uint32_t v) {
		u16(v >> 16);
		u16(v & 0xffff);
	}
	void zeros(size_t n) { b.insert(b.end(), n, 0); }
	void pad4() {
		while (b.size() % 4 != 0) {
			b.push_back(0);
		}
	}
	void put16(size_t at, uint32_t v) {
		b[at] = uint8_t(v >> 8);
		b[at + 1] = uint8_t(v);
	}
	void put32(size_t at, uint32_t v) {
		put16(at, v >> 16);
		put16(at + 2, v & 0xffff);
	}
};

uint32_t checksum(const uint8_t *p_data, size_t p_len) {
	uint32_t sum = 0;
	for (size_t i = 0; i < p_len; i += 4) {
		uint32_t w = 0;
		for (size_t k = 0; k < 4; ++k) {
			w = (w << 8) | (i + k < p_len ? p_data[i + k] : 0);
		}
		sum += w;
	}
	return sum;
}

int32_t scaled(int32_t p_swf, int32_t p_divisor) {
	return int32_t(std::lround(double(p_swf) / double(p_divisor)));
}

struct TtGlyph {
	std::vector<int32_t> xs;
	std::vector<int32_t> ys;
	std::vector<uint8_t> on;
	std::vector<uint32_t> ends;
	int32_t x_min = 0;
	int32_t y_min = 0;
	int32_t x_max = 0;
	int32_t y_max = 0;
	uint16_t advance = 0;
};

struct UnicodeRange {
	uint32_t first;
	uint32_t last;
	uint32_t bit;
};
constexpr UnicodeRange kUnicodeRanges[] = {
	{ 0x0000, 0x007F, 0 }, { 0x0080, 0x00FF, 1 }, { 0x0100, 0x017F, 2 }, { 0x0180, 0x024F, 3 }, { 0x0250, 0x02AF, 4 }, { 0x02B0, 0x02FF, 5 },
	{ 0x0300, 0x036F, 6 }, { 0x0370, 0x03FF, 7 }, { 0x0400, 0x052F, 9 }, { 0x0530, 0x058F, 10 }, { 0x0590, 0x05FF, 11 }, { 0x0600, 0x06FF, 13 },
	{ 0x0E00, 0x0E7F, 24 }, { 0x1E00, 0x1EFF, 29 }, { 0x1F00, 0x1FFF, 30 }, { 0x2000, 0x206F, 31 }, { 0x2070, 0x209F, 32 }, { 0x20A0, 0x20CF, 33 },
	{ 0x2100, 0x214F, 35 }, { 0x2150, 0x218F, 36 }, { 0x2190, 0x21FF, 37 }, { 0x2200, 0x22FF, 38 }, { 0x2300, 0x23FF, 39 }, { 0x2500, 0x257F, 43 },
	{ 0x2580, 0x259F, 44 }, { 0x25A0, 0x25FF, 45 }, { 0x2600, 0x26FF, 46 }, { 0x3000, 0x303F, 48 }, { 0x3040, 0x309F, 49 }, { 0x30A0, 0x30FF, 50 },
	{ 0x3130, 0x318F, 52 }, { 0xAC00, 0xD7AF, 56 }, { 0x4E00, 0x9FFF, 59 }, { 0xF900, 0xFAFF, 61 }, { 0xFF00, 0xFFEF, 68 },
};

bool fits16(int32_t p_v) {
	return p_v >= -32768 && p_v <= 32767;
}

uint16_t weight_class(const std::string &p_name, bool p_bold) {
	std::string l;
	for (const char c : p_name) {
		l.push_back(char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
	}
	if (l.find("semibold") != std::string::npos) {
		return 600;
	}
	if (l.find("medium") != std::string::npos) {
		return 500;
	}
	if (l.find("bold") != std::string::npos || p_bold) {
		return 700;
	}
	return 400;
}

std::string postscript_name(const std::string &p_family) {
	std::string out;
	for (const char c : p_family) {
		const unsigned char u = static_cast<unsigned char>(c);
		if (u > 32 && u < 127 && std::strchr("[](){}<>/%", c) == nullptr) {
			out.push_back(c);
		}
	}
	if (out.size() > 63) {
		out.resize(63);
	}
	return out.empty() ? std::string("openBF3Font") : out;
}

std::vector<uint16_t> utf16(const std::string &p_text) {
	std::vector<uint16_t> out;
	size_t i = 0;
	while (i < p_text.size()) {
		const unsigned char c = static_cast<unsigned char>(p_text[i]);
		uint32_t cp = 0xFFFD;
		size_t len = 1;
		if (c < 0x80) {
			cp = c;
		} else if ((c >> 5) == 6 && i + 1 < p_text.size()) {
			cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(p_text[i + 1]) & 0x3F);
			len = 2;
		} else if ((c >> 4) == 14 && i + 2 < p_text.size()) {
			cp = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(p_text[i + 1]) & 0x3F) << 6) | (static_cast<unsigned char>(p_text[i + 2]) & 0x3F);
			len = 3;
		}
		if (cp > 0xFFFF) {
			cp = 0xFFFD;
		}
		out.push_back(uint16_t(cp));
		i += len;
	}
	return out;
}

}

Result<std::vector<uint8_t>> write_truetype(const SwfFontOutlines &p_font, const TrueTypeOptions &p_options, WorkBudget &r_budget, TrueTypeStats *r_stats) {
	if (p_font.kind != SwfFontKind::Outlines || p_font.glyphs.size() != p_font.codes.size() || p_font.glyphs.empty()) {
		return make_error("truetype " + p_font.name + ": the font has no outlines (" + swf_font_kind_name(p_font.kind) + ")");
	}
	if (p_font.glyphs.size() > 65534) {
		return make_error("truetype " + p_font.name + ": " + std::to_string(p_font.glyphs.size()) + " glyphs exceed 65,534");
	}
	if (p_options.units_per_em == 0 || kSwfFontUnitsPerEm % p_options.units_per_em != 0) {
		return make_error("truetype " + p_font.name + ": unitsPerEm " + std::to_string(p_options.units_per_em) + " does not divide 20480");
	}
	const int32_t div = kSwfFontUnitsPerEm / p_options.units_per_em;
	const int32_t upem = p_options.units_per_em;
	const size_t n_src = p_font.glyphs.size();
	const size_t n_glyphs = n_src + 1;
	BF3_TRY(r_budget.charge(n_glyphs));
	TrueTypeStats stats;
	stats.glyphs = uint32_t(n_glyphs);

	std::vector<TtGlyph> glyphs(n_glyphs);
	{
		TtGlyph &g = glyphs[0];
		const int32_t w = (upem * 6) / 10;
		const int32_t h = (upem * 7) / 10;
		const int32_t t = upem / 20;
		const int32_t m = upem / 40;
		g.advance = uint16_t(w + 2 * m);
		if (p_options.notdef_box) {
			const int32_t outer[4][2] = { { m, 0 }, { m, h }, { m + w, h }, { m + w, 0 } };
			const int32_t inner[4][2] = { { m + t, t }, { m + w - t, t }, { m + w - t, h - t }, { m + t, h - t } };
			for (const auto &p : outer) {
				g.xs.push_back(p[0]);
				g.ys.push_back(p[1]);
				g.on.push_back(1);
			}
			g.ends.push_back(3);
			for (const auto &p : inner) {
				g.xs.push_back(p[0]);
				g.ys.push_back(p[1]);
				g.on.push_back(1);
			}
			g.ends.push_back(7);
		}
	}
	for (size_t i = 0; i < n_src; ++i) {
		GlyphOutline outline = p_font.glyphs[i];
		BF3_TRY(r_budget.charge(1 + outline.points.size()));
		if (outline.points.size() > 65535 || outline.contour_ends.size() > 32767) {
			return make_error("truetype " + p_font.name + ": glyph " + std::to_string(i) + " exceeds the TrueType limits (" +
					std::to_string(outline.points.size()) + " points, " + std::to_string(outline.contour_ends.size()) + " contours)");
		}
		if (p_options.normalise_winding) {
			WindingStats ws;
			Status st = normalise_winding(outline, r_budget, &ws);
			if (!st) {
				return error_at("truetype " + p_font.name + " glyph " + std::to_string(i), st.error());
			}
			stats.contours_reversed += ws.reversed;
		}
		TtGlyph &g = glyphs[i + 1];
		uint32_t prev_end = 0;
		bool first_contour = true;
		for (const uint32_t e : outline.contour_ends) {
			if (e >= outline.points.size() || (!first_contour && e <= prev_end)) {
				return make_error("truetype " + p_font.name + ": glyph " + std::to_string(i) + " has contour ends out of order");
			}
			prev_end = e;
			first_contour = false;
		}
		if (outline.contour_ends.empty() ? !outline.points.empty() : outline.contour_ends.back() + 1 != outline.points.size()) {
			return make_error("truetype " + p_font.name + ": glyph " + std::to_string(i) + " has points after its last contour");
		}
		for (const GlyphPoint &p : outline.points) {
			const int32_t x = scaled(p.x, div);
			const int32_t y = -scaled(p.y, div);
			if (!fits16(x) || !fits16(y)) {
				return make_error("truetype " + p_font.name + ": glyph " + std::to_string(i) + " coordinate (" + std::to_string(x) + ", " + std::to_string(y) +
						") outside int16");
			}
			g.xs.push_back(x);
			g.ys.push_back(y);
			g.on.push_back(p.on_curve ? 1 : 0);
		}
		g.ends = outline.contour_ends;
		const int32_t adv = i < p_font.advances.size() ? scaled(p_font.advances[i], div) : 0;
		g.advance = uint16_t(std::clamp(adv, 0, 65535));
	}

	int32_t hx_min = 32767, hy_min = 32767, hx_max = -32768, hy_max = -32768;
	bool any_points = false;
	uint32_t max_points = 0;
	uint32_t max_contours = 0;
	uint16_t advance_max = 0;
	int32_t min_lsb = 32767, min_rsb = 32767, max_extent = -32768;
	for (TtGlyph &g : glyphs) {
		advance_max = std::max(advance_max, g.advance);
		if (g.xs.empty()) {
			continue;
		}
		g.x_min = *std::min_element(g.xs.begin(), g.xs.end());
		g.x_max = *std::max_element(g.xs.begin(), g.xs.end());
		g.y_min = *std::min_element(g.ys.begin(), g.ys.end());
		g.y_max = *std::max_element(g.ys.begin(), g.ys.end());
		hx_min = std::min(hx_min, g.x_min);
		hy_min = std::min(hy_min, g.y_min);
		hx_max = std::max(hx_max, g.x_max);
		hy_max = std::max(hy_max, g.y_max);
		any_points = true;
		max_points = std::max(max_points, uint32_t(g.xs.size()));
		max_contours = std::max(max_contours, uint32_t(g.ends.size()));
		min_lsb = std::min(min_lsb, g.x_min);
		min_rsb = std::min(min_rsb, int32_t(g.advance) - g.x_max);
		max_extent = std::max(max_extent, g.x_max);
		stats.contours += uint32_t(g.ends.size());
		stats.points += uint32_t(g.xs.size());
	}
	if (!any_points) {
		hx_min = hy_min = hx_max = hy_max = 0;
		min_lsb = min_rsb = max_extent = 0;
	}
	stats.max_points = max_points;
	stats.max_contours = max_contours;

	std::vector<std::pair<uint16_t, uint16_t>> cmap;
	{
		std::vector<int32_t> first(65536, -1);
		BF3_TRY(r_budget.charge(n_src));
		for (size_t i = 0; i < n_src; ++i) {
			const uint16_t c = p_font.codes[i];
			if (first[c] < 0) {
				first[c] = int32_t(i + 1);
			} else {
				stats.duplicate_codes++;
			}
		}
		for (uint32_t c = 0; c < 0xFFFF; ++c) {
			if (first[c] >= 0) {
				cmap.push_back({ uint16_t(c), uint16_t(first[c]) });
			}
		}
	}
	stats.cmap_codes = uint32_t(cmap.size());
	auto glyph_of = [&](uint16_t p_code) -> int32_t {
		auto it = std::lower_bound(cmap.begin(), cmap.end(), p_code, [](const auto &e, uint16_t c) { return e.first < c; });
		return (it != cmap.end() && it->first == p_code) ? int32_t(it->second) : -1;
	};

	std::vector<std::pair<uint32_t, int32_t>> kern;
	if (p_options.kerning) {
		BF3_TRY(r_budget.charge(p_font.kerning.size()));
		for (const KerningPair &k : p_font.kerning) {
			const int32_t l = glyph_of(k.left);
			const int32_t r = glyph_of(k.right);
			const int32_t v = scaled(k.adjust, div);
			if (l < 0 || r < 0 || v == 0) {
				continue;
			}
			kern.push_back({ (uint32_t(l) << 16) | uint32_t(r), v });
		}
		std::stable_sort(kern.begin(), kern.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
		kern.erase(std::unique(kern.begin(), kern.end(), [](const auto &a, const auto &b) { return a.first == b.first; }), kern.end());
		if (kern.size() > 10920) {
			return make_error("truetype " + p_font.name + ": " + std::to_string(kern.size()) + " kerning pairs exceed one kern subtable");
		}
	}
	stats.kerning_pairs = uint32_t(kern.size());

	const std::string family = p_options.family.empty() ? p_font.name : p_options.family;
	const std::string subfamily = p_options.subfamily.empty() ? std::string(p_font.bold ? "Bold" : "Regular") : p_options.subfamily;
	const int32_t ascender = scaled(p_font.ascent, div);
	const int32_t descender = -scaled(p_font.descent, div);
	const int32_t line_gap = scaled(p_font.leading, div);
	if (!fits16(ascender) || !fits16(descender) || !fits16(line_gap)) {
		return make_error("truetype " + p_font.name + ": metrics outside int16");
	}

	struct Table {
		uint32_t tag;
		Be data;
	};
	std::vector<Table> tables;

	Be glyf;
	Be loca;
	for (const TtGlyph &g : glyphs) {
		loca.u32(uint32_t(glyf.b.size()));
		if (g.ends.empty()) {
			continue;
		}
		glyf.s16(int32_t(g.ends.size()));
		glyf.s16(g.x_min);
		glyf.s16(g.y_min);
		glyf.s16(g.x_max);
		glyf.s16(g.y_max);
		for (const uint32_t e : g.ends) {
			glyf.u16(e);
		}
		glyf.u16(0);
		std::vector<uint8_t> flags;
		Be xb;
		Be yb;
		int32_t px = 0;
		int32_t py = 0;
		for (size_t i = 0; i < g.xs.size(); ++i) {
			const int32_t dx = g.xs[i] - px;
			const int32_t dy = g.ys[i] - py;
			if (!fits16(dx) || !fits16(dy)) {
				return make_error("truetype " + p_font.name + ": a coordinate step outside int16");
			}
			px = g.xs[i];
			py = g.ys[i];
			uint8_t f = g.on[i] ? 0x01 : 0x00;
			if (dx == 0) {
				f |= 0x10;
			} else if (dx >= -255 && dx <= 255) {
				f |= 0x02;
				if (dx > 0) {
					f |= 0x10;
				}
				xb.u8(uint32_t(std::abs(dx)));
			} else {
				xb.s16(dx);
			}
			if (dy == 0) {
				f |= 0x20;
			} else if (dy >= -255 && dy <= 255) {
				f |= 0x04;
				if (dy > 0) {
					f |= 0x20;
				}
				yb.u8(uint32_t(std::abs(dy)));
			} else {
				yb.s16(dy);
			}
			flags.push_back(f);
		}
		for (size_t i = 0; i < flags.size();) {
			size_t run = 1;
			while (i + run < flags.size() && flags[i + run] == flags[i] && run < 256) {
				++run;
			}
			if (run > 1) {
				glyf.u8(flags[i] | 0x08);
				glyf.u8(uint32_t(run - 1));
			} else {
				glyf.u8(flags[i]);
			}
			i += run;
		}
		glyf.b.insert(glyf.b.end(), xb.b.begin(), xb.b.end());
		glyf.b.insert(glyf.b.end(), yb.b.begin(), yb.b.end());
		glyf.pad4();
	}
	loca.u32(uint32_t(glyf.b.size()));
	BF3_TRY(r_budget.charge(glyf.b.size() / 16 + 1));

	Be head;
	head.u32(0x00010000);
	head.u32(0x00010000);
	head.u32(0);
	head.u32(0x5F0F3CF5);
	head.u16(0x0003);
	head.u16(uint32_t(upem));
	head.zeros(16);
	head.s16(hx_min);
	head.s16(hy_min);
	head.s16(hx_max);
	head.s16(hy_max);
	head.u16((p_font.bold ? 0x01 : 0) | (p_font.italic ? 0x02 : 0));
	head.u16(8);
	head.s16(2);
	head.s16(1);
	head.s16(0);

	Be hhea;
	hhea.u32(0x00010000);
	hhea.s16(ascender);
	hhea.s16(descender);
	hhea.s16(line_gap);
	hhea.u16(advance_max);
	hhea.s16(min_lsb);
	hhea.s16(min_rsb);
	hhea.s16(max_extent);
	hhea.s16(1);
	hhea.s16(0);
	hhea.s16(0);
	hhea.zeros(8);
	hhea.s16(0);
	hhea.u16(uint32_t(n_glyphs));

	Be hmtx;
	for (const TtGlyph &g : glyphs) {
		hmtx.u16(g.advance);
		hmtx.s16(g.xs.empty() ? 0 : g.x_min);
	}

	Be maxp;
	maxp.u32(0x00010000);
	maxp.u16(uint32_t(n_glyphs));
	maxp.u16(max_points);
	maxp.u16(max_contours);
	maxp.u16(0);
	maxp.u16(0);
	maxp.u16(2);
	maxp.zeros(16);

	Be os2;
	{
		std::array<uint32_t, 4> ranges{};
		uint32_t code_pages = 0;
		for (const auto &[code, glyph] : cmap) {
			for (const UnicodeRange &u : kUnicodeRanges) {
				if (code >= u.first && code <= u.last) {
					ranges[u.bit / 32] |= 1u << (u.bit % 32);
				}
			}
			if (code >= 0x00C0 && code <= 0x00FF) {
				code_pages |= 1u << 0;
			}
			if (code >= 0x0100 && code <= 0x017F) {
				code_pages |= 1u << 1;
			}
			if (code >= 0x0410 && code <= 0x044F) {
				code_pages |= 1u << 2;
			}
		}
		if (code_pages == 0) {
			code_pages = 1;
		}
		uint64_t adv_sum = 0;
		uint32_t adv_n = 0;
		for (const TtGlyph &g : glyphs) {
			if (g.advance != 0) {
				adv_sum += g.advance;
				++adv_n;
			}
		}
		auto glyph_top = [&](uint16_t p_code) -> int32_t {
			const int32_t g = glyph_of(p_code);
			return (g > 0 && !glyphs[size_t(g)].xs.empty()) ? glyphs[size_t(g)].y_max : 0;
		};
		const uint16_t first_char = cmap.empty() ? 0 : cmap.front().first;
		const uint16_t last_char = cmap.empty() ? 0 : cmap.back().first;
		uint32_t selection = 0x80;
		selection |= p_font.bold ? 0x20 : 0x40;
		if (p_font.italic) {
			selection |= 0x01;
			selection &= ~0x40u;
		}
		os2.u16(4);
		os2.s16(adv_n ? int32_t(adv_sum / adv_n) : 0);
		os2.u16(weight_class(family, p_font.bold));
		os2.u16(5);
		os2.u16(0);
		os2.s16(upem * 65 / 100);
		os2.s16(upem * 60 / 100);
		os2.s16(0);
		os2.s16(upem * 75 / 1000);
		os2.s16(upem * 65 / 100);
		os2.s16(upem * 60 / 100);
		os2.s16(0);
		os2.s16(upem * 35 / 100);
		os2.s16(upem * 5 / 100);
		os2.s16(upem * 25 / 100);
		os2.s16(0);
		os2.zeros(10);
		for (const uint32_t r : ranges) {
			os2.u32(r);
		}
		os2.u8('o');
		os2.u8('B');
		os2.u8('F');
		os2.u8('3');
		os2.u16(selection);
		os2.u16(first_char);
		os2.u16(last_char);
		os2.s16(ascender);
		os2.s16(descender);
		os2.s16(line_gap);
		os2.u16(uint32_t(std::max(0, ascender)));
		os2.u16(uint32_t(std::max(0, -descender)));
		os2.u32(code_pages);
		os2.u32(0);
		os2.s16(glyph_top('x'));
		os2.s16(glyph_top('H'));
		os2.u16(0);
		os2.u16(0x20);
		os2.u16(kern.empty() ? 1 : 2);
	}

	Be cmap_t;
	{
		struct Seg {
			uint16_t start, end;
			int32_t delta;
		};
		std::vector<Seg> segs;
		for (const auto &[code, glyph] : cmap) {
			const int32_t d = int32_t(glyph) - int32_t(code);
			if (!segs.empty() && segs.back().end + 1 == code && segs.back().delta == d) {
				segs.back().end = code;
			} else {
				segs.push_back({ code, code, d });
			}
		}
		segs.push_back({ 0xFFFF, 0xFFFF, 1 });
		if (segs.size() > 8190) {
			return make_error("truetype " + p_font.name + ": " + std::to_string(segs.size()) + " cmap segments exceed one format-4 subtable");
		}
		const uint32_t seg_count = uint32_t(segs.size());
		uint32_t search = 1;
		uint32_t selector = 0;
		while (search * 2 <= seg_count) {
			search *= 2;
			++selector;
		}
		Be sub;
		sub.u16(4);
		sub.u16(0);
		sub.u16(0);
		sub.u16(seg_count * 2);
		sub.u16(search * 2);
		sub.u16(selector);
		sub.u16(seg_count * 2 - search * 2);
		for (const Seg &s : segs) {
			sub.u16(s.end);
		}
		sub.u16(0);
		for (const Seg &s : segs) {
			sub.u16(s.start);
		}
		for (const Seg &s : segs) {
			sub.u16(uint32_t(s.delta) & 0xFFFF);
		}
		for (size_t i = 0; i < segs.size(); ++i) {
			sub.u16(0);
		}
		sub.put16(2, uint32_t(sub.b.size()));
		cmap_t.u16(0);
		cmap_t.u16(2);
		cmap_t.u16(0);
		cmap_t.u16(3);
		cmap_t.u32(20);
		cmap_t.u16(3);
		cmap_t.u16(1);
		cmap_t.u32(20);
		cmap_t.b.insert(cmap_t.b.end(), sub.b.begin(), sub.b.end());
	}

	Be kern_t;
	if (!kern.empty()) {
		const uint32_t n = uint32_t(kern.size());
		uint32_t search = 1;
		uint32_t selector = 0;
		while (search * 2 <= n) {
			search *= 2;
			++selector;
		}
		kern_t.u16(0);
		kern_t.u16(1);
		kern_t.u16(0);
		kern_t.u16(14 + 6 * n);
		kern_t.u16(0x0001);
		kern_t.u16(n);
		kern_t.u16(search * 6);
		kern_t.u16(selector);
		kern_t.u16((n - search) * 6);
		for (const auto &[pair, value] : kern) {
			kern_t.u16(pair >> 16);
			kern_t.u16(pair & 0xFFFF);
			kern_t.s16(value);
		}
	}

	Be name;
	{
		const std::string full = subfamily == "Regular" ? family : family + " " + subfamily;
		const std::string unique = family + " " + subfamily + "; openBF3 ui1 TrueType v" + std::to_string(kTrueTypeWriterVersion);
		const std::string ps = postscript_name(family);
		const std::pair<uint16_t, std::string> ids[] = { { uint16_t(1), family }, { uint16_t(2), subfamily }, { uint16_t(3), unique }, { uint16_t(4), full }, { uint16_t(6), ps } };
		Be strings;
		struct Rec {
			uint16_t platform, encoding, language, id, length, offset;
		};
		std::vector<Rec> recs;
		for (const auto &[id, text] : ids) {
			std::string roman;
			for (const char c : text) {
				roman.push_back(static_cast<unsigned char>(c) < 0x80 ? c : '?');
			}
			recs.push_back({ 1, 0, 0, id, uint16_t(roman.size()), uint16_t(strings.b.size()) });
			strings.b.insert(strings.b.end(), roman.begin(), roman.end());
		}
		for (const auto &[id, text] : ids) {
			const std::vector<uint16_t> u = utf16(text);
			recs.push_back({ 3, 1, 0x0409, id, uint16_t(u.size() * 2), uint16_t(strings.b.size()) });
			for (const uint16_t c : u) {
				strings.u16(c);
			}
		}
		if (strings.b.size() > 65535) {
			return make_error("truetype " + p_font.name + ": name table too long");
		}
		name.u16(0);
		name.u16(uint32_t(recs.size()));
		name.u16(uint32_t(6 + 12 * recs.size()));
		for (const Rec &r : recs) {
			name.u16(r.platform);
			name.u16(r.encoding);
			name.u16(r.language);
			name.u16(r.id);
			name.u16(r.length);
			name.u16(r.offset);
		}
		name.b.insert(name.b.end(), strings.b.begin(), strings.b.end());
	}

	Be post;
	post.u32(0x00030000);
	post.u32(0);
	post.s16(-upem / 10);
	post.s16(upem / 20);
	post.u32(0);
	post.zeros(16);

	tables.push_back({ tag4("OS/2"), std::move(os2) });
	tables.push_back({ tag4("cmap"), std::move(cmap_t) });
	tables.push_back({ tag4("glyf"), std::move(glyf) });
	tables.push_back({ tag4("head"), std::move(head) });
	tables.push_back({ tag4("hhea"), std::move(hhea) });
	tables.push_back({ tag4("hmtx"), std::move(hmtx) });
	if (!kern.empty()) {
		tables.push_back({ tag4("kern"), std::move(kern_t) });
	}
	tables.push_back({ tag4("loca"), std::move(loca) });
	tables.push_back({ tag4("maxp"), std::move(maxp) });
	tables.push_back({ tag4("name"), std::move(name) });
	tables.push_back({ tag4("post"), std::move(post) });

	const uint32_t num = uint32_t(tables.size());
	uint32_t search = 1;
	uint32_t selector = 0;
	while (search * 2 <= num) {
		search *= 2;
		++selector;
	}
	Be out;
	out.u32(0x00010000);
	out.u16(num);
	out.u16(search * 16);
	out.u16(selector);
	out.u16(num * 16 - search * 16);
	const size_t dir_at = out.b.size();
	out.zeros(16 * num);
	size_t head_at = 0;
	for (uint32_t i = 0; i < num; ++i) {
		Table &t = tables[i];
		const size_t at = out.b.size();
		const size_t len = t.data.b.size();
		out.b.insert(out.b.end(), t.data.b.begin(), t.data.b.end());
		out.pad4();
		if (t.tag == tag4("head")) {
			head_at = at;
		}
		const size_t rec = dir_at + 16 * i;
		out.put32(rec, t.tag);
		out.put32(rec + 4, checksum(out.b.data() + at, len));
		out.put32(rec + 8, uint32_t(at));
		out.put32(rec + 12, uint32_t(len));
	}
	out.put32(head_at + 8, 0xB1B0AFBAu - checksum(out.b.data(), out.b.size()));
	stats.bytes = out.b.size();
	if (r_stats != nullptr) {
		*r_stats = stats;
	}
	return std::move(out.b);
}

}
