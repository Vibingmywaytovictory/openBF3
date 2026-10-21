#include <doctest/doctest.h>

#include "bf3/core/install.h"
#include "bf3/core/ui/swf_geometry.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/ui/swf_movie.h"
#include "test_support.h"
#include "ui_swf_writer.h"

#include <chrono>
#include <cmath>
#include <array>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;
using namespace ui1test;

namespace {

SwfMovie parse_geometry(const Bytes &p_payload) {
	Result<SwfMovie> m = SwfMovie::parse(p_payload);
	REQUIRE_MESSAGE(m.ok(), (m.ok() ? std::string() : m.error().message));
	return std::move(*m);
}

ShapeFill solid(uint8_t p_r, uint8_t p_g, uint8_t p_b, uint8_t p_a = 255) {
	ShapeFill f;
	f.r = p_r;
	f.g = p_g;
	f.b = p_b;
	f.a = p_a;
	return f;
}

ShapePath path(int32_t p_x, int32_t p_y, uint32_t p_fill0, uint32_t p_fill1, uint32_t p_line, std::vector<Seg> p_segs) {
	ShapePath p;
	p.x0 = p_x;
	p.y0 = p_y;
	p.fill0 = p_fill0;
	p.fill1 = p_fill1;
	p.line = p_line;
	p.segs = std::move(p_segs);
	return p;
}

Seg to(int32_t p_x, int32_t p_y) {
	return Seg{ p_x, p_y };
}
Seg curve(int32_t p_cx, int32_t p_cy, int32_t p_x, int32_t p_y) {
	Seg s{ p_x, p_y };
	s.curve = true;
	s.cx = p_cx;
	s.cy = p_cy;
	return s;
}

Bytes ring_shape() {
	return define_shape_paths(3, 20, 10, 10, { solid(200, 10, 20, 128) }, {},
			{ path(0, 0, 0, 1, 0, { to(200, 0), to(200, 200), to(0, 200), to(0, 0) }), path(40, 40, 0, 1, 0, { to(40, 160), to(160, 160), to(160, 40), to(40, 40) }) });
}

Bytes shared_edge_shape() {
	return define_shape_paths(2, 21, 20, 10, { solid(1, 2, 3), solid(4, 5, 6) }, {},
			{ path(0, 0, 0, 1, 0, { to(200, 0) }), path(200, 0, 2, 1, 0, { to(200, 200) }), path(200, 200, 0, 1, 0, { to(0, 200), to(0, 0) }),
					path(200, 0, 0, 2, 0, { to(400, 0), to(400, 200), to(200, 200) }) });
}

Bytes curved_shape() {
	return define_shape_paths(3, 22, 30, 30, { solid(9, 9, 9) }, {},
			{ path(500, 300, 0, 1, 0, { curve(500, 500, 300, 500), curve(100, 500, 100, 300), curve(100, 100, 300, 100), curve(500, 100, 500, 300) }) });
}

Bytes lines_shape() {
	ShapeLine thick;
	thick.width = 40;
	thick.r = 10;
	thick.g = 20;
	thick.b = 30;
	thick.a = 40;
	return define_shape_paths(3, 23, 10, 10, {}, { thick },
			{ path(0, 0, 0, 0, 1, { to(200, 0), to(200, 200), to(0, 200), to(0, 0) }), path(300, 300, 0, 0, 1, { to(400, 300), to(400, 400) }) });
}

Bytes shape4_lines() {
	ShapeLine filled;
	filled.width = 60;
	filled.start_cap = 1;
	filled.end_cap = 2;
	filled.join = 2;
	filled.miter = 0x0180;
	filled.no_hscale = true;
	filled.pixel_hinting = true;
	filled.no_close = true;
	filled.fill = solid(7, 8, 9, 10);
	ShapeLine plain;
	plain.width = 20;
	plain.r = 1;
	plain.a = 2;
	plain.no_vscale = true;
	return define_shape_paths(4, 24, 10, 10, { solid(1, 1, 1) }, { filled, plain },
			{ path(0, 0, 0, 1, 1, { to(200, 0), to(200, 200), to(0, 200), to(0, 0) }), path(0, 0, 0, 0, 2, { to(100, 100) }) });
}

Bytes bitmap_and_gradient_shape() {
	std::vector<ShapeFill> fills;
	for (const uint8_t type : { uint8_t(0x40), uint8_t(0x41), uint8_t(0x42), uint8_t(0x43) }) {
		ShapeFill f;
		f.type = type;
		f.bitmap = uint16_t(10 + type - 0x40);
		f.m = Mat{ 20, 0, 0, 20, 3, 4 };
		fills.push_back(f);
	}
	ShapeFill linear;
	linear.type = 0x10;
	linear.m = Mat{ 0.5f, 0, 0, 0.25f, 1, 2 };
	linear.stops = { { 0, { 1, 2, 3, 4 } }, { 255, { 5, 6, 7, 8 } } };
	fills.push_back(linear);
	ShapeFill focal;
	focal.type = 0x13;
	focal.stops = { { 10, { 9, 9, 9, 9 } } };
	focal.focal = 0x0080;
	fills.push_back(focal);
	std::vector<ShapePath> paths;
	for (uint32_t i = 0; i < fills.size(); ++i) {
		const int32_t x = int32_t(i) * 100;
		paths.push_back(path(x, 0, 0, i + 1, 0, { to(x + 100, 0), to(x + 100, 100), to(x, 100), to(x, 0) }));
	}
	return define_shape_paths(4, 25, 30, 5, fills, {}, paths);
}

Bytes open_shape() {
	return define_shape_paths(3, 26, 10, 10, { solid(1, 2, 3) }, {}, { path(0, 0, 0, 1, 0, { to(200, 0), to(200, 200) }) });
}

Bytes geometry_movie() {
	return movie(cat({ ring_shape(), shared_edge_shape(), curved_shape(), lines_shape(), shape4_lines(), bitmap_and_gradient_shape(), open_shape(),
			show_frame(), end_tag() }));
}

bool area_matches(const SwfFillMesh &p_mesh, double p_relative) {
	const double outline = std::fabs(p_mesh.outline_area());
	const double tri = p_mesh.triangle_area();
	return std::fabs(tri - outline) <= p_relative * std::max(outline, 1e-9);
}

double seconds_since_start(std::chrono::steady_clock::time_point p_t0) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_t0).count();
}

}

