// DefineFont2/3 outlines and winding (bf3/core/ui/swf_font.h; docs/design/ui-swf-fonts.md §7.1, §9). Clean-room from
// the public SWF file format specification (v19) and the retail font libraries.
#include "bf3/core/ui/swf_font.h"

#include "swf_bits.h"
#include "swf_font_detail.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace bf3::core::ui {

namespace {

using detail::SwfReader;

Error font_error(const std::string &p_what, size_t p_at) {
	return make_error("font: " + p_what + " at " + std::to_string(p_at));
}

Status charge(WorkBudget &r_budget, size_t p_units, size_t p_at) {
	Status st = r_budget.charge(p_units);
	if (!st) {
		return font_error("work budget exhausted (" + st.error().message + ")", p_at);
	}
	return Status::success();
}

// One glyph SHAPE in [p_begin, p_end) of the tag.
Status read_glyph(std::span<const uint8_t> p_tag, size_t p_begin, size_t p_end, WorkBudget &r_budget, GlyphOutline &r_glyph) {
	SwfReader r(p_tag.subspan(p_begin, p_end - p_begin), p_begin);
	const uint8_t nb = r.u8();
	unsigned fill_bits = nb >> 4;
	unsigned line_bits = nb & 0x0f;
	int32_t x = 0;
	int32_t y = 0;
	bool open = false; // a contour has been started
	size_t contour_start = 0;
	auto close_contour = [&]() {
		if (!open) {
			return;
		}
		open = false;
		std::vector<GlyphPoint> &pts = r_glyph.points;
		if (pts.size() - contour_start < 2) {
			pts.resize(contour_start); // a lone move-to: no contour
			return;
		}
		// Implicitly closed: drop a repeated start point.
		if (pts.back() == pts[contour_start] && pts.back().on_curve) {
			pts.pop_back();
		}
		if (pts.size() - contour_start < 2) {
			pts.resize(contour_start);
			return;
		}
		r_glyph.contour_ends.push_back(uint32_t(pts.size() - 1));
	};
	auto start = [&]() -> Status {
		close_contour();
		if (r_glyph.contour_ends.size() >= kSwfFontMaxContoursPerGlyph) {
			return font_error("more than " + std::to_string(kSwfFontMaxContoursPerGlyph) + " contours in a glyph", r.abs_pos());
		}
		contour_start = r_glyph.points.size();
		r_glyph.points.push_back({ x, y, true });
		open = true;
		return Status::success();
	};
	auto add = [&](int32_t p_x, int32_t p_y, bool p_on) -> Status {
		if (r_glyph.points.size() >= kSwfFontMaxPointsPerGlyph) {
			return font_error("more than " + std::to_string(kSwfFontMaxPointsPerGlyph) + " points in a glyph", r.abs_pos());
		}
		BF3_TRY(charge(r_budget, 1, r.abs_pos()));
		r_glyph.points.push_back({ p_x, p_y, p_on });
		return Status::success();
	};
	while (!r.failed()) {
		BF3_TRY(charge(r_budget, 1, r.abs_pos()));
		if (r.ub(1) == 0) {
			const uint32_t flags = r.ub(5);
			if (flags == 0) {
				break;
			}
			if ((flags & 0x10) != 0) {
				return font_error("glyph shape with new styles", r.abs_pos());
			}
			if ((flags & 0x01) != 0) {
				const unsigned n = r.ub(5);
				x = r.sb(n);
				y = r.sb(n);
				if (r.failed()) {
					break;
				}
				BF3_TRY(start());
			}
			if ((flags & 0x02) != 0) {
				r.ub(fill_bits);
			}
			if ((flags & 0x04) != 0) {
				r.ub(fill_bits);
			}
			if ((flags & 0x08) != 0) {
				r.ub(line_bits);
			}
		} else {
			const bool straight = r.ub(1) != 0;
			const unsigned n = r.ub(4) + 2;
			int32_t dx = 0;
			int32_t dy = 0;
			int32_t cx = 0;
			int32_t cy = 0;
			if (straight) {
				if (r.ub(1) != 0) {
					dx = r.sb(n);
					dy = r.sb(n);
				} else if (r.ub(1) != 0) {
					dy = r.sb(n);
				} else {
					dx = r.sb(n);
				}
			} else {
				cx = r.sb(n);
				cy = r.sb(n);
				dx = r.sb(n);
				dy = r.sb(n);
			}
			if (r.failed()) {
				break;
			}
			if (!open) {
				BF3_TRY(start());
			}
			if (!straight) {
				x += cx;
				y += cy;
				BF3_TRY(add(x, y, false));
			}
			x += dx;
			y += dy;
			BF3_TRY(add(x, y, true));
			r_glyph.edges++;
		}
	}
	if (r.failed()) {
		return font_error("glyph shape past the end of its slot", r.abs_pos());
	}
	close_contour();
	return Status::success();
}

// Polygon of one contour with each quadratic flattened at 4 steps (doubles, SWF units).
void flatten(const std::vector<GlyphPoint> &p_points, size_t p_first, size_t p_last, std::vector<std::pair<double, double>> &r_poly) {
	r_poly.clear();
	const size_t n = p_last - p_first + 1;
	auto pt = [&](size_t i) -> const GlyphPoint & { return p_points[p_first + (i % n)]; };
	// Start at an on-curve point (an all-off contour starts at the midpoint of its first two points).
	size_t s = 0;
	while (s < n && !pt(s).on_curve) {
		++s;
	}
	double px;
	double py;
	if (s == n) {
		px = (pt(0).x + pt(1).x) / 2.0;
		py = (pt(0).y + pt(1).y) / 2.0;
		s = 0;
	} else {
		px = pt(s).x;
		py = pt(s).y;
		s = s + 1;
	}
	r_poly.push_back({ px, py });
	for (size_t k = 0; k < n; ++k) {
		const GlyphPoint &p = pt(s + k);
		if (p.on_curve) {
			px = p.x;
			py = p.y;
			r_poly.push_back({ px, py });
			continue;
		}
		const GlyphPoint &q = pt(s + k + 1);
		double ex;
		double ey;
		if (q.on_curve) {
			ex = q.x;
			ey = q.y;
			++k;
		} else {
			ex = (p.x + q.x) / 2.0;
			ey = (p.y + q.y) / 2.0;
		}
		for (int step = 1; step <= 4; ++step) {
			const double t = step / 4.0;
			const double u = 1.0 - t;
			r_poly.push_back({ u * u * px + 2 * u * t * p.x + t * t * ex, u * u * py + 2 * u * t * p.y + t * t * ey });
		}
		px = ex;
		py = ey;
	}
}

double shoelace(const std::vector<std::pair<double, double>> &p_poly) {
	double a = 0.0;
	for (size_t i = 0; i < p_poly.size(); ++i) {
		const auto &p = p_poly[i];
		const auto &q = p_poly[(i + 1) % p_poly.size()];
		a += p.first * q.second - q.first * p.second;
	}
	return a / 2.0;
}

bool inside(const std::vector<std::pair<double, double>> &p_poly, double p_x, double p_y) {
	bool in = false;
	for (size_t i = 0, j = p_poly.size() - 1; i < p_poly.size(); j = i++) {
		const auto &a = p_poly[i];
		const auto &b = p_poly[j];
		if ((a.second > p_y) != (b.second > p_y)) {
			const double x = a.first + (p_y - a.second) * (b.first - a.first) / (b.second - a.second);
			if (p_x < x) {
				in = !in;
			}
		}
	}
	return in;
}

} // namespace

