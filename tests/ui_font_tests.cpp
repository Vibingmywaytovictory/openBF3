#include <doctest/doctest.h>

#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/cache/entry_format.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/ui/font_ttf.h"
#include "bf3/core/ui/swf_font.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/ui/swf_movie.h"
#include "bf3/core/ui/ui_fonts.h"
#include "test_support.h"
#include "ui_swf_writer.h"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;
using namespace ui1test;

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

void hostile_font(const char *p_name, const std::function<void()> &p_fn) {
	const auto t0 = Clock::now();
	p_fn();
	const double s = std::chrono::duration<double>(Clock::now() - t0).count();
	MESSAGE(std::string(p_name) << ": " << s << " s" << std::string(kAsan ? " (asan, reported)" : ""));
	if (!kAsan) {
		CHECK_MESSAGE(s < 1.0, p_name);
	}
}

Contour box(int32_t x0, int32_t y0, int32_t x1, int32_t y1, bool p_outer) {

	Contour c{ x0, y0, {} };
	if (p_outer) {
		c.segs = { { x1, y0 }, { x1, y1 }, { x0, y1 }, { x0, y0 } };
	} else {
		c.segs = { { x0, y1 }, { x1, y1 }, { x1, y0 }, { x0, y0 } };
	}
	return c;
}

Font3 synth_font() {
	Font3 f;
	f.id = 7;
	f.name = "Synth Sans Medium";
	const Glyph square = { box(1000, -10000, 9000, 0, true) };
	const Glyph ring = { box(1000, -12000, 11000, 0, true), box(3000, -9000, 9000, -3000, false) };
	Contour hump{ 0, 0, {} };
	Seg curve{ 10000, 0, true, 5000, -10000 };
	hump.segs = { curve, { 0, 0 } };
	f.glyphs = { square, ring, { hump }, square };
	f.codes = { 0x41, 0x4F, 0x43, 0x41 };
	f.advances = { 10245, 12000, 10000, 10245 };
	f.kerning = { { 0x41, 0x4F, -1229 }, { 0x4F, 0x43, 300 } };
	return f;
}

Bytes font_movie(const Font3 &p_font) {
	return movie(cat({ define_font3(p_font), export_assets({ { p_font.id, p_font.name } }), show_frame(), end_tag() }));
}

SwfFontOutlines outlines_of(const Font3 &p_font) {
	Result<SwfMovie> m = SwfMovie::parse(font_movie(p_font));
	REQUIRE_MESSAGE(m.ok(), (m.ok() ? std::string() : m.error().message));
	REQUIRE(m->fonts.size() == 1);
	WorkBudget b(kUiFontConvertBudgetUnits);
	Result<SwfFontOutlines> o = load_font_outlines(*m, m->fonts[0], b);
	REQUIRE_MESSAGE(o.ok(), (o.ok() ? std::string() : o.error().message));
	return std::move(*o);
}

std::vector<uint8_t> ttf_of(const SwfFontOutlines &p_font, TrueTypeStats *r_stats = nullptr, const TrueTypeOptions &p_options = {}) {
	WorkBudget b(kUiFontConvertBudgetUnits);
	Result<std::vector<uint8_t>> t = write_truetype(p_font, p_options, b, r_stats);
	REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
	return std::move(*t);
}

TrueTypeFont read_ok(const std::vector<uint8_t> &p_ttf) {
	WorkBudget b(kTrueTypeReadBudgetUnits);
	Result<TrueTypeFont> f = read_truetype(p_ttf, b);
	REQUIRE_MESSAGE(f.ok(), (f.ok() ? std::string() : f.error().message));
	return std::move(*f);
}

int32_t scaled10(int32_t p_v) {
	return int32_t(std::lround(double(p_v) / 10.0));
}