TEST_SUITE("ui-swf-unit") {
	TEST_CASE("swf geometry: contours per fill, holes, shared edges, curves, lines, styles") {
		const SwfMovie m = parse_geometry(geometry_movie());
		REQUIRE(m.shapes.size() == 7);

		const SwfShape *ring = m.find_shape(20);
		REQUIRE(ring != nullptr);
		REQUIRE(ring->fills.size() == 1);
		CHECK((ring->fills[0].color == SwfColor{ 200, 10, 20, 128 }));
		REQUIRE(ring->meshes.size() == 1);
		const SwfFillMesh &rm = ring->meshes[0];
		CHECK(rm.fill == 0);
		CHECK(rm.contour_ends == std::vector<uint32_t>{ 4, 8 });
		CHECK(rm.open_contours == 0);
		CHECK(std::fabs(std::fabs(rm.outline_area()) - 64.0) < 1e-9);
		CHECK(std::fabs(rm.triangle_area() - 64.0) < 1e-6);
		CHECK(rm.outline[0] == SwfPoint{ 0, 0 });
		CHECK(rm.outline[4] == SwfPoint{ 2, 2 });
		CHECK(ring->mesh_of(0) == &rm);
		CHECK(ring->mesh_of(1) == nullptr);

		const SwfShape *shared = m.find_shape(21);
		REQUIRE(shared != nullptr);
		CHECK((shared->fills[0].color == SwfColor{ 1, 2, 3, 255 }));
		REQUIRE(shared->meshes.size() == 2);
		for (const SwfFillMesh &mesh : shared->meshes) {
			CAPTURE(mesh.fill);
			CHECK(mesh.contour_ends.size() == 1);
			CHECK(mesh.open_contours == 0);
			CHECK(std::fabs(std::fabs(mesh.outline_area()) - 100.0) < 1e-9);
			CHECK(std::fabs(mesh.triangle_area() - 100.0) < 1e-6);
			CHECK(mesh.indices.size() == 6);
		}
		CHECK(shared->meshes[0].outline_area() * shared->meshes[1].outline_area() > 0.0);

		const SwfShape *round = m.find_shape(22);
		REQUIRE(round != nullptr);
		REQUIRE(round->meshes.size() == 1);
		const SwfFillMesh &cm = round->meshes[0];
		const double exact = 200.0 + 4.0 * (2.0 / 3.0) * 50.0;
		const uint32_t n = swf_curve_segments({ 25, 15 }, { 25, 25 }, { 15, 25 });
		CHECK(cm.outline.size() == 4 * n);
		CHECK(std::fabs(std::fabs(cm.outline_area()) - exact) < 4.0 * 2.0 * 15.0 * kSwfFlattenTolerance);
		CHECK(area_matches(cm, 1e-6));
		for (const SwfPoint &p : cm.outline) {
			CHECK(std::isfinite(p.x));
		}

		const SwfShape *lined = m.find_shape(23);
		REQUIRE(lined != nullptr);
		CHECK(lined->meshes.empty());
		REQUIRE(lined->lines.size() == 1);
		CHECK(lined->lines[0].width == 2.0f);
		CHECK((lined->lines[0].color == SwfColor{ 10, 20, 30, 40 }));
		REQUIRE(lined->strokes.size() == 1);
		const SwfStroke &st = lined->strokes[0];
		CHECK(st.line == 0);
		CHECK(st.path_ends == std::vector<uint32_t>{ 5, 8 });
		CHECK(st.closed == std::vector<bool>{ true, false });
		CHECK(st.points[5] == SwfPoint{ 15, 15 });
		CHECK(st.points[7] == SwfPoint{ 20, 20 });

		const SwfShape *s4 = m.find_shape(24);
		REQUIRE(s4 != nullptr);
		REQUIRE(s4->lines.size() == 2);
		const SwfLineStyle &l0 = s4->lines[0];
		CHECK(l0.width == 3.0f);
		CHECK(l0.start_cap == 1);
		CHECK(l0.end_cap == 2);
		CHECK(l0.join == 2);
		CHECK(l0.miter_limit == 1.5f);
		CHECK(l0.no_hscale);
		CHECK_FALSE(l0.no_vscale);
		CHECK(l0.pixel_hinting);
		CHECK(l0.no_close);
		CHECK(l0.has_fill);
		CHECK(l0.fill.kind == SwfFillKind::Solid);
		CHECK((l0.color == SwfColor{ 7, 8, 9, 10 }));
		const SwfLineStyle &l1 = s4->lines[1];
		CHECK_FALSE(l1.has_fill);
		CHECK(l1.no_vscale);
		CHECK((l1.color == SwfColor{ 1, 0, 0, 2 }));
		REQUIRE(s4->strokes.size() == 2);
		CHECK(s4->strokes[1].line == 1);
		REQUIRE(s4->meshes.size() == 1);
		CHECK(area_matches(s4->meshes[0], 1e-9));

		const SwfShape *styles = m.find_shape(25);
		REQUIRE(styles != nullptr);
		REQUIRE(styles->fills.size() == 6);
		const struct {
			bool repeating, clipped, smoothed;
		} flags[] = { { true, false, true }, { false, true, true }, { true, false, false }, { false, true, false } };
		for (size_t i = 0; i < 4; ++i) {
			CAPTURE(i);
			const SwfFillStyle &f = styles->fills[i];
			CHECK(f.kind == SwfFillKind::Bitmap);
			CHECK(f.bitmap_id == 10 + i);
			CHECK(f.repeating() == flags[i].repeating);
			CHECK(f.clipped() == flags[i].clipped);
			CHECK(f.smoothed() == flags[i].smoothed);
			CHECK((f.texel_to_shape() == SwfMatrix{ 1, 0, 0, 1, 3, 4 }));
		}
		const SwfFillStyle &lin = styles->fills[4];
		CHECK(lin.kind == SwfFillKind::LinearGradient);
		CHECK(lin.matrix.a == 0.5f);
		CHECK(lin.matrix.d == 0.25f);
		REQUIRE(lin.stops.size() == 2);
		CHECK((lin.stops[1] == SwfGradientStop{ 255, { 5, 6, 7, 8 } }));
		const SwfFillStyle &foc = styles->fills[5];
		CHECK(foc.kind == SwfFillKind::FocalGradient);
		CHECK(foc.focal == 0.5f);
		CHECK(foc.stops.size() == 1);
		CHECK(styles->meshes.size() == 6);
		CHECK(std::string(swf_fill_kind_name(SwfFillKind::FocalGradient)) == "focal_gradient");

		const SwfShape *open = m.find_shape(26);
		REQUIRE(open != nullptr);
		REQUIRE(open->meshes.size() == 1);
		CHECK(open->meshes[0].open_contours == 1);
		CHECK(open->meshes[0].outline.size() == 3);
		CHECK(std::fabs(open->meshes[0].triangle_area() - 50.0) < 1e-6);
	}

	TEST_CASE("swf geometry: the flattening tolerance and the nonzero sweep") {
		const SwfPoint a{ 0, 0 };
		const SwfPoint c{ 50, 100 };
		const SwfPoint b{ 100, 0 };
		const uint32_t n = swf_curve_segments(a, c, b);
		const double bend = std::hypot(0.0 - 2.0 * 50.0 + 100.0, 0.0 - 2.0 * 100.0 + 0.0);
		CHECK(bend / (4.0 * n * n) <= kSwfFlattenTolerance);
		CHECK(bend / (4.0 * (n - 1) * (n - 1)) > kSwfFlattenTolerance);
		CHECK(swf_curve_segments(a, { 50, 0 }, b) == 1);
		CHECK(swf_curve_segments(a, { 50, 1e9f }, b) == kSwfCurveMaxSegments);

		WorkBudget budget(1u << 20);
		SwfTriangulation t;
		const std::vector<SwfPoint> bow = { { 0, 0 }, { 10, 10 }, { 10, 0 }, { 0, 10 } };
		const std::vector<uint32_t> one = { 4 };
		REQUIRE(swf_triangulate_nonzero(bow, one, budget, t).ok());
		CHECK(std::fabs(swf_signed_area(bow, one)) < 1e-9);
		CHECK(std::fabs(swf_triangle_area(t.vertices, t.indices) - 50.0) < 1e-6);
		CHECK(t.crossings >= 1);

		const std::vector<SwfPoint> nested = { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 }, { 2, 2 }, { 8, 2 }, { 8, 8 }, { 2, 8 } };
		const std::vector<uint32_t> two = { 4, 8 };
		REQUIRE(swf_triangulate_nonzero(nested, two, budget, t).ok());
		CHECK(std::fabs(swf_signed_area(nested, two)) == doctest::Approx(136.0));
		CHECK(swf_triangle_area(t.vertices, t.indices) == doctest::Approx(100.0));

		std::vector<SwfPoint> star;
		for (int i = 0; i < 31; ++i) {
			const double ang = double(i) * 2.0 * 3.14159265358979 * 15.0 / 31.0;
			star.push_back({ float(50.0 * std::cos(ang)), float(50.0 * std::sin(ang)) });
		}
		const std::vector<uint32_t> star_end = { uint32_t(star.size()) };
		const auto t0 = std::chrono::steady_clock::now();
		WorkBudget big(1u << 24);
		REQUIRE(swf_triangulate_nonzero(star, star_end, big, t).ok());
		MESSAGE("31-point star: " << t.slabs << " slabs, " << t.crossings << " crossings, " << t.indices.size() / 3 << " triangles, "
								   << seconds_since_start(t0) << " s");
		CHECK(t.crossings > 0);
		CHECK(swf_triangle_area(t.vertices, t.indices) <= 3.14159265358979 * 2500.0);
		for (const uint32_t i : t.indices) {
			CHECK(i < t.vertices.size());
		}

		WorkBudget tiny(4);
		CHECK_FALSE(swf_triangulate_nonzero(star, star_end, tiny, t).ok());
		const std::vector<SwfPoint> bad = { { 0, 0 }, { std::nanf(""), 1 }, { 1, 1 } };
		CHECK_FALSE(swf_triangulate_nonzero(bad, std::vector<uint32_t>{ 3 }, budget, t).ok());
		CHECK_FALSE(swf_triangulate_nonzero(bad, std::vector<uint32_t>{ 4 }, budget, t).ok());

		constexpr uint32_t kSquares = 20000;
		std::vector<SwfPoint> row;
		std::vector<uint32_t> row_ends;
		row.insert(row.end(), { { 0, 0 }, { 1, 0 }, { 1, 5 }, { 1, 10 }, { 0, 10 } });
		row_ends.push_back(uint32_t(row.size()));
		for (uint32_t i = 1; i < kSquares; ++i) {
			const float x = float(2 * i);
			row.insert(row.end(), { { x, 0 }, { x + 1, 0 }, { x + 1, 10 }, { x, 10 } });
			row_ends.push_back(uint32_t(row.size()));
		}
		const auto r0 = std::chrono::steady_clock::now();
		WorkBudget row_budget(1u << 20);
		REQUIRE(swf_triangulate_nonzero(row, row_ends, row_budget, t).ok());
		MESSAGE(kSquares << " side-by-side squares over 2 slabs: " << t.indices.size() / 3 << " triangles, " << seconds_since_start(r0) << " s");
		CHECK(t.slabs == 2);
		CHECK(t.indices.size() / 3 == 2 * kSquares + 2);
		CHECK(swf_triangle_area(t.vertices, t.indices) == doctest::Approx(10.0 * kSquares));
	}

	TEST_CASE("swf geometry: refused shapes (style index past its table, unknown fill type, too many points)") {
		Result<SwfMovie> past = SwfMovie::parse(movie(cat({ define_shape_paths(3, 30, 1, 1, { solid(1, 1, 1), solid(2, 2, 2) }, {},
																	   { path(0, 0, 0, 3, 0, { to(20, 0), to(0, 20), to(0, 0) }) }),
				show_frame(), end_tag() })));
		REQUIRE_FALSE(past.ok());
		CHECK(past.error().message.find("fill1 style 3 past the 2 styles of its table") != std::string::npos);
		ShapeFill odd;
		odd.type = 0x20;
		Result<SwfMovie> unknown = SwfMovie::parse(movie(cat({ define_shape_paths(3, 31, 1, 1, { odd }, {}, {}), show_frame(), end_tag() })));
		REQUIRE_FALSE(unknown.ok());
		CHECK(unknown.error().message.find("fill type 0x20 is not a SWF fill type") != std::string::npos);

		std::vector<Seg> bends;
		for (int i = 0; i < 1100; ++i) {
			bends.push_back(curve(i * 40 + 20, (i % 2 == 0) ? 30000 : -30000, i * 40 + 40, 0));
		}
		bends.push_back(to(0, 0));
		const auto t0 = std::chrono::steady_clock::now();
		Result<SwfMovie> many = SwfMovie::parse(movie(cat({ define_shape_paths(3, 32, 1, 1, { solid(1, 1, 1) }, {}, { path(0, 0, 0, 1, 0, bends) }),
				show_frame(), end_tag() })));
		const double s = seconds_since_start(t0);
		REQUIRE_FALSE(many.ok());
		CHECK(many.error().message.find("flattened points") != std::string::npos);
		MESSAGE("1,100 maximal curves: " << s << " s");
		if (!kAsan) {
			CHECK(s < 1.0);
		}
	}

	TEST_CASE("swf geometry: 2,000 seeded mutants of the geometry movie parse fully or fail cleanly") {
		std::mt19937_64 rng(0x6e0e7);
		const Bytes base = geometry_movie();
		size_t ok = 0;
		size_t refused = 0;
		for (size_t i = 0; i < fuzz_iters(2000); ++i) {
			Bytes body(base.begin() + 8, base.end());
			mutate(body, rng);
			Result<SwfMovie> m = SwfMovie::parse(gfx_from_body(body));
			if (!m) {
				++refused;
				CHECK(!m.error().message.empty());
				continue;
			}
			++ok;
			for (const SwfShape &s : m->shapes) {
				for (const SwfFillMesh &mesh : s.meshes) {
					CHECK(mesh.fill < s.fills.size());
					CHECK((mesh.contour_ends.empty() || mesh.contour_ends.back() == mesh.outline.size()));
					CHECK(mesh.indices.size() % 3 == 0);
					for (const uint32_t v : mesh.indices) {
						CHECK(v < mesh.vertices.size());
					}
				}
				for (const SwfStroke &st : s.strokes) {
					CHECK(st.line < s.lines.size());
					CHECK(st.path_ends.size() == st.closed.size());
				}
			}
		}
		MESSAGE("geometry mutants: " << ok << " parsed, " << refused << " refused");
		CHECK(ok > 0);
		CHECK(refused > 0);
	}
}