namespace detail {

Status read_font_tag(std::span<const uint8_t> p_tag, uint16_t p_tag_code, bool p_glyphs, WorkBudget &r_budget, SwfFontOutlines &r_font) {
	if (p_tag_code != swf_tag::kDefineFont2 && p_tag_code != swf_tag::kDefineFont3) {
		return font_error("tag code " + std::to_string(p_tag_code) + " is not DefineFont2/3", 0);
	}
	SwfReader r(p_tag);
	r_font.id = r.u16();
	const uint8_t flags = r.u8();
	r_font.language = r.u8();
	const std::string_view name = r.pstr8();
	const size_t count = r.u16();
	if (r.failed()) {
		return font_error("header truncated (" + std::to_string(p_tag.size()) + " bytes)", r.pos());
	}
	BF3_TRY(charge(r_budget, level::text_units(name.size()), r.pos()));
	r_font.name = std::string(name);
	r_font.bold = (flags & 0x01) != 0;
	r_font.italic = (flags & 0x02) != 0;
	r_font.small_text = (flags & 0x20) != 0;
	r_font.has_layout = (flags & 0x80) != 0;
	const bool wide_offsets = (flags & 0x08) != 0;
	const bool wide_codes = (flags & 0x04) != 0;
	const size_t off_size = wide_offsets ? 4 : 2;
	const size_t code_size = wide_codes ? 2 : 1;
	const size_t base = r.pos();
	auto read_off = [&]() -> uint32_t { return wide_offsets ? r.u32() : r.u16(); };

	size_t codes_at = 0;
	if (count == 0) {
		r_font.kind = SwfFontKind::NoGlyphs;
		if (r.left() >= off_size) {
			(void)read_off(); // CodeTableOffset
		}
		codes_at = r.pos();
	} else {
		if ((count + 1) * off_size > r.left()) {
			return font_error("offset table of " + std::to_string(count) + " glyphs past the end of the tag", r.pos());
		}
		BF3_TRY(charge(r_budget, count + 1, r.pos()));
		const uint32_t first = read_off();
		if (first == 0) {
			// GFx stripped form: OffsetTable[0] = 0, CodeTableOffset, the codes; no shapes.
			r_font.kind = SwfFontKind::CodesOnly;
			const uint32_t code_off = read_off();
			if (code_off < 2 * off_size || code_off > p_tag.size() - base) {
				return font_error("code table offset " + std::to_string(code_off) + " outside the tag", r.pos());
			}
			codes_at = base + code_off;
		} else {
			r_font.kind = SwfFontKind::Outlines;
			std::vector<uint32_t> offsets(count + 1);
			offsets[0] = first;
			for (size_t i = 1; i <= count; ++i) {
				offsets[i] = read_off(); // offsets[count] = CodeTableOffset
			}
			if (first < (count + 1) * off_size) {
				return font_error("first glyph offset " + std::to_string(first) + " inside the offset table", base);
			}
			for (size_t i = 1; i <= count; ++i) {
				if (offsets[i] < offsets[i - 1]) {
					return font_error("glyph offsets not ascending at glyph " + std::to_string(i), base);
				}
			}
			if (offsets[count] > p_tag.size() - base) {
				return font_error("code table offset " + std::to_string(offsets[count]) + " past the end of the tag", base);
			}
			if (p_glyphs) {
				r_font.glyphs.resize(count);
				for (size_t i = 0; i < count; ++i) {
					const size_t gb = base + offsets[i];
					const size_t ge = base + offsets[i + 1];
					if (ge <= gb) {
						continue; // an empty slot: no shape
					}
					BF3_TRY(read_glyph(p_tag, gb, ge, r_budget, r_font.glyphs[i]));
				}
			}
			codes_at = base + offsets[count];
		}
	}
	r.seek(codes_at);
	if (count * code_size > r.left()) {
		return font_error("code table of " + std::to_string(count) + " codes past the end of the tag", codes_at);
	}
	BF3_TRY(charge(r_budget, count, codes_at));
	r_font.codes.resize(count);
	std::vector<bool> seen(65536, false);
	for (size_t i = 0; i < count; ++i) {
		const uint16_t c = wide_codes ? r.u16() : r.u8();
		r_font.codes[i] = c;
		if (seen[c]) {
			r_font.duplicate_codes++;
		}
		seen[c] = true;
	}
	if (r_font.has_layout) {
		const size_t at = r.pos();
		if (6 + 2 * count > r.left()) {
			return font_error("layout of " + std::to_string(count) + " glyphs past the end of the tag", at);
		}
		r_font.ascent = r.u16();
		r_font.descent = r.u16();
		r_font.leading = r.s16();
		BF3_TRY(charge(r_budget, 2 * count, at));
		if (p_glyphs) {
			r_font.advances.resize(count);
		}
		for (size_t i = 0; i < count; ++i) {
			const int16_t a = r.s16();
			if (p_glyphs) {
				r_font.advances[i] = a;
			}
		}
		if (count > r.left()) {
			return font_error("bounds table of " + std::to_string(count) + " glyphs past the end of the tag", r.pos());
		}
		if (p_glyphs) {
			r_font.bounds.resize(count);
		}
		for (size_t i = 0; i < count && !r.failed(); ++i) {
			r.align();
			const unsigned n = r.ub(5);
			GlyphBounds b;
			b.x_min = r.sb(n);
			b.x_max = r.sb(n);
			b.y_min = r.sb(n);
			b.y_max = r.sb(n);
			r.align();
			if (p_glyphs) {
				r_font.bounds[i] = b;
			}
		}
		const size_t kerning = r.u16();
		const size_t rec = wide_codes ? 6 : 4;
		if (r.failed() || kerning * rec > r.left()) {
			return font_error("kerning table of " + std::to_string(kerning) + " pairs past the end of the tag", r.pos());
		}
		BF3_TRY(charge(r_budget, kerning, r.pos()));
		if (p_glyphs) {
			r_font.kerning.resize(kerning);
		}
		for (size_t i = 0; i < kerning; ++i) {
			KerningPair k;
			k.left = wide_codes ? r.u16() : r.u8();
			k.right = wide_codes ? r.u16() : r.u8();
			k.adjust = r.s16();
			if (p_glyphs) {
				r_font.kerning[i] = k;
			}
		}
	}
	if (r.failed()) {
		return font_error("truncated (" + std::to_string(p_tag.size()) + " bytes)", r.pos());
	}
	r_font.trailing_bytes = uint32_t(r.left());
	return Status::success();
}

} // namespace detail