void check_round_trip(const SwfFontOutlines &p_src, const TrueTypeFont &p_ttf) {
	REQUIRE(p_ttf.glyphs.size() == p_src.glyphs.size() + 1);
	size_t worst_points = 0;
	for (size_t i = 0; i < p_src.glyphs.size(); ++i) {
		CAPTURE(i);
		const GlyphOutline &s = p_src.glyphs[i];
		const TrueTypeGlyph &g = p_ttf.glyphs[i + 1];
		REQUIRE(g.xs.size() == s.points.size());
		CHECK((std::vector<uint32_t>(g.contour_ends.begin(), g.contour_ends.end()) == s.contour_ends));
		for (size_t k = 0; k < s.points.size(); ++k) {
			const double dx = std::fabs(double(g.xs[k]) - double(s.points[k].x) / 10.0);
			const double dy = std::fabs(double(g.ys[k]) + double(s.points[k].y) / 10.0);
			if (dx > 0.5 || dy > 0.5 || (g.on_curve[k] != 0) != s.points[k].on_curve) {
				++worst_points;
			}
		}
		if (i < p_src.advances.size()) {
			CHECK(int32_t(g.advance) == std::max(0, scaled10(p_src.advances[i])));
		}
	}
	CHECK(worst_points == 0);
}

struct ScratchRoot {
	fs::path path;
	ScratchRoot() {
		wchar_t tmp[MAX_PATH];
		GetTempPathW(MAX_PATH, tmp);
		path = fs::path(tmp) / (L"openbf3-ui1-fonts-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
		std::error_code ec;
		fs::create_directories(path, ec);
	}
	~ScratchRoot() {
		std::error_code ec;
		fs::remove_all(path, ec);
	}
};

}