TEST_SUITE("ui-swf") {
	TEST_CASE("swf geometry: every shape of the 434 retail movies (styles census, area identity, HeaderGradient)") {
		bf3::test::Mounted fx;
		if (!bf3::test::open_or_skip(fx)) {
			return;
		}
		Result<SwfLibrary> lib = SwfLibrary::open(fx.install);
		REQUIRE(lib.ok());
		std::map<uint8_t, uint32_t> declared;
		std::map<uint8_t, uint32_t> used;
		uint32_t movies = 0;
		uint32_t errors = 0;
		uint32_t shapes = 0;
		uint32_t lines_declared = 0;
		uint32_t lines_used = 0;
		uint32_t meshes = 0;
		uint32_t open_contours = 0;
		uint32_t triangles = 0;
		uint32_t area_mismatches = 0;
		double worst = 0.0;
		std::string worst_at;
		for (const SwfMovieSource &src : lib->movies()) {
			Result<std::shared_ptr<const SwfMovie>> m = lib->load(src.name);
			if (!m) {
				++errors;
				MESSAGE(src.name << ": " << m.error().message);
				continue;
			}
			++movies;
			for (const SwfShape &s : (*m)->shapes) {
				++shapes;
				for (const SwfFillStyle &f : s.fills) {
					declared[f.type]++;
				}
				lines_declared += uint32_t(s.lines.size());
				lines_used += uint32_t(s.strokes.size());
				for (const SwfFillMesh &mesh : s.meshes) {
					++meshes;
					used[s.fills[mesh.fill].type]++;
					open_contours += mesh.open_contours;
					triangles += uint32_t(mesh.indices.size() / 3);
					const double outline = std::fabs(mesh.outline_area());
					const double rel = std::fabs(mesh.triangle_area() - outline) / std::max(outline, 1e-9);
					if (rel > worst) {
						worst = rel;
						worst_at = src.name + " shape " + std::to_string(s.id);
					}
					if (!area_matches(mesh, 1e-3)) {
						++area_mismatches;
					}
				}
			}
		}
		std::string census = "declared:";
		for (const auto &[type, count] : declared) {
			census += " 0x" + std::to_string(type) + "=" + std::to_string(count);
		}
		census += "; used:";
		for (const auto &[type, count] : used) {
			census += " " + std::to_string(type) + "=" + std::to_string(count);
		}
		MESSAGE(census);
		MESSAGE(movies << " movies, " << errors << " errors, " << shapes << " shapes, " << meshes << " meshes, " << triangles << " triangles, " << open_contours
					   << " open contours, lines declared " << lines_declared << " used " << lines_used << ", worst area " << worst << " at " << worst_at);
		CHECK(movies == 434);
		CHECK(errors == 0);
		CHECK(shapes == 1159);
		CHECK(meshes == 1403);
		CHECK((used == std::map<uint8_t, uint32_t>{ { 0x00, 315 }, { SwfFillStyle::kClippedBitmap, 1014 }, { SwfFillStyle::kClippedBitmapNoSmoothing, 74 } }));
		CHECK((declared == std::map<uint8_t, uint32_t>{ { 0x00, 351 }, { SwfFillStyle::kClippedBitmap, 1865 }, { SwfFillStyle::kClippedBitmapNoSmoothing, 124 } }));
		CHECK(lines_used == 9);
		CHECK(lines_declared == 13);
		CHECK(open_contours == 0);
		CHECK(triangles == 5851);
		CHECK(area_mismatches == 0);

		Result<std::shared_ptr<const SwfMovie>> textures = lib->load("ui/assets/textures");
		REQUIRE(textures.ok());
		const SwfMovie &t = **textures;
		const std::optional<uint16_t> header = t.export_id("HeaderGradient");
		REQUIRE(header == std::optional<uint16_t>(57));
		const SwfSprite *outer = t.find_sprite(57);
		REQUIRE(outer != nullptr);
		REQUIRE(outer->timeline.events.size() == 1);
		CHECK(outer->timeline.events[0].character == 56);
		CHECK(outer->timeline.events[0].matrix.d == doctest::Approx(0.285004).epsilon(1e-5));
		const SwfSprite *inner = t.find_sprite(56);
		REQUIRE(inner != nullptr);
		REQUIRE(inner->timeline.events.size() == 1);
		CHECK(inner->timeline.events[0].character == 55);
		const SwfShape *piece = t.find_shape(55);
		REQUIRE(piece != nullptr);
		CHECK(piece->version == 1);
		CHECK((piece->bounds == SwfRect{ 0, 16, 0, 200 }));
		REQUIRE(piece->fills.size() == 2);
		const SwfFillStyle &fill = piece->fills[1];
		CHECK(fill.kind == SwfFillKind::Bitmap);
		CHECK(fill.type == SwfFillStyle::kClippedBitmapNoSmoothing);
		CHECK(fill.bitmap_id == 54);
		CHECK((fill.texel_to_shape() == SwfMatrix{ 0, 1, -1, 0, 16, 0 }));
		const SwfSubImage *sub = t.find_sub_image(54);
		REQUIRE(sub != nullptr);
		CHECK((std::array<uint16_t, 4>{ sub->x1, sub->y1, sub->width(), sub->height() } == std::array<uint16_t, 4>{ 242, 0, 200, 16 }));
		REQUIRE(t.find_external_image(sub->image_id) != nullptr);
		CHECK(t.find_external_image(sub->image_id)->file_name == "textures_i1.tga");
		REQUIRE(piece->meshes.size() == 1);
		const SwfFillMesh &mesh = piece->meshes[0];
		CHECK(mesh.fill == 1);
		CHECK((mesh.outline == std::vector<SwfPoint>{ { 16, 0 }, { 16, 200 }, { 0, 200 }, { 0, 0 } }));
		CHECK(mesh.indices.size() == 6);
		CHECK(std::fabs(mesh.outline_area()) == 3200.0);
		CHECK(mesh.triangle_area() == 3200.0);
		const SwfMatrix m = fill.texel_to_shape();
		CHECK((SwfPoint{ m.a * 0 + m.c * 0 + m.tx, m.b * 0 + m.d * 0 + m.ty } == SwfPoint{ 16, 0 }));
		CHECK((SwfPoint{ m.a * 200 + m.c * 16 + m.tx, m.b * 200 + m.d * 16 + m.ty } == SwfPoint{ 0, 200 }));
		const SnapshotDiff diff = diff_snapshots(fx.before, fx.reader.snapshot());
		MESSAGE("install.unchanged: " << fx.before.files.size() << " index files, " << (diff.clean() ? "clean" : diff.summary()));
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}
}