Result<SwfFontOutlines> parse_font_outlines(std::span<const uint8_t> p_tag, uint16_t p_tag_code, WorkBudget &r_budget) {
	SwfFontOutlines f;
	BF3_TRY(detail::read_font_tag(p_tag, p_tag_code, true, r_budget, f));
	return f;
}

Result<SwfFontOutlines> load_font_outlines(const SwfMovie &p_movie, const SwfFontInfo &p_font, WorkBudget &r_budget) {
	const std::span<const uint8_t> tag = p_movie.font_tag(p_font);
	if (tag.size() != p_font.tag_length) {
		return make_error("font " + p_font.name + ": tag range outside the movie body");
	}
	Result<SwfFontOutlines> f = parse_font_outlines(tag, p_font.tag_code, r_budget);
	if (!f) {
		return error_at("font " + p_font.name, f.error());
	}
	return f;
}

Status normalise_winding(GlyphOutline &r_glyph, WorkBudget &r_budget, WindingStats *r_stats) {
	const size_t contours = r_glyph.contour_ends.size();
	if (contours == 0) {
		return Status::success();
	}
	// The polygons, curves flattened at 4 steps (<= 4 polygon points per stored point).
	std::vector<std::vector<std::pair<double, double>>> polys(contours);
	size_t total = 0;
	size_t first = 0;
	for (size_t c = 0; c < contours; ++c) {
		const size_t last = r_glyph.contour_ends[c];
		if (last < first || last >= r_glyph.points.size()) {
			return make_error("font: contour end " + std::to_string(last) + " outside the glyph's " + std::to_string(r_glyph.points.size()) + " points");
		}
		BF3_TRY(r_budget.charge(4 * (last - first + 1)));
		flatten(r_glyph.points, first, last, polys[c]);
		total += polys[c].size();
		first = last + 1;
	}
	// Point-in-polygon of one point per contour against every other contour: c x total edge tests, charged first.
	BF3_TRY(r_budget.charge(contours * total));
	first = 0;
	for (size_t c = 0; c < contours; ++c) {
		const size_t last = r_glyph.contour_ends[c];
		WindingStats s;
		s.contours = 1;
		const double area = shoelace(polys[c]);
		if (last - first + 1 < 3 || area == 0.0) {
			s.degenerate = 1;
		} else {
			const double px = r_glyph.points[first].x;
			const double py = r_glyph.points[first].y;
			uint32_t depth = 0;
			for (size_t d = 0; d < contours; ++d) {
				if (d != c && polys[d].size() >= 3 && inside(polys[d], px, py)) {
					++depth;
				}
			}
			const bool hole = (depth % 2) == 1;
			s.outer = hole ? 0 : 1;
			s.holes = hole ? 1 : 0;
			const bool want_positive = !hole;
			if ((area > 0.0) != want_positive) {
				// Reverse, keeping the first point first: p0, pn-1, ..., p1.
				std::reverse(r_glyph.points.begin() + first + 1, r_glyph.points.begin() + last + 1);
				s.reversed = 1;
			}
		}
		if (r_stats != nullptr) {
			r_stats->contours += s.contours;
			r_stats->outer += s.outer;
			r_stats->holes += s.holes;
			r_stats->reversed += s.reversed;
			r_stats->degenerate += s.degenerate;
		}
		first = last + 1;
	}
	return Status::success();
}

} // namespace bf3::core::ui