TEST_SUITE("ui-font-unit") {
	TEST_CASE("font: DefineFont3 outlines read back as written") {
		const SwfFontOutlines o = outlines_of(synth_font());
		CHECK(o.kind == SwfFontKind::Outlines);
		CHECK(o.name == "Synth Sans Medium");
		CHECK(o.has_layout);
		CHECK(o.ascent == 22240);
		CHECK(o.descent == 4320);
		CHECK((o.codes == std::vector<uint16_t>{ 0x41, 0x4F, 0x43, 0x41 }));
		CHECK(o.duplicate_codes == 1);
		REQUIRE(o.glyphs.size() == 4);

		CHECK(o.glyphs[0].points.size() == 4);
		CHECK(o.glyphs[0].contour_ends == std::vector<uint32_t>{ 3 });
		CHECK((o.glyphs[0].points[0] == GlyphPoint{ 1000, -10000, true }));
		CHECK((o.glyphs[0].points[2] == GlyphPoint{ 9000, 0, true }));
		CHECK(o.glyphs[0].edges == 4);
		CHECK((o.glyphs[1].contour_ends == std::vector<uint32_t>{ 3, 7 }));

		REQUIRE(o.glyphs[2].points.size() == 3);
		CHECK((o.glyphs[2].points[1] == GlyphPoint{ 5000, -10000, false }));
		CHECK((o.glyphs[2].points[2] == GlyphPoint{ 10000, 0, true }));
		CHECK((o.advances == std::vector<int16_t>{ 10245, 12000, 10000, 10245 }));
		REQUIRE(o.kerning.size() == 2);
		CHECK(o.kerning[0].left == 0x41);
		CHECK(o.kerning[0].adjust == -1229);
		CHECK(o.trailing_bytes == 0);
	}

	TEST_CASE("font: TrueType round trip (points, flags, advances, cmap, kern, name, checksums)") {
		const SwfFontOutlines o = outlines_of(synth_font());
		TrueTypeStats stats;
		const std::vector<uint8_t> ttf = ttf_of(o, &stats);
		CHECK(stats.glyphs == 5);
		CHECK(stats.cmap_codes == 3);
		CHECK(stats.duplicate_codes == 1);
		CHECK(stats.contours == 1 + 2 + 1 + 1 + 2);
		CHECK(stats.contours_reversed == 0);
		CHECK(stats.kerning_pairs == 2);
		CHECK(stats.bytes == ttf.size());
		const TrueTypeFont f = read_ok(ttf);
		CHECK(f.units_per_em == 2048);
		CHECK(f.ascender == 2224);
		CHECK(f.descender == -432);
		CHECK(f.line_gap == 0);
		CHECK(f.weight_class == 500);
		CHECK(f.family == "Synth Sans Medium");
		CHECK(f.subfamily == "Regular");
		CHECK((f.tables == std::vector<std::string>{ "OS/2", "cmap", "glyf", "head", "hhea", "hmtx", "kern", "loca", "maxp", "name", "post" }));

		CHECK((f.cmap == std::vector<std::pair<uint16_t, uint16_t>>{ { 0x41, 1 }, { 0x43, 3 }, { 0x4F, 2 } }));
		REQUIRE(f.glyphs.size() == 5);
		CHECK(f.glyphs[0].contour_ends.size() == 2);
		check_round_trip(o, f);

		CHECK(f.glyphs[1].xs[0] == 100);
		CHECK(f.glyphs[1].ys[0] == 1000);
		CHECK(f.glyphs[1].x_min == 100);
		CHECK(f.glyphs[1].y_max == 1000);
		CHECK(f.glyphs[1].lsb == 100);
		CHECK((f.glyphs[3].on_curve == std::vector<uint8_t>{ 1, 0, 1 }));
		CHECK(f.glyphs[1].advance == 1025);

		REQUIRE(f.kerning.size() == 2);
		CHECK(f.kerning[0].left == 1);
		CHECK(f.kerning[0].right == 2);
		CHECK(f.kerning[0].adjust == -123);
		CHECK(f.kerning[1].left == 2);
		CHECK(f.kerning[1].right == 3);
		CHECK(f.kerning[1].adjust == 30);

		TrueTypeOptions plain;
		plain.kerning = false;
		plain.notdef_box = false;
		plain.family = "Other Family";
		const TrueTypeFont g = read_ok(ttf_of(o, nullptr, plain));
		CHECK(g.kerning.empty());
		CHECK(g.glyphs[0].contour_ends.empty());
		CHECK(g.family == "Other Family");
		Font3 bold = synth_font();
		bold.name = "Synth Sans Bold";
		bold.bold = true;
		CHECK(read_ok(ttf_of(outlines_of(bold))).weight_class == 700);
		CHECK(read_ok(ttf_of(outlines_of(bold))).subfamily == "Bold");
	}

	TEST_CASE("font: deterministic bytes") {
		const SwfFontOutlines o = outlines_of(synth_font());
		const std::vector<uint8_t> a = ttf_of(o);
		const std::vector<uint8_t> b = ttf_of(o);
		CHECK(a == b);
		TrueTypeOptions no_kern;
		no_kern.kerning = false;
		CHECK(ttf_of(o, nullptr, no_kern) != a);
	}

	TEST_CASE("font: winding by nesting depth") {

		GlyphOutline g;
		auto add = [&](std::vector<GlyphPoint> p_points) {
			for (const GlyphPoint &p : p_points) {
				g.points.push_back(p);
			}
			g.contour_ends.push_back(uint32_t(g.points.size() - 1));
		};
		add({ { 0, 0 }, { 10000, 0 }, { 10000, -10000 }, { 0, -10000 } });
		add({ { 2000, -8000 }, { 8000, -8000 }, { 8000, -2000 }, { 2000, -2000 } });
		add({ { 4000, -6000 }, { 6000, -6000 }, { 6000, -4000 }, { 4000, -4000 } });
		add({ { 20000, 0 }, { 21000, 0 } });
		const GlyphOutline before = g;
		WorkBudget b(1u << 20);
		WindingStats s;
		REQUIRE(normalise_winding(g, b, &s).ok());
		CHECK(s.contours == 4);
		CHECK(s.outer == 2);
		CHECK(s.holes == 1);
		CHECK(s.reversed == 2);
		CHECK(s.degenerate == 1);

		CHECK((g.points[0] == before.points[0]));
		CHECK((g.points[1] == before.points[3]));
		CHECK((g.points[3] == before.points[1]));
		CHECK((g.points[4] == before.points[4]));
		CHECK((g.points[5] == before.points[7]));
		CHECK((g.points[8] == before.points[8]));
		CHECK((g.points[9] == before.points[9]));

		WindingStats again;
		REQUIRE(normalise_winding(g, b, &again).ok());
		CHECK(again.reversed == 0);

		Font3 f = synth_font();
		f.glyphs[1] = { box(1000, -12000, 11000, 0, true), box(3000, -9000, 9000, -3000, true) };
		TrueTypeStats stats;
		(void)ttf_of(outlines_of(f), &stats);
		CHECK(stats.contours_reversed == 1);

		GlyphOutline bad;
		bad.points = { { 0, 0 }, { 1, 0 }, { 1, 1 } };
		bad.contour_ends = { 5 };
		CHECK_FALSE(normalise_winding(bad, b).ok());
	}

	TEST_CASE("font: TrueType limits are refused") {
		SwfFontOutlines base = outlines_of(synth_font());
		TrueTypeOptions raw;
		raw.normalise_winding = false;
		WorkBudget b(size_t(1) << 26);
		{
			SwfFontOutlines f = base;
			GlyphOutline &g = f.glyphs[0];
			g.points.clear();
			for (uint32_t i = 0; i < 65536; ++i) {
				g.points.push_back({ int32_t(i % 1000), -int32_t(i % 777), true });
			}
			g.contour_ends = { 65535 };
			Result<std::vector<uint8_t>> t = write_truetype(f, raw, b);
			REQUIRE_FALSE(t.ok());
			CHECK(t.error().message.find("TrueType limits") != std::string::npos);
		}
		{
			SwfFontOutlines f = base;
			f.glyphs[0].points[1].x = 400000;
			Result<std::vector<uint8_t>> t = write_truetype(f, raw, b);
			REQUIRE_FALSE(t.ok());
			CHECK(t.error().message.find("int16") != std::string::npos);
		}
		{
			SwfFontOutlines f = base;
			f.glyphs[0].contour_ends = { 1, 0 };
			CHECK_FALSE(write_truetype(f, raw, b).ok());
			f.glyphs[0].contour_ends = { 2 };
			CHECK_FALSE(write_truetype(f, raw, b).ok());
		}
		{
			SwfFontOutlines f = base;
			f.kind = SwfFontKind::NoGlyphs;
			CHECK_FALSE(write_truetype(f, raw, b).ok());
			SwfFontOutlines empty = base;
			empty.glyphs.clear();
			empty.codes.clear();
			CHECK_FALSE(write_truetype(empty, raw, b).ok());
		}
		{
			SwfFontOutlines f = base;
			f.glyphs.resize(65535);
			f.codes.resize(65535, 0x20);
			Result<std::vector<uint8_t>> t = write_truetype(f, raw, b);
			REQUIRE_FALSE(t.ok());
			CHECK(t.error().message.find("65,534") != std::string::npos);
		}
		{
			TrueTypeOptions odd = raw;
			odd.units_per_em = 1000;
			CHECK_FALSE(write_truetype(base, odd, b).ok());
		}
	}

	TEST_CASE("font: hostile glyphs end on their caps and budgets") {
		hostile_font("4,096 contours x 16,384 points on the 2^25 font budget", [] {
			SwfFontOutlines f = outlines_of(synth_font());
			GlyphOutline g;
			for (uint32_t c = 0; c < kSwfFontMaxContoursPerGlyph; ++c) {
				const int32_t x = int32_t(c % 64) * 300;
				const int32_t y = -int32_t(c / 64) * 300;
				for (const GlyphPoint p : { GlyphPoint{ x, y }, GlyphPoint{ x + 200, y }, GlyphPoint{ x + 200, y + 200 }, GlyphPoint{ x, y + 200 } }) {
					g.points.push_back(p);
				}
				g.contour_ends.push_back(uint32_t(g.points.size() - 1));
			}
			REQUIRE(g.points.size() == kSwfFontMaxPointsPerGlyph);
			f.glyphs[0] = g;
			WorkBudget b(kUiFontConvertBudgetUnits);
			Result<std::vector<uint8_t>> t = write_truetype(f, {}, b);
			REQUIRE_FALSE(t.ok());
			CHECK(t.error().message.find("work steps") != std::string::npos);
			WorkBudget w(kUiFontConvertBudgetUnits);
			CHECK_FALSE(normalise_winding(g, w).ok());
		});
		hostile_font("4,097 contours / 16,385 points in a SWF glyph", [] {
			Font3 f = synth_font();
			Glyph many;
			for (uint32_t c = 0; c <= kSwfFontMaxContoursPerGlyph; ++c) {
				const int32_t x = int32_t(c % 64) * 30;
				const int32_t y = -int32_t(c / 64) * 30;
				many.push_back(Contour{ x, y, { { x + 20, y }, { x + 20, y + 20 } } });
			}
			f.glyphs[0] = many;
			Result<SwfMovie> m = SwfMovie::parse(font_movie(f));
			REQUIRE(m.ok());
			WorkBudget b(kUiFontConvertBudgetUnits);
			Result<SwfFontOutlines> o = load_font_outlines(*m, m->fonts[0], b);
			REQUIRE_FALSE(o.ok());
			CHECK(o.error().message.find("contours") != std::string::npos);
			Contour line{ 0, 0, {} };
			for (uint32_t i = 0; i < kSwfFontMaxPointsPerGlyph + 1; ++i) {
				line.segs.push_back({ int32_t(i % 2) * 10 + int32_t(i), -int32_t(i % 3) });
			}
			f.glyphs[0] = { line };
			Result<SwfMovie> m2 = SwfMovie::parse(font_movie(f));
			REQUIRE(m2.ok());
			WorkBudget b2(kUiFontConvertBudgetUnits);
			Result<SwfFontOutlines> o2 = load_font_outlines(*m2, m2->fonts[0], b2);
			REQUIRE_FALSE(o2.ok());
			CHECK(o2.error().message.find("points") != std::string::npos);
		});
		hostile_font("bad offsets and tables", [] {
			const Bytes tag = define_font3(synth_font());
			const Bytes body(tag.begin() + 6, tag.end());
			WorkBudget b(kUiFontConvertBudgetUnits);
			CHECK(parse_font_outlines(body, swf_tag::kDefineFont3, b).ok());
			CHECK_FALSE(parse_font_outlines(body, swf_tag::kDefineShape, b).ok());

			Bytes swapped = body;
			const size_t name_len = swapped[4];
			const size_t offsets = 5 + name_len + 2;
			std::swap_ranges(swapped.begin() + ptrdiff_t(offsets), swapped.begin() + ptrdiff_t(offsets + 4), swapped.begin() + ptrdiff_t(offsets + 4));
			CHECK_FALSE(parse_font_outlines(swapped, swf_tag::kDefineFont3, b).ok());

			Bytes past = body;
			const uint32_t big = 0x7FFFFFFF;
			std::memcpy(past.data() + offsets + 4 * 4, &big, 4);
			CHECK_FALSE(parse_font_outlines(past, swf_tag::kDefineFont3, b).ok());

			Bytes kern = body;
			kern[kern.size() - 2 * 6 - 2] = 0xff;
			kern[kern.size() - 2 * 6 - 1] = 0xff;
			CHECK_FALSE(parse_font_outlines(kern, swf_tag::kDefineFont3, b).ok());
		});
	}

	TEST_CASE("font: every truncation of a written font is refused; 2,000 mutants each fail cleanly") {
		const SwfFontOutlines o = outlines_of(synth_font());
		const std::vector<uint8_t> ttf = ttf_of(o);

		size_t data_end = 0;
		const size_t tables = size_t(ttf[4] << 8 | ttf[5]);
		for (size_t i = 0; i < tables; ++i) {
			const size_t r = 12 + 16 * i;
			auto be32 = [&](size_t at) { return size_t(ttf[at]) << 24 | size_t(ttf[at + 1]) << 16 | size_t(ttf[at + 2]) << 8 | size_t(ttf[at + 3]); };
			data_end = std::max(data_end, be32(r + 8) + be32(r + 12));
		}
		hostile_font("TrueType truncations", [&] {
			size_t refused = 0;
			for (size_t n = 0; n < data_end; ++n) {
				WorkBudget b(kTrueTypeReadBudgetUnits);
				refused += read_truetype(std::span<const uint8_t>(ttf.data(), n), b).ok() ? 0 : 1;
			}
			CHECK(refused == data_end);
		});
		hostile_font("TrueType mutants", [&] {
			std::mt19937_64 rng(0x7f4a7c15);
			size_t refused = 0;
			for (size_t i = 0; i < fuzz_iters(2000); ++i) {
				Bytes m = ttf;
				mutate(m, rng);
				WorkBudget b(kTrueTypeReadBudgetUnits);
				Result<TrueTypeFont> f = read_truetype(m, b);
				refused += f.ok() ? 0 : 1;
			}
			MESSAGE("TrueType mutants: " << refused << " refused");
			CHECK(refused > 0);
		});
		hostile_font("DefineFont3 truncations and mutants", [&] {
			const Bytes tag = define_font3(synth_font());
			const Bytes body(tag.begin() + 6, tag.end());
			for (size_t n = 0; n < body.size(); ++n) {
				WorkBudget b(kUiFontConvertBudgetUnits);
				CHECK_FALSE(parse_font_outlines(std::span<const uint8_t>(body.data(), n), swf_tag::kDefineFont3, b).ok());
			}
			std::mt19937_64 rng(0x9e3779b9);
			size_t ok = 0;
			for (size_t i = 0; i < fuzz_iters(2000); ++i) {
				Bytes m = body;
				mutate(m, rng);
				WorkBudget b(kUiFontConvertBudgetUnits);
				Result<SwfFontOutlines> f = parse_font_outlines(m, swf_tag::kDefineFont3, b);
				if (!f) {
					continue;
				}
				++ok;
				if (f->kind == SwfFontKind::Outlines) {
					Result<std::vector<uint8_t>> t = write_truetype(*f, {}, b);
					if (t) {
						WorkBudget rb(kTrueTypeReadBudgetUnits);
						CHECK(read_truetype(*t, rb).ok());
					}
				}
			}
			MESSAGE("DefineFont3 mutants: " << ok << " parsed");
		});
	}

	TEST_CASE("font: name resolution rules and the in-memory converter") {
		FontMappingCollection c;
		c.fonts = { { "Purista EA Medium", { "$MENU_medium" } }, { "Purista EA Semibold", { "$MENU_bold" } }, { "Venice-Medium", { "$HUD_medium" } },
			{ "Venice-SemiBold", { "$HUD_bold" } } };
		struct Row {
			const char *name;
			FontResolution how;
			const char *font;
			const char *via;
		};
		const Row rows[] = {
			{ "$HUD_medium", FontResolution::Exact, "Venice-Medium", "$HUD_medium" },
			{ "Purista EA Semibold", FontResolution::Exact, "Purista EA Semibold", "$MENU_bold" },
			{ "$Menu_bold", FontResolution::CaseFolded, "Purista EA Semibold", "$MENU_bold" },
			{ "$Menu_medium*", FontResolution::CaseFolded, "Purista EA Medium", "$MENU_medium" },
			{ "$HUD", FontResolution::Family, "Venice-Medium", "$HUD_medium" },
			{ "$HUD_light", FontResolution::Family, "Venice-Medium", "$HUD_medium" },
			{ "$Menu_light*", FontResolution::Family, "Purista EA Medium", "$MENU_medium" },
			{ "$FONT_TITLE", FontResolution::Family, "Purista EA Semibold", "$MENU_bold" },
			{ "$FONT_TITLE*", FontResolution::Family, "Purista EA Semibold", "$MENU_bold" },
			{ "Arial", FontResolution::Device, "", "" },
			{ "Purista SemiBold", FontResolution::Device, "", "" },
			{ "$WEIRD", FontResolution::Unresolved, "", "" },
		};
		for (const Row &r : rows) {
			const std::string used_name = r.name;
			CAPTURE(used_name);
			const ResolvedFont got = resolve_font_name(c, r.name);
			CHECK(got.how == r.how);
			CHECK(got.font_long_name == r.font);
			CHECK(got.via == r.via);
		}
		CHECK(std::string(font_resolution_name(FontResolution::CaseFolded)).size() > 0);

		std::vector<std::pair<std::string, std::vector<uint8_t>>> payloads;
		payloads.emplace_back("ui/static/synth_fontlib", font_movie(synth_font()));
		const SwfLibrary lib = SwfLibrary::from_memory(std::move(payloads));
		WorkBudget b(kUiFontConvertBudgetUnits);
		Result<ConvertedFont> f = convert_library_font(lib, "ui/static/synth_fontlib", "Synth Sans Medium", nullptr, b);
		REQUIRE_MESSAGE(f.ok(), (f.ok() ? std::string() : f.error().message));
		CHECK(f->source_glyphs == 4);
		CHECK(f->distinct_codes == 3);
		CHECK((f->codes == std::vector<uint16_t>{ 0x41, 0x43, 0x4F }));
		CHECK(f->cache.outcome == CacheOutcome::Uncached);
		CHECK(read_ok(f->ttf).family == "Synth Sans Medium");
		WorkBudget b2(kUiFontConvertBudgetUnits);
		CHECK_FALSE(convert_library_font(lib, "ui/static/synth_fontlib", "No Such Font", nullptr, b2).ok());
		CHECK_FALSE(convert_library_font(lib, "ui/static/none", "Synth Sans Medium", nullptr, b2).ok());
	}
}

