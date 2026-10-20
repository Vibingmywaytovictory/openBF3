#include "bf3/core/ui/font_ttf.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace bf3::core::ui {

namespace {

constexpr size_t kMaxTables = 64;
constexpr size_t kMaxCmapSegments = 8192;

struct Span {
	const uint8_t *p = nullptr;
	size_t n = 0;
	bool has(size_t p_at, size_t p_len) const { return p_at <= n && p_len <= n - p_at; }
	uint8_t u8(size_t p_at) const { return p[p_at]; }
	uint16_t u16(size_t p_at) const { return uint16_t((p[p_at] << 8) | p[p_at + 1]); }
	int16_t s16(size_t p_at) const { return int16_t(u16(p_at)); }
	uint32_t u32(size_t p_at) const { return (uint32_t(u16(p_at)) << 16) | u16(p_at + 2); }
};

Error tt_error(const std::string &p_what) {
	return make_error("truetype: " + p_what);
}

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

std::string tag_text(uint32_t p_tag) {
	std::string s(4, ' ');
	for (int i = 0; i < 4; ++i) {
		s[size_t(i)] = char((p_tag >> (24 - 8 * i)) & 0xff);
	}
	return s;
}

void append_utf8(std::string &r_out, uint32_t p_cp) {
	if (p_cp < 0x80) {
		r_out.push_back(char(p_cp));
	} else if (p_cp < 0x800) {
		r_out.push_back(char(0xC0 | (p_cp >> 6)));
		r_out.push_back(char(0x80 | (p_cp & 0x3F)));
	} else {
		r_out.push_back(char(0xE0 | (p_cp >> 12)));
		r_out.push_back(char(0x80 | ((p_cp >> 6) & 0x3F)));
		r_out.push_back(char(0x80 | (p_cp & 0x3F)));
	}
}

}