namespace {

struct Expect {
	const char *name;
	uint32_t glyphs;
	uint32_t codes;
	uint32_t kerning;
	bool e_acute;
};

Result<UiFontLibrary> load_fonts(const SwfLibrary &p_lib, const EbxResolver &p_ui, const fs::path &p_root, const InstallReader &p_reader, Cache &r_cache) {
	CacheOptions co;
	co.root = p_root;
	co.guard = p_reader.write_guard();
	std::string why;
	const CacheStatus st = r_cache.open(std::move(co), &why);
	REQUIRE_MESSAGE(st == CacheStatus::Ok, why);
	UiFontLibraryOptions o;
	o.cache = &r_cache;
	return UiFontLibrary::load(p_lib, p_ui, o);
}

}

TEST_SUITE("ui-font") {
	TEST_CASE("font: the en UI fonts convert, round-trip and cache (G3, T2, install unchanged)") {
		InstallReader reader;
		if (!reader.locate({})) {
			bf3::test::skip_install_case(reader.describe().c_str());
			return;
		}
		Result<std::shared_ptr<Install>> install = Install::open(reader);
		REQUIRE(install.ok());
		const InstallSnapshot before = reader.snapshot();
		Result<SwfLibrary> lib = SwfLibrary::open(*install);
		REQUIRE(lib.ok());
		Result<std::unique_ptr<EbxLibrary>> ui = EbxLibrary::open(*install, { "Win32/UI" });
		REQUIRE(ui.ok());
		const ScratchRoot scratch;
		const fs::path root = scratch.path / L"cache";

		const Expect expect[] = {
			{ "Purista EA Medium", 444, 442, 335, true },
			{ "Purista EA Semibold", 444, 442, 324, true },
			{ "Venice-Medium", 34, 32, 0, false },
			{ "Venice-SemiBold", 34, 32, 0, false },
		};

		double cold_s = 0.0;
		std::map<std::string, std::vector<uint8_t>> cold_bytes;
		{
			Cache cache;
			Result<UiFontLibrary> fonts = load_fonts(*lib, **ui, root, reader, cache);
			REQUIRE_MESSAGE(fonts.ok(), (fonts.ok() ? std::string() : fonts.error().message));
			cold_s = fonts->load_seconds();
			REQUIRE(fonts->fonts().size() == 4);
			Result<std::shared_ptr<const SwfMovie>> fontlib = lib->load(fonts->mapping().font_library);
			REQUIRE(fontlib.ok());
			for (const Expect &e : expect) {
				const std::string font_name = e.name;
				CAPTURE(font_name);
				const ConvertedFont *f = fonts->find(e.name);
				REQUIRE(f != nullptr);
				CHECK(f->cache.outcome == CacheOutcome::Converted);
				CHECK(f->stats.glyphs == e.glyphs);
				CHECK(f->stats.cmap_codes == e.codes);
				CHECK(f->source_glyphs + 1u == e.glyphs);
				CHECK(f->distinct_codes == e.codes);
				CHECK(f->stats.contours_reversed == 0);
				CHECK(f->stats.kerning_pairs == e.kerning);
				const bool has_e_acute = std::find(f->codes.begin(), f->codes.end(), uint16_t(0x00C9)) != f->codes.end();
				CHECK(has_e_acute == e.e_acute);

				const SwfFontInfo *info = nullptr;
				for (const SwfFontInfo &i : (*fontlib)->fonts) {
					if (i.name == e.name && i.kind == SwfFontKind::Outlines) {
						info = &i;
					}
				}
				REQUIRE(info != nullptr);
				WorkBudget b(kUiFontConvertBudgetUnits);
				Result<SwfFontOutlines> src = load_font_outlines(**fontlib, *info, b);
				REQUIRE(src.ok());
				const TrueTypeFont back = read_ok(f->ttf);
				CHECK(back.family == e.name);
				CHECK(back.glyphs.size() == e.glyphs);
				CHECK(back.cmap.size() == e.codes);
				CHECK(back.kerning.size() == e.kerning);
				check_round_trip(*src, back);
				MESSAGE(std::string(e.name) << ": " << f->stats.glyphs << " glyphs, " << f->stats.cmap_codes << " codes, " << f->stats.contours << " contours, "
							   << f->stats.kerning_pairs << " kerning pairs, " << f->ttf.size() << " bytes");
				cold_bytes[e.name] = f->ttf;
			}

			CHECK(fonts->find("$HUD_medium")->font_long_name == "Venice-Medium");
			CHECK(fonts->find("$Menu_bold")->font_long_name == "Purista EA Semibold");
			CHECK(fonts->find("Arial") == nullptr);
			cache.close();
		}

		double warm_s = 0.0;
		{
			Cache cache;
			Result<UiFontLibrary> fonts = load_fonts(*lib, **ui, root, reader, cache);
			REQUIRE(fonts.ok());
			warm_s = fonts->load_seconds();
			for (const ConvertedFont &f : fonts->fonts()) {
				CAPTURE(f.font_long_name);
				CHECK(f.cache.outcome == CacheOutcome::Hit);
				CHECK(f.ttf == cold_bytes[f.font_long_name]);
			}
			cache.close();
		}

		{
			const size_t want = cold_bytes["Purista EA Medium"].size() + kCacheEntryHeaderSize;
			fs::path entry;
			std::error_code ec;
			for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
				if (it->is_regular_file(ec) && it->file_size(ec) == want) {
					entry = it->path();
				}
			}
			REQUIRE_FALSE(entry.empty());
			{
				std::fstream file(entry, std::ios::in | std::ios::out | std::ios::binary);
				REQUIRE(file.good());
				file.seekg(std::streamoff(kCacheEntryHeaderSize + 1000));
				char c = 0;
				file.read(&c, 1);
				c = char(c ^ 0x5a);
				file.seekp(std::streamoff(kCacheEntryHeaderSize + 1000));
				file.write(&c, 1);
			}
			Cache cache;
			Result<UiFontLibrary> fonts = load_fonts(*lib, **ui, root, reader, cache);
			REQUIRE(fonts.ok());
			for (const ConvertedFont &f : fonts->fonts()) {
				CAPTURE(f.font_long_name);
				if (f.font_long_name == "Purista EA Medium") {
					CHECK(f.cache.dropped_corrupt);
					CHECK(f.cache.outcome == CacheOutcome::Converted);
				} else {
					CHECK(f.cache.outcome == CacheOutcome::Hit);
				}
				CHECK(f.ttf == cold_bytes[f.font_long_name]);
			}
			cache.close();
			Cache again;
			Result<UiFontLibrary> healed = load_fonts(*lib, **ui, root, reader, again);
			REQUIRE(healed.ok());
			for (const ConvertedFont &f : healed->fonts()) {
				CHECK(f.cache.outcome == CacheOutcome::Hit);
			}
			again.close();
		}

		MESSAGE("T2: cold " << cold_s << " s (limit 1.0), warm " << warm_s << " s (limit 0.25)"
							<< std::string(kAsan ? " (asan, reported)" : "; shared machine, noisy"));
		if (!kAsan) {
			CHECK(cold_s <= 1.0);
			CHECK(warm_s <= 0.25);
		}
		const SnapshotDiff diff = diff_snapshots(before, reader.snapshot());
		MESSAGE("install.unchanged: " << before.files.size() << " index files, " << (diff.clean() ? "clean" : diff.summary()));
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}
}