Result<TrueTypeFont> read_truetype(std::span<const uint8_t> p_bytes, WorkBudget &r_budget) {
	const Span f{ p_bytes.data(), p_bytes.size() };
	if (!f.has(0, 12)) {
		return tt_error("file of " + std::to_string(f.n) + " bytes is shorter than the offset table");
	}
	if (f.u32(0) != 0x00010000) {
		return tt_error("sfnt version is not 1.0");
	}
	const size_t num = f.u16(4);
	if (num == 0 || num > kMaxTables) {
		return tt_error(std::to_string(num) + " tables (1.." + std::to_string(kMaxTables) + " allowed)");
	}
	if (!f.has(12, 16 * num)) {
		return tt_error("table directory past the end");
	}
	BF3_TRY(r_budget.charge(num));
	struct Rec {
		uint32_t tag;
		uint32_t sum;
		size_t at;
		size_t len;
	};
	std::vector<Rec> recs;
	TrueTypeFont font;
	for (size_t i = 0; i < num; ++i) {
		const size_t r = 12 + 16 * i;
		Rec rec{ f.u32(r), f.u32(r + 4), f.u32(r + 8), f.u32(r + 12) };
		if (!f.has(rec.at, rec.len)) {
			return tt_error("table " + tag_text(rec.tag) + " outside the file");
		}
		if (i > 0 && rec.tag <= recs.back().tag) {
			return tt_error("table directory not sorted at " + tag_text(rec.tag));
		}
		recs.push_back(rec);
		font.tables.push_back(tag_text(rec.tag));
	}
	BF3_TRY(r_budget.charge(f.n / 64 + 1));
	auto find = [&](const char *p_tag) -> const Rec * {
		uint32_t t = 0;
		for (int i = 0; i < 4; ++i) {
			t = (t << 8) | uint8_t(p_tag[i]);
		}
		for (const Rec &r : recs) {
			if (r.tag == t) {
				return &r;
			}
		}
		return nullptr;
	};

	const Rec *head = find("head");
	if (head == nullptr || head->len < 54) {
		return tt_error("no head table of 54 bytes");
	}
	for (const Rec &r : recs) {
		uint32_t sum = checksum(f.p + r.at, r.len);
		if (&r == head) {
			sum -= f.u32(r.at + 8);
		}
		if (sum != r.sum) {
			return tt_error("table " + tag_text(r.tag) + " checksum mismatch");
		}
	}
	if (0xB1B0AFBAu - (checksum(f.p, f.n) - f.u32(head->at + 8)) != f.u32(head->at + 8)) {
		return tt_error("head.checkSumAdjustment does not match the file");
	}
	if (f.u32(head->at + 12) != 0x5F0F3CF5) {
		return tt_error("head magic");
	}
	font.units_per_em = f.u16(head->at + 18);
	if (font.units_per_em < 16 || font.units_per_em > 16384) {
		return tt_error("unitsPerEm " + std::to_string(font.units_per_em));
	}
	const int16_t loca_format = f.s16(head->at + 50);
	if (loca_format != 0 && loca_format != 1) {
		return tt_error("indexToLocFormat " + std::to_string(loca_format));
	}

	const Rec *maxp = find("maxp");
	if (maxp == nullptr || maxp->len < 6) {
		return tt_error("no maxp table");
	}
	const size_t n_glyphs = f.u16(maxp->at + 4);
	if (n_glyphs == 0) {
		return tt_error("maxp.numGlyphs is 0");
	}
	BF3_TRY(r_budget.charge(n_glyphs));

	const Rec *hhea = find("hhea");
	if (hhea == nullptr || hhea->len < 36) {
		return tt_error("no hhea table");
	}
	font.ascender = f.s16(hhea->at + 4);
	font.descender = f.s16(hhea->at + 6);
	font.line_gap = f.s16(hhea->at + 8);
	const size_t n_metrics = f.u16(hhea->at + 34);
	if (n_metrics == 0 || n_metrics > n_glyphs) {
		return tt_error("hhea.numberOfHMetrics " + std::to_string(n_metrics) + " for " + std::to_string(n_glyphs) + " glyphs");
	}
	const Rec *hmtx = find("hmtx");
	if (hmtx == nullptr || hmtx->len < 4 * n_metrics + 2 * (n_glyphs - n_metrics)) {
		return tt_error("hmtx shorter than its metrics");
	}
	const Rec *loca = find("loca");
	const Rec *glyf = find("glyf");
	const size_t loca_entry = loca_format == 1 ? 4 : 2;
	if (loca == nullptr || glyf == nullptr || loca->len < loca_entry * (n_glyphs + 1)) {
		return tt_error("loca / glyf missing or loca too short");
	}
	font.glyphs.resize(n_glyphs);
	for (size_t g = 0; g < n_glyphs; ++g) {
		TrueTypeGlyph &out = font.glyphs[g];
		if (g < n_metrics) {
			out.advance = f.u16(hmtx->at + 4 * g);
			out.lsb = f.s16(hmtx->at + 4 * g + 2);
		} else {
			out.advance = f.u16(hmtx->at + 4 * (n_metrics - 1));
			out.lsb = f.s16(hmtx->at + 4 * n_metrics + 2 * (g - n_metrics));
		}
		auto loc = [&](size_t i) -> size_t { return loca_format == 1 ? size_t(f.u32(loca->at + 4 * i)) : size_t(f.u16(loca->at + 2 * i)) * 2; };
		const size_t a = loc(g);
		const size_t b = loc(g + 1);
		if (b < a || b > glyf->len) {
			return tt_error("loca not ascending or past glyf at glyph " + std::to_string(g));
		}
		if (b == a) {
			continue;
		}
		const Span gs{ f.p + glyf->at + a, b - a };
		if (!gs.has(0, 10)) {
			return tt_error("glyph " + std::to_string(g) + " header truncated");
		}
		const int16_t contours = gs.s16(0);
		if (contours < 0) {
			return tt_error("glyph " + std::to_string(g) + " is composite");
		}
		out.x_min = gs.s16(2);
		out.y_min = gs.s16(4);
		out.x_max = gs.s16(6);
		out.y_max = gs.s16(8);
		size_t at = 10;
		if (!gs.has(at, 2 * size_t(contours) + 2)) {
			return tt_error("glyph " + std::to_string(g) + " contour table truncated");
		}
		BF3_TRY(r_budget.charge(1 + size_t(contours)));
		uint32_t prev = 0;
		for (int16_t c = 0; c < contours; ++c) {
			const uint16_t e = gs.u16(at);
			if (c > 0 && e <= prev) {
				return tt_error("glyph " + std::to_string(g) + " contour ends not ascending");
			}
			prev = e;
			out.contour_ends.push_back(e);
			at += 2;
		}
		const size_t points = contours == 0 ? 0 : size_t(out.contour_ends.back()) + 1;
		const size_t instr = gs.u16(at);
		at += 2;
		if (!gs.has(at, instr)) {
			return tt_error("glyph " + std::to_string(g) + " instructions truncated");
		}
		at += instr;
		BF3_TRY(r_budget.charge(points));
		std::vector<uint8_t> flags;
		flags.reserve(points);
		while (flags.size() < points) {
			if (!gs.has(at, 1)) {
				return tt_error("glyph " + std::to_string(g) + " flags truncated");
			}
			const uint8_t fl = gs.u8(at++);
			flags.push_back(fl);
			if ((fl & 0x08) != 0) {
				if (!gs.has(at, 1)) {
					return tt_error("glyph " + std::to_string(g) + " flag repeat truncated");
				}
				const size_t rep = gs.u8(at++);
				if (rep > points - flags.size()) {
					return tt_error("glyph " + std::to_string(g) + " flag repeat past the point count");
				}
				flags.insert(flags.end(), rep, fl);
			}
		}
		auto coords = [&](uint8_t p_short, uint8_t p_same, std::vector<int16_t> &r_values) -> Status {
			int32_t v = 0;
			for (size_t i = 0; i < points; ++i) {
				const uint8_t fl = flags[i];
				if ((fl & p_short) != 0) {
					if (!gs.has(at, 1)) {
						return tt_error("glyph " + std::to_string(g) + " coordinates truncated");
					}
					const int32_t d = gs.u8(at++);
					v += (fl & p_same) != 0 ? d : -d;
				} else if ((fl & p_same) == 0) {
					if (!gs.has(at, 2)) {
						return tt_error("glyph " + std::to_string(g) + " coordinates truncated");
					}
					v += gs.s16(at);
					at += 2;
				}
				if (v < -32768 || v > 32767) {
					return tt_error("glyph " + std::to_string(g) + " coordinate outside int16");
				}
				r_values.push_back(int16_t(v));
			}
			return Status::success();
		};
		BF3_TRY(coords(0x02, 0x10, out.xs));
		BF3_TRY(coords(0x04, 0x20, out.ys));
		for (const uint8_t fl : flags) {
			out.on_curve.push_back(fl & 0x01);
		}
	}

	const Rec *cmap = find("cmap");
	if (cmap == nullptr || cmap->len < 4) {
		return tt_error("no cmap table");
	}
	{
		const Span c{ f.p + cmap->at, cmap->len };
		const size_t subtables = c.u16(2);
		if (!c.has(4, 8 * subtables)) {
			return tt_error("cmap directory truncated");
		}
		size_t sub_at = 0;
		bool found = false;
		for (size_t i = 0; i < subtables; ++i) {
			const uint16_t platform = c.u16(4 + 8 * i);
			const uint16_t encoding = c.u16(6 + 8 * i);
			const size_t off = c.u32(8 + 8 * i);
			if ((platform == 3 && encoding == 1) || (!found && platform == 0 && encoding == 3)) {
				sub_at = off;
				found = true;
			}
		}
		if (!found || !c.has(sub_at, 14) || c.u16(sub_at) != 4) {
			return tt_error("no format-4 Unicode cmap subtable");
		}
		const size_t len = c.u16(sub_at + 2);
		if (!c.has(sub_at, len)) {
			return tt_error("cmap subtable past the end");
		}
		const Span s{ c.p + sub_at, len };
		const size_t segs = s.u16(6) / 2;
		if (segs == 0 || segs > kMaxCmapSegments || !s.has(14, 8 * segs + 2)) {
			return tt_error("cmap with " + std::to_string(segs) + " segments");
		}
		BF3_TRY(r_budget.charge(segs));
		const size_t ends = 14;
		const size_t starts = ends + 2 * segs + 2;
		const size_t deltas = starts + 2 * segs;
		const size_t ranges = deltas + 2 * segs;
		uint32_t prev_end = 0;
		for (size_t i = 0; i < segs; ++i) {
			const uint32_t end = s.u16(ends + 2 * i);
			const uint32_t start = s.u16(starts + 2 * i);
			const uint16_t delta = s.u16(deltas + 2 * i);
			const uint16_t range = s.u16(ranges + 2 * i);
			if (start > end || (i > 0 && start <= prev_end)) {
				return tt_error("cmap segments not ascending");
			}
			prev_end = end;
			BF3_TRY(r_budget.charge(end - start + 1));
			for (uint32_t code = start; code <= end; ++code) {
				if (code == 0xFFFF) {
					break;
				}
				uint32_t glyph = 0;
				if (range == 0) {
					glyph = (code + delta) & 0xFFFF;
				} else {
					const size_t at = ranges + 2 * i + range + 2 * (code - start);
					if (!s.has(at, 2)) {
						return tt_error("cmap glyph id array past the end");
					}
					glyph = s.u16(at);
					if (glyph != 0) {
						glyph = (glyph + delta) & 0xFFFF;
					}
				}
				if (glyph == 0) {
					continue;
				}
				if (glyph >= n_glyphs) {
					return tt_error("cmap maps U+" + std::to_string(code) + " to glyph " + std::to_string(glyph) + " of " + std::to_string(n_glyphs));
				}
				font.cmap.push_back({ uint16_t(code), uint16_t(glyph) });
			}
		}
	}

	if (const Rec *kern = find("kern"); kern != nullptr) {
		const Span k{ f.p + kern->at, kern->len };
		if (!k.has(0, 4) || k.u16(0) != 0) {
			return tt_error("kern table is not version 0");
		}
		size_t at = 4;
		const size_t subtables = k.u16(2);
		BF3_TRY(r_budget.charge(subtables));
		for (size_t i = 0; i < subtables; ++i) {
			if (!k.has(at, 14)) {
				return tt_error("kern subtable truncated");
			}
			const size_t len = k.u16(at + 2);
			const uint16_t coverage = k.u16(at + 4);
			const size_t pairs = k.u16(at + 6);
			if (len < 14 || !k.has(at, len) || 14 + 6 * pairs > len) {
				return tt_error("kern subtable past the end");
			}
			if ((coverage >> 8) == 0) {
				BF3_TRY(r_budget.charge(pairs));
				for (size_t p = 0; p < pairs; ++p) {
					const size_t r = at + 14 + 6 * p;
					KerningPair kp;
					kp.left = k.u16(r);
					kp.right = k.u16(r + 2);
					kp.adjust = k.s16(r + 4);
					if (kp.left >= n_glyphs || kp.right >= n_glyphs) {
						return tt_error("kern pair names a glyph past numGlyphs");
					}
					font.kerning.push_back(kp);
				}
			}
			at += len;
		}
	}

	const Rec *name = find("name");
	if (name == nullptr || name->len < 6) {
		return tt_error("no name table");
	}
	{
		const Span n{ f.p + name->at, name->len };
		const size_t count = n.u16(2);
		const size_t strings = n.u16(4);
		if (!n.has(6, 12 * count) || strings > n.n) {
			return tt_error("name records truncated");
		}
		BF3_TRY(r_budget.charge(count));
		for (int pass = 0; pass < 2; ++pass) {
			const uint16_t want_platform = pass == 0 ? 3 : 1;
			for (size_t i = 0; i < count; ++i) {
				const size_t r = 6 + 12 * i;
				const uint16_t platform = n.u16(r);
				const uint16_t id = n.u16(r + 6);
				const size_t len = n.u16(r + 8);
				const size_t off = strings + n.u16(r + 10);
				if (platform != want_platform || (id != 1 && id != 2)) {
					continue;
				}
				if (!n.has(off, len)) {
					return tt_error("name string past the end");
				}
				std::string text;
				if (platform == 3) {
					for (size_t k = 0; k + 1 < len; k += 2) {
						append_utf8(text, n.u16(off + k));
					}
				} else {
					text.assign(reinterpret_cast<const char *>(n.p + off), len);
				}
				std::string &target = id == 1 ? font.family : font.subfamily;
				if (target.empty()) {
					target = text;
				}
			}
		}
	}
	if (font.family.empty()) {
		return tt_error("no family name");
	}
	if (const Rec *os2 = find("OS/2"); os2 != nullptr && os2->len >= 6) {
		font.weight_class = f.u16(os2->at + 4);
	}
	return font;
}

}
