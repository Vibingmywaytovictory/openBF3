#include "swf_view_check.h"

#include "bf3_localisation.h"
#include "godot_util.h"
#include "swf_view.h"
#include "ui_assets.h"
#include "ui_swf_writer.h"

#include "bf3/core/level/budget.h"
#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/swf_text.h"
#include "bf3/core/util/format.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;
using bf3::core::fmt;

namespace {

constexpr const char *kMovies[2] = { "hudmpscreen", "hudtdmscreen" };
constexpr const char *kWidgetRoot = "instance1";
constexpr const char *kCompassPath = "instance1.Compass_01";
constexpr const char *kCompassLinkage = "mc_singlePlayerCompass";
constexpr const char *kCompassName = "multiplayerCompass";
constexpr const char *kTiltPath = "instance1.Ammo_01";
constexpr float kAs2TiltX = -15.0f;
constexpr float kAs2TiltY = 15.0f;
constexpr float kAs2Perspective = 25.0f;
constexpr int32_t kRestCap = 600;
constexpr uint32_t kRestMovieFrame = 90;
constexpr int32_t kPlayFrames = 30;
constexpr int32_t kPerfFrames = 300;
constexpr int32_t kPerfTextFields = 8;
constexpr double kEdge = 2.0;
constexpr double kScaleEdge = 3.0;
constexpr double kTiltMeanDiff = 0.04;
constexpr int32_t kColorLevels = 2;
constexpr std::array<std::pair<int, const char *>, 3> kTargets{ { { 1, "instance1.HudBackgroundWidget_01.Background.renderHolder" },
		{ 1, "instance1.Minimap_01.map" }, { 0, "instance1.Health_01.Health.healthHolder.EKGRenderTarget" } } };
constexpr std::array<const char *, 3> kTargetRenderers{ "t2;iBgDraw", "t2;iUIMinimapRender", "t2;iUISoldierCompRender" };
constexpr const char *kTransmitMovie = "transmit";
constexpr int32_t kTransmitSide = 40;
constexpr uint8_t kTransmitAlphaLow = 102;
constexpr uint8_t kTransmitAlphaHigh = 153;

struct TransmitProbe {
	const char *what;
	int32_t x;
	int32_t y;
	uint8_t want;
	int32_t tolerance;
};

constexpr std::array<TransmitProbe, 5> kTransmitProbes{ { { "outside every piece", 20, 20, 255, 0 }, { "add piece", 120, 120, 255, 0 },
		{ "normal piece alpha 0.4", 220, 120, 153, 0 }, { "normal 0.6 over normal 0.4", 320, 120, 61, 1 }, { "add piece over normal 0.6", 420, 120, 102, 0 } } };

std::shared_ptr<const ui::SwfLibrary> transmit_library() {
	using namespace ui1test;
	const auto square = [](uint16_t p_id, uint8_t p_alpha) {
		ShapeFill f;
		f.r = 255;
		f.g = 255;
		f.b = 255;
		f.a = p_alpha;
		ShapePath path;
		path.fill1 = 1;
		const int32_t side = kTransmitSide * 20;
		path.segs = { { side, 0 }, { side, side }, { 0, side }, { 0, 0 } };
		return define_shape_paths(3, p_id, float(kTransmitSide), float(kTransmitSide), { f }, {}, { path });
	};
	const auto place = [](uint16_t p_depth, uint16_t p_char, float p_x, bool p_add) {
		Place p;
		p.depth = p_depth;
		p.character = p_char;
		p.matrix = Mat{ 1, 0, 0, 1, p_x, 100 };
		if (p_add) {
			p.blend = 8;
		}
		return place_object2(p, true);
	};
	const Bytes tags = cat({ square(1, 255), square(2, kTransmitAlphaLow), square(3, kTransmitAlphaHigh), place(1, 1, 100, true), place(2, 2, 200, false),
			place(3, 2, 300, false), place(4, 3, 300, false), place(5, 3, 400, false), place(6, 1, 400, true), show_frame(), end_tag() });
	return std::make_shared<const ui::SwfLibrary>(ui::SwfLibrary::from_memory({ { std::string("ui/assets/") + kTransmitMovie, movie(tags) } }));
}

struct Box {
	double x0 = 1e30;
	double y0 = 1e30;
	double x1 = -1e30;
	double y1 = -1e30;
	bool empty() const { return x0 > x1 || y0 > y1; }
	void add(double p_x, double p_y) {
		x0 = std::min(x0, p_x);
		y0 = std::min(y0, p_y);
		x1 = std::max(x1, p_x);
		y1 = std::max(y1, p_y);
	}
	void add(const Box &p_b) {
		if (!p_b.empty()) {
			add(p_b.x0, p_b.y0);
			add(p_b.x1, p_b.y1);
		}
	}
	Box grown(double p_m) const { return empty() ? *this : Box{ x0 - p_m, y0 - p_m, x1 + p_m, y1 + p_m }; }
	Box intersect(const Box &p_b) const {
		Box r{ std::max(x0, p_b.x0), std::max(y0, p_b.y0), std::min(x1, p_b.x1), std::min(y1, p_b.y1) };
		return r.empty() ? Box{} : r;
	}
	bool contains(double p_x, double p_y) const { return !empty() && p_x > x0 && p_x < x1 && p_y > y0 && p_y < y1; }
	bool inside(const Box &p_outer, double p_tol) const {
		return empty() || (!p_outer.empty() && x0 >= p_outer.x0 - p_tol && y0 >= p_outer.y0 - p_tol && x1 <= p_outer.x1 + p_tol && y1 <= p_outer.y1 + p_tol);
	}
	bool near(const Box &p_b, double p_tol) const {
		return !empty() && !p_b.empty() && std::abs(x0 - p_b.x0) <= p_tol && std::abs(y0 - p_b.y0) <= p_tol && std::abs(x1 - p_b.x1) <= p_tol &&
				std::abs(y1 - p_b.y1) <= p_tol;
	}
	std::string str() const { return empty() ? std::string("empty") : fmt("[%.1f,%.1f..%.1f,%.1f]", x0, y0, x1, y1); }
};

struct Img {
	int32_t w = 0;
	int32_t h = 0;
	std::vector<uint8_t> px;
	uint8_t at(int32_t p_x, int32_t p_y, int32_t p_c) const { return px[size_t((p_y * w + p_x) * 4 + p_c)]; }
};

Img img_of(Ref<Image> p_image) {
	Img out;
	if (p_image.is_null()) {
		return out;
	}
	if (p_image->get_format() != Image::FORMAT_RGBA8) {
		p_image->convert(Image::FORMAT_RGBA8);
	}
	out.w = p_image->get_width();
	out.h = p_image->get_height();
	const PackedByteArray data = p_image->get_data();
	out.px.assign(data.ptr(), data.ptr() + data.size());
	return out;
}

Box coverage(const Img &p_img) {
	Box b;
	for (int32_t y = 0; y < p_img.h; ++y) {
		for (int32_t x = 0; x < p_img.w; ++x) {
			const size_t i = size_t((y * p_img.w + x) * 4);
			if ((p_img.px[i] | p_img.px[i + 1] | p_img.px[i + 2] | p_img.px[i + 3]) != 0) {
				b.add(x, y);
				b.add(x + 1, y + 1);
			}
		}
	}
	return b;
}

double ink(const Img &p_img) {
	double s = 0.0;
	for (size_t i = 0; i < p_img.px.size(); i += 4) {
		s += double(std::max({ p_img.px[i], p_img.px[i + 1], p_img.px[i + 2], p_img.px[i + 3] })) / 255.0;
	}
	return s;
}

Img premul_over(const Img &p_top, const Img &p_bottom) {
	Img out = p_bottom;
	for (size_t i = 0; i + 3 < out.px.size() && i + 3 < p_top.px.size(); i += 4) {
		const double ta = double(p_top.px[i + 3]) / 255.0;
		for (int c = 0; c < 4; ++c) {
			const double v = double(p_top.px[i + size_t(c)]) + double(p_bottom.px[i + size_t(c)]) * (1.0 - ta);
			out.px[i + size_t(c)] = uint8_t(std::clamp(std::lround(v), 0L, 255L));
		}
	}
	return out;
}

struct Window2 {
	double kx = 1.0;
	double ky = 1.0;
	double ox = 0.0;
	double oy = 0.0;
	std::array<double, 2> map(const ui::SwfMatrix &p_m, double p_x, double p_y) const {
		return { kx * (p_m.a * p_x + p_m.c * p_y + p_m.tx) + ox, ky * (p_m.b * p_x + p_m.d * p_y + p_m.ty) + oy };
	}
};

struct Expect {
	Box box;
	std::vector<std::array<double, 2>> points;
	double margin = 0.0;
};

double filter_margin(const ui::SwfFilter &p_f, const Window2 &p_w) {
	if (p_f.kind != ui::SwfFilterKind::DropShadow && p_f.kind != ui::SwfFilterKind::Glow && p_f.kind != ui::SwfFilterKind::Blur) {
		return 0.0;
	}
	const double k = std::max(p_w.kx, p_w.ky);
	const double blur = double(std::max(p_f.blur_x, p_f.blur_y)) * k * double(p_f.passes) * 0.5 + 1.0;
	const double offset = p_f.kind == ui::SwfFilterKind::DropShadow ? double(p_f.distance) * k : 0.0;
	return blur + offset + 1.0;
}

bool alpha_visible(const ui::SwfSceneNode &p_n) {
	return p_n.world_color.mul[3] > 0.0f || p_n.world_color.add[3] > 0.0f;
}

void expect_node(const ui::SwfScene &p_scene, const ui::SwfFontSet *p_fonts, ui::SwfNodeId p_id, const Window2 &p_w, bool p_all, double p_margin,
		const ui::SwfMatrix *p_frame, Expect &r_out) {
	const ui::SwfSceneNode *n = p_scene.node(p_id);
	if (n == nullptr) {
		return;
	}
	if (!p_all && (!n->world_visible || n->clip_depth != 0 || !alpha_visible(*n))) {
		return;
	}
	double margin = p_margin;
	if (!p_all) {
		for (const ui::SwfFilter &f : n->filters) {
			margin += filter_margin(f, p_w);
		}
	}
	const ui::SwfMatrix &m = p_frame != nullptr ? *p_frame : n->world;
	auto put = [&](double p_x, double p_y) {
		const std::array<double, 2> q = p_w.map(m, p_x, p_y);
		r_out.points.push_back(q);
		r_out.box.add(q[0] - margin, q[1] - margin);
		r_out.box.add(q[0] + margin, q[1] + margin);
		r_out.margin = std::max(r_out.margin, margin);
	};
	const ui::SwfShape *shape = n->shape != nullptr ? n->shape : n->drawing.get();
	if (shape != nullptr) {
		for (const ui::SwfFillMesh &mesh : shape->meshes) {
			for (const ui::SwfPoint &v : mesh.vertices) {
				put(v.x, v.y);
			}
		}
		for (const ui::SwfStroke &st : shape->strokes) {
			const double hw = st.line < shape->lines.size() ? std::max(0.5, double(shape->lines[st.line].width) * 0.5) : 0.5;
			for (const ui::SwfPoint &v : st.points) {
				put(v.x - hw, v.y - hw);
				put(v.x + hw, v.y + hw);
			}
		}
	}
	if (n->edit_text != nullptr && p_fonts != nullptr) {
		bf3::core::level::WorkBudget budget(ui::kSwfTextBudgetUnits);
		bf3::core::Result<ui::SwfTextLayout> layout = ui::swf_layout_node_text(*p_fonts, *n, budget);
		if (layout) {
			const ui::SwfRect &fb = layout->bounds;
			const Box field{ double(fb.x_min), double(fb.y_min), double(fb.x_max), double(fb.y_max) };
			for (const ui::SwfGlyphPlacement &g : layout->glyphs) {
				if (g.face == nullptr || g.glyph >= g.face->outlines.glyphs.size()) {
					continue;
				}
				const double s = double(g.size) / double(ui::kSwfFontUnitsPerEm);
				Box glyph;
				for (const ui::GlyphPoint &p : g.face->outlines.glyphs[g.glyph].points) {
					glyph.add(double(g.x) + p.x * s, double(g.baseline) + p.y * s);
				}
				if (glyph.intersect(field).empty()) {
					continue;
				}
				for (const ui::GlyphPoint &p : g.face->outlines.glyphs[g.glyph].points) {
					put(std::clamp(double(g.x) + p.x * s, field.x0, field.x1), std::clamp(double(g.baseline) + p.y * s, field.y0, field.y1));
				}
			}
		}
	}
	if (n->sub_image != nullptr) {
		put(0.0, 0.0);
		put(n->sub_image->width(), n->sub_image->height());
	}
	for (const ui::SwfNodeId c : n->children) {
		expect_node(p_scene, p_fonts, c, p_w, p_all, margin, nullptr, r_out);
	}
}

Expect expect_of(const ui::SwfScene &p_scene, const ui::SwfFontSet *p_fonts, ui::SwfNodeId p_id, const SwfStageFit &p_fit, bool p_all = false) {
	Window2 w{ p_fit.scale_x, p_fit.scale_y, p_fit.offset_x, p_fit.offset_y };
	Expect e;
	double margin = 0.0;
	if (!p_all) {
		for (const ui::SwfSceneNode *a = p_scene.node(p_scene.node(p_id)->parent); a != nullptr; a = p_scene.node(a->parent)) {
			for (const ui::SwfFilter &f : a->filters) {
				margin += filter_margin(f, w);
			}
		}
	}
	expect_node(p_scene, p_fonts, p_id, w, p_all, margin, nullptr, e);
	return e;
}

std::array<double, 9> independent_tilt(double p_px, double p_py, const ui::SwfRect &p_stage, const SwfStageFit &p_fit) {
	const double cx = (p_stage.x_min + p_stage.x_max) / 2.0;
	const double cy = (p_stage.y_min + p_stage.y_max) / 2.0;
	const double f = (p_stage.height() / 2.0) / std::tan(double(kAs2Perspective) / 2.0 * bf3::core::kDegToRad);
	const double ax = double(kAs2TiltX) * bf3::core::kDegToRad;
	const double ay = double(kAs2TiltY) * bf3::core::kDegToRad;
	auto project = [&](double p_x, double p_y) {
		const double u = p_x - p_px;
		const double v = p_y - p_py;
		const double y1 = v * std::cos(ax);
		const double z1 = v * std::sin(ax);
		const double x2 = u * std::cos(ay) + z1 * std::sin(ay);
		const double z2 = -u * std::sin(ay) + z1 * std::cos(ay);
		const double X = p_px - cx + x2;
		const double Y = p_py - cy + y1;
		return std::array<double, 2>{ cx + f * X / (f + z2), cy + f * Y / (f + z2) };
	};
	std::array<double, 9> h{};
	const std::array<std::array<double, 2>, 4> src{ { { p_px - 100.0, p_py - 100.0 }, { p_px + 100.0, p_py - 100.0 }, { p_px + 100.0, p_py + 100.0 },
			{ p_px - 100.0, p_py + 100.0 } } };
	double a[8][9] = {};
	for (int i = 0; i < 4; ++i) {
		const std::array<double, 2> d = project(src[size_t(i)][0], src[size_t(i)][1]);
		const double sx = p_fit.scale_x * src[size_t(i)][0] + p_fit.offset_x;
		const double sy = p_fit.scale_y * src[size_t(i)][1] + p_fit.offset_y;
		const double dx = p_fit.scale_x * d[0] + p_fit.offset_x;
		const double dy = p_fit.scale_y * d[1] + p_fit.offset_y;
		const double r0[9] = { sx, sy, 1.0, 0.0, 0.0, 0.0, -dx * sx, -dx * sy, dx };
		const double r1[9] = { 0.0, 0.0, 0.0, sx, sy, 1.0, -dy * sx, -dy * sy, dy };
		std::copy(r0, r0 + 9, a[2 * i]);
		std::copy(r1, r1 + 9, a[2 * i + 1]);
	}
	for (int c = 0; c < 8; ++c) {
		int p = c;
		for (int r = c + 1; r < 8; ++r) {
			if (std::abs(a[r][c]) > std::abs(a[p][c])) {
				p = r;
			}
		}
		std::swap(a[c], a[p]);
		for (int r = 0; r < 8; ++r) {
			if (r == c || a[c][c] == 0.0) {
				continue;
			}
			const double k = a[r][c] / a[c][c];
			for (int j = c; j < 9; ++j) {
				a[r][j] -= k * a[c][j];
			}
		}
	}
	for (int i = 0; i < 8; ++i) {
		h[size_t(i)] = a[i][8] / a[i][i];
	}
	h[8] = 1.0;
	return h;
}

std::array<double, 2> happly(const std::array<double, 9> &p_h, double p_x, double p_y) {
	const double w = p_h[6] * p_x + p_h[7] * p_y + p_h[8];
	return { (p_h[0] * p_x + p_h[1] * p_y + p_h[2]) / w, (p_h[3] * p_x + p_h[4] * p_y + p_h[5]) / w };
}

std::array<double, 9> hinverse(const std::array<double, 9> &p_h) {
	const double a = p_h[0], b = p_h[1], c = p_h[2], d = p_h[3], e = p_h[4], f = p_h[5], g = p_h[6], h = p_h[7], i = p_h[8];
	const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
	return { (e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det, (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det,
		(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det };
}

Img warp(const Img &p_src, const std::array<double, 9> &p_h) {
	Img out;
	out.w = p_src.w;
	out.h = p_src.h;
	out.px.assign(p_src.px.size(), 0);
	const std::array<double, 9> inv = hinverse(p_h);
	for (int32_t y = 0; y < out.h; ++y) {
		for (int32_t x = 0; x < out.w; ++x) {
			const std::array<double, 2> s = happly(inv, x + 0.5, y + 0.5);
			const double fx = s[0] - 0.5;
			const double fy = s[1] - 0.5;
			const int32_t ix = int32_t(std::floor(fx));
			const int32_t iy = int32_t(std::floor(fy));
			const double tx = fx - ix;
			const double ty = fy - iy;
			for (int c = 0; c < 4; ++c) {
				double v = 0.0;
				for (int k = 0; k < 4; ++k) {
					const int32_t sx = ix + (k & 1);
					const int32_t sy = iy + (k >> 1);
					if (sx < 0 || sy < 0 || sx >= p_src.w || sy >= p_src.h) {
						continue;
					}
					const double wgt = ((k & 1) ? tx : 1.0 - tx) * ((k >> 1) ? ty : 1.0 - ty);
					v += wgt * p_src.at(sx, sy, c);
				}
				out.px[size_t((y * out.w + x) * 4 + c)] = uint8_t(std::clamp(std::lround(v), 0L, 255L));
			}
		}
	}
	return out;
}

struct RestTracker {
	std::map<std::string, std::set<uint32_t>> seen;
	std::vector<std::string> moving;
	bool settled(const ui::SwfScene &p_scene) {
		bool all = true;
		for (const ui::SwfNodeId id : p_scene.draw_order()) {
			const ui::SwfSceneNode *n = p_scene.node(id);
			if (n == nullptr || !n->playing || n->total_frames() < 2) {
				continue;
			}
			if (seen[std::string(p_scene.movie_name()) + ":" + n->path].insert(n->frame).second) {
				all = false;
				moving.push_back(n->path + fmt(" %u/%u", n->frame, n->total_frames()));
			}
		}
		return all;
	}
};

bool under_mask(const ui::SwfScene &p_scene, const ui::SwfSceneNode &p_n) {
	for (const ui::SwfSceneNode *a = &p_n; a != nullptr; a = p_scene.node(a->parent)) {
		if (a->clip_depth != 0) {
			return true;
		}
	}
	return false;
}

std::optional<ui::SwfColor> single_solid_fill(const ui::SwfSceneNode &p_n) {
	if (p_n.shape == nullptr || !p_n.shape->strokes.empty() || p_n.shape->meshes.empty() || p_n.scaling_grid != nullptr) {
		return std::nullopt;
	}
	std::optional<ui::SwfColor> color;
	for (const ui::SwfFillMesh &mesh : p_n.shape->meshes) {
		if (mesh.fill >= p_n.shape->fills.size() || p_n.shape->fills[mesh.fill].kind != ui::SwfFillKind::Solid) {
			return std::nullopt;
		}
		const ui::SwfColor c = p_n.shape->fills[mesh.fill].color;
		if (color && !(*color == c)) {
			return std::nullopt;
		}
		color = c;
	}
	return color;
}

bool additive_chain(const ui::SwfScene &p_scene, const ui::SwfSceneNode &p_n) {
	for (const ui::SwfSceneNode *a = &p_n; a != nullptr; a = p_scene.node(a->parent)) {
		if (a->blend_mode == ui::swf_blend::kAdd) {
			return true;
		}
	}
	return false;
}

bool rgb_add(const ui::SwfColorXform &p_cx) {
	return p_cx.add[0] != 0.0f || p_cx.add[1] != 0.0f || p_cx.add[2] != 0.0f;
}

bool rgb_from_add_only(const ui::SwfColorXform &p_cx) {
	return p_cx.mul[0] == 0.0f && p_cx.mul[1] == 0.0f && p_cx.mul[2] == 0.0f;
}

std::pair<std::array<uint8_t, 4>, uint32_t> dominant_pixel(const Img &p_img) {
	std::map<uint32_t, uint32_t> counts;
	for (size_t i = 0; i + 3 < p_img.px.size(); i += 4) {
		if (p_img.px[i + 3] != 0) {
			counts[uint32_t(p_img.px[i]) | uint32_t(p_img.px[i + 1]) << 8 | uint32_t(p_img.px[i + 2]) << 16 | uint32_t(p_img.px[i + 3]) << 24]++;
		}
	}
	std::pair<uint32_t, uint32_t> best{ 0, 0 };
	for (const auto &[key, n] : counts) {
		if (n > best.second) {
			best = { key, n };
		}
	}
	return { { uint8_t(best.first), uint8_t(best.first >> 8), uint8_t(best.first >> 16), uint8_t(best.first >> 24) }, best.second };
}

bool supported(const ui::SwfFilter &p_f) {
	return p_f.kind == ui::SwfFilterKind::DropShadow || p_f.kind == ui::SwfFilterKind::Glow || p_f.kind == ui::SwfFilterKind::Blur;
}

}

struct Bf3SwfViewCheck::State {
	struct Row {
		std::string name;
		bool pass = false;
		std::string detail;
	};
	struct Job {
		std::function<void()> setup;
		std::function<void()> measure;
	};
	struct Measured {
		Box small;
		Box big;
		double ink_small = 0.0;
		double ink_big = 0.0;
	};
	struct Tally {
		uint32_t checked = 0;
		uint32_t failed = 0;
		std::string detail;
		void fail(const std::string &p_what) {
			++failed;
			if (failed <= 12) {
				detail += (detail.empty() ? "" : "; ") + p_what;
			}
		}
	};

	std::string shot_dir;
	int phase = 0;
	int frames = 0;
	double played_seconds = 0.0;
	std::array<Bf3SwfView *, 4> views{};
	Bf3SwfView *transmit = nullptr;
	SubViewport *big = nullptr;
	std::vector<Row> rows;
	std::vector<Job> jobs;
	size_t job = 0;
	bool job_armed = false;
	std::map<std::string, Measured> measured[2];
	std::map<std::string, Measured> filtered[2];
	Vector2i small_size;
	uint32_t mask_drawn = 0;
	uint32_t mask_cut = 0;
	Tally color;
	uint32_t color_solid = 0;
	uint32_t color_add_only = 0;
	uint32_t additive_light = 0;
	uint32_t additive_bare = 0;
	Img tilt_base;
	std::vector<std::pair<int, std::string>> perf_fields;
	std::vector<double> perf_ms;
	int perf_frame = 0;
	uint32_t perf_structure0[2] = { 0, 0 };
	uint32_t perf_builds0[2] = { 0, 0 };
	int exit_code = 0;
	const ui::SwfFontSet *fonts = nullptr;

	Bf3SwfView *small_view(int p_movie) const { return views[size_t(p_movie)]; }
	Bf3SwfView *big_view(int p_movie) const { return views[size_t(2 + p_movie)]; }
	void add(const std::string &p_name, bool p_pass, const std::string &p_detail) { rows.push_back({ p_name, p_pass, p_detail }); }
	void add(const std::string &p_name, const Tally &p_t) {
		add(p_name, p_t.checked > 0 && p_t.failed == 0, fmt("%u checked, %u failed", p_t.checked, p_t.failed) + (p_t.detail.empty() ? "" : ": " + p_t.detail));
	}
};

Bf3SwfViewCheck::Bf3SwfViewCheck() :
		state_(std::make_unique<State>()) {}

Bf3SwfViewCheck::~Bf3SwfViewCheck() = default;

void Bf3SwfViewCheck::_bind_methods() {}

void Bf3SwfViewCheck::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	state_->shot_dir = user_arg_value("shot-dir");
	Window *w = get_window();
	w->set_content_scale_mode(Window::CONTENT_SCALE_MODE_DISABLED);
	w->set_size(Vector2i(1280, 720));
	set_process(true);
}

void Bf3SwfViewCheck::setup() {
	State &s = *state_;
	s.big = memnew(SubViewport);
	s.big->set_size(Vector2i(2560, 1440));
	s.big->set_update_mode(SubViewport::UPDATE_ALWAYS);
	add_child(s.big);
	for (int i = 0; i < 4; ++i) {
		Bf3SwfView *v = memnew(Bf3SwfView);
		v->set_playing(false);
		if (i < 2) {
			add_child(v);
		} else {
			s.big->add_child(v);
		}
		const bf3::core::Status st = v->open(kMovies[i % 2]);
		s.add(fmt("open %s %s", kMovies[i % 2], i < 2 ? "1280x720" : "2560x1440"), bool(st), st ? std::string("opened") : st.error().message);
		s.views[size_t(i)] = v;
	}
}

void Bf3SwfViewCheck::finish() {
	State &s = *state_;
	bool all = true;
	for (const State::Row &r : s.rows) {
		all = all && r.pass;
		UtilityFunctions::print(to_godot(fmt("[swf view] %s %s: %s", r.pass ? "PASS" : "FAIL", r.name.c_str(), r.detail.c_str())));
	}
	UtilityFunctions::print(all && s.exit_code == 0 ? "SWF VIEW CHECK: PASS" : "SWF VIEW CHECK: FAIL");
	set_process(false);
	get_tree()->quit(all && s.exit_code == 0 ? 0 : 1);
}

void Bf3SwfViewCheck::_process(double p_delta) {
	State &s = *state_;
	Bf3UiAssets *assets = Bf3UiAssets::current();
	if (s.phase == 0) {
		if (assets == nullptr) {
			s.add("assets", false, "no Bf3UiAssets in the scene");
			finish();
			return;
		}
		if (assets->has_failed()) {
			s.add("assets", false, to_std(assets->get_error()));
			finish();
			return;
		}
		if (!assets->is_loaded()) {
			return;
		}
		s.fonts = assets->swf_fonts();
		s.add("DefineFont3 font set", s.fonts != nullptr, s.fonts != nullptr ? fmt("%zu faces", s.fonts->faces().size()) : std::string("missing"));
		setup();
		for (Bf3SwfView *v : s.views) {
			if (!v->is_open()) {
				finish();
				return;
			}
		}
		for (int i : { 1, 3 }) {
			ui::SwfScene &sc = *s.views[size_t(i)]->scene();
			const ui::SwfSceneNode *compass = sc.find_node(kCompassPath);
			int32_t depth = 0;
			for (const ui::SwfNodeId c : compass != nullptr ? compass->children : std::vector<ui::SwfNodeId>{}) {
				depth = std::max(depth, sc.node(c)->as2_depth() + 1);
			}
			const bf3::core::Result<ui::SwfNodeId> r = sc.attach_movie(kCompassPath, kCompassLinkage, kCompassName, depth);
			if (i == 1) {
				s.add("compass strip attached as in Widget.Hud.Compass onClipLoad", bool(r), r ? fmt("depth %d", depth) : r.error().message);
			}
		}
		s.views[0]->set_playing(true);
		s.views[1]->set_playing(true);
		s.played_seconds += p_delta;
		s.phase = 1;
		s.frames = 1;
		return;
	}
	if (s.phase == 1) {
		if (s.frames < kPlayFrames) {
			s.played_seconds += p_delta;
			++s.frames;
			return;
		}
		s.views[0]->set_playing(false);
		s.views[1]->set_playing(false);
		for (int m = 0; m < 2; ++m) {
			ui::SwfScene &sc = *s.small_view(m)->scene();
			const double expected = std::floor(s.played_seconds * double(sc.frame_rate()));
			const double got = double(sc.stats().ticks + sc.stats().frames_dropped);
			s.add(fmt("timeline at movie frame rate %s", kMovies[m]), std::abs(got - expected) <= 1.0,
					fmt("%.0f frames advanced over %.3f s of process time at %.1f fps (expected %.0f)", got, s.played_seconds, double(sc.frame_rate()), expected));
			const bf3::core::Status st = s.big_view(m)->scene()->advance(sc.stats().ticks);
			(void)st;
		}
		RestTracker tracker;
		bool settled = tracker.settled(*s.small_view(0)->scene()) & tracker.settled(*s.small_view(1)->scene());
		int32_t rest = 0;
		const auto at_rest_frame = [&s]() { return s.small_view(0)->scene()->stats().ticks >= kRestMovieFrame && s.small_view(1)->scene()->stats().ticks >= kRestMovieFrame; };
		while (rest < kRestCap && (!settled || !at_rest_frame())) {
			for (Bf3SwfView *v : s.views) {
				const bf3::core::Status st = v->scene()->advance(1);
				(void)st;
			}
			++rest;
			tracker.moving.clear();
			settled = tracker.settled(*s.small_view(0)->scene()) & tracker.settled(*s.small_view(1)->scene());
		}
		std::string blockers;
		for (const std::string &b : tracker.moving) {
			if (blockers.size() < 600) {
				blockers += " " + b;
			}
		}
		const uint32_t ticks0 = s.small_view(0)->scene()->stats().ticks;
		const uint32_t ticks1 = s.small_view(1)->scene()->stats().ticks;
		s.add("rest frame reached", settled && ticks0 == kRestMovieFrame && ticks1 == kRestMovieFrame,
				fmt("every playing timeline stopped or back on a frame it already showed after %d frames (cap %d), at movie frames %u and %u (rest frame %u)", rest,
						kRestCap, ticks0, ticks1, kRestMovieFrame) +
						blockers);
		s.phase = 2;
		s.frames = 0;
		return;
	}
	if (s.phase == 2) {
		if (++s.frames < 3) {
			return;
		}
		const ui::SwfRect fit_small = s.small_view(0)->visible_rect();
		const ui::SwfRect fit_big = s.big_view(0)->visible_rect();
		const SwfStageFit fs = s.small_view(0)->stage_fit();
		const SwfStageFit fb = s.big_view(0)->stage_fit();
		s.add("stage fit 1280x720", fs.scale_x == 1.0 && fs.scale_y == 1.0 && fs.offset_x == 0.0 && fs.offset_y == 0.0,
				fmt("scale %.3f x %.3f offset %.1f, %.1f; visible %.0f..%.0f x %.0f..%.0f", fs.scale_x, fs.scale_y, fs.offset_x, fs.offset_y, fit_small.x_min,
						fit_small.x_max, fit_small.y_min, fit_small.y_max));
		s.add("stage fit 2560x1440", fb.scale_x == 2.0 && fb.scale_y == 2.0 && fb.offset_x == 0.0 && fb.offset_y == 0.0,
				fmt("scale %.3f x %.3f offset %.1f, %.1f; visible %.0f..%.0f x %.0f..%.0f", fb.scale_x, fb.scale_y, fb.offset_x, fb.offset_y, fit_big.x_min,
						fit_big.x_max, fit_big.y_min, fit_big.y_max));
		bool any_mask = false;
		bool any_filter = false;
		for (int i = 0; i < 4; ++i) {
			Bf3SwfView *v = s.views[size_t(i)];
			const ui::SwfScene &sc = *v->scene();
			uint32_t layers = 0;
			uint32_t passes = 0;
			uint32_t shadows = 0;
			uint32_t glows = 0;
			uint32_t blurs = 0;
			uint32_t masks = 0;
			uint32_t add_nodes = 0;
			uint32_t glyph_missing = 0;
			for (const ui::SwfNodeId id : sc.draw_order()) {
				const ui::SwfSceneNode &n = *sc.node(id);
				if (n.clip_depth != 0 && !under_mask(sc, *sc.node(n.parent))) {
					++masks;
				}
				if (n.script_mask != ui::kSwfNoNode && sc.node(n.script_mask) != nullptr && !under_mask(sc, n)) {
					++masks;
				}
				if (under_mask(sc, n)) {
					continue;
				}
				uint32_t ok = 0;
				for (const ui::SwfFilter &f : n.filters) {
					if (supported(f)) {
						++ok;
						(f.kind == ui::SwfFilterKind::DropShadow ? shadows : f.kind == ui::SwfFilterKind::Glow ? glows : blurs)++;
					}
				}
				layers += ok > 0 ? 1 : 0;
				passes += ok;
				add_nodes += n.blend_mode == ui::swf_blend::kAdd ? 1 : 0;
				if (n.edit_text != nullptr && s.fonts != nullptr) {
					bf3::core::level::WorkBudget budget(ui::kSwfTextBudgetUnits);
					const bf3::core::Result<ui::SwfTextLayout> l = ui::swf_layout_node_text(*s.fonts, n, budget);
					glyph_missing += l ? l->missing_glyphs : 0;
				}
			}
			const SwfViewStats &st = v->stats();
			const std::string name = fmt("%s %s", kMovies[i % 2], i < 2 ? "1280x720" : "2560x1440");
			s.add("filter layers " + name,
					st.filter_layers == layers && st.filter_passes == passes && st.drop_shadow_passes == shadows && st.glow_passes == glows &&
							st.blur_passes == blurs && st.kernels_clamped == 0,
					fmt("view %u layers / %u passes (%u drop shadow, %u glow, %u blur), scene %u / %u (%u, %u, %u); kernels clamped %u", st.filter_layers,
							st.filter_passes, st.drop_shadow_passes, st.glow_passes, st.blur_passes, layers, passes, shadows, glows, blurs, st.kernels_clamped));
			s.add("mask groups " + name, st.mask_groups == masks, fmt("view %u, scene clip-depth masks %u", st.mask_groups, masks));
			s.add("add blend " + name, add_nodes == 0 || st.additive_items + st.blend_layers > 0,
					fmt("scene add-blend nodes %u, view additive items %u, blend layers %u", add_nodes, st.additive_items, st.blend_layers));
			s.add("data coverage " + name,
					st.unsupported_fills == 0 && st.unsupported_filters == 0 && st.unsupported_blends == 0 && st.unsupported_lines == 0 && st.missing_textures == 0 &&
							st.missing_fonts == 0 && st.missing_glyphs == glyph_missing && st.layers_clamped == 0,
					fmt("items %u (fills %u, bitmap fills %u, strokes %u, text %u, bitmaps %u, glyphs %u); unsupported fills %u filters %u blends %u lines %u; "
						"missing textures %u fonts %u glyphs %u (layout %u); layers clamped %u",
							st.items, st.fill_items, st.bitmap_fill_items, st.stroke_items, st.text_items, st.bitmap_items, st.glyphs, st.unsupported_fills,
							st.unsupported_filters, st.unsupported_blends, st.unsupported_lines, st.missing_textures, st.missing_fonts, st.missing_glyphs, glyph_missing,
							st.layers_clamped));
			any_mask = any_mask || st.mask_groups > 0;
			any_filter = any_filter || st.filter_passes > 0;
		}
		s.add("mask and filter passes present", any_mask && any_filter, fmt("mask %s, filter %s", any_mask ? "yes" : "no", any_filter ? "yes" : "no"));

		const Img win_small = img_of(get_viewport()->get_texture()->get_image());
		const Img win_big = img_of(s.big->get_texture()->get_image());
		s.small_size = Vector2i(win_small.w, win_small.h);
		const Img hud_small = premul_over(img_of(s.small_view(1)->capture_hud()), img_of(s.small_view(0)->capture_hud()));
		const Img hud_big = premul_over(img_of(s.big_view(1)->capture_hud()), img_of(s.big_view(0)->capture_hud()));
		s.add("captures", win_small.w == 1280 && win_small.h == 720 && hud_small.w == 1280 && hud_small.h == 720 && win_big.w == 2560 && hud_big.w == 2560,
				fmt("window %dx%d, hud %dx%d, 2x window %dx%d, 2x hud %dx%d", win_small.w, win_small.h, hud_small.w, hud_small.h, win_big.w, win_big.h, hud_big.w,
						hud_big.h));
		if (!s.shot_dir.empty()) {
			Bf3Localisation *loc = Bf3Localisation::current();
			const std::filesystem::path dir = std::filesystem::path(std::u8string(s.shot_dir.begin(), s.shot_dir.end()));
			std::error_code ec;
			std::filesystem::create_directories(dir, ec);
			const std::array<std::pair<const char *, const Img *>, 4> shots{ { { "swf_view_1280.png", &win_small }, { "swf_view_1280_hud.png", &hud_small },
					{ "swf_view_2560.png", &win_big }, { "swf_view_2560_hud.png", &hud_big } } };
			for (const auto &[file, img] : shots) {
				const std::filesystem::path target = dir / file;
				bool ok = loc != nullptr && loc->reader().write_guard().allows_write(target);
				if (ok) {
					PackedByteArray bytes;
					bytes.resize(int64_t(img->px.size()));
					std::copy(img->px.begin(), img->px.end(), bytes.ptrw());
					Ref<Image> out = Image::create_from_data(img->w, img->h, false, Image::FORMAT_RGBA8, bytes);
					const std::u8string u8 = target.u8string();
					ok = out.is_valid() && out->save_png(to_godot(std::string(u8.begin(), u8.end()))) == OK;
				}
				s.add(std::string("shot ") + file, ok, ok ? fmt("%dx%d", img->w, img->h) : std::string("not written"));
			}
		}

		auto isolate = [&s](int p_movie, const std::string &p_path) {
			const bf3::core::Status a = s.small_view(p_movie)->set_isolated(p_path);
			const bf3::core::Status b = s.big_view(p_movie)->set_isolated(p_path);
			(void)a;
			(void)b;
		};
		for (int m = 0; m < 2; ++m) {
			const ui::SwfScene &sc = *s.small_view(m)->scene();
			const ui::SwfSceneNode *root = sc.find_node(kWidgetRoot);
			std::vector<std::string> paths;
			for (const ui::SwfNodeId c : root != nullptr ? root->children : std::vector<ui::SwfNodeId>{}) {
				paths.push_back(sc.node(c)->path);
			}
			for (const ui::SwfNodeId id : sc.draw_order()) {
				const ui::SwfSceneNode &n = *sc.node(id);
				if (n.edit_text != nullptr && n.world_visible && alpha_visible(n) && n.masks.empty() && !under_mask(sc, n)) {
					paths.push_back(n.path);
				}
			}
			for (const std::string &path : paths) {
				s.jobs.push_back({ [isolate, m, path]() { isolate(m, path); },
						[&s, m, path]() {
							State::Measured r;
							const Img a = img_of(s.small_view(m)->capture_hud());
							const Img b = img_of(s.big_view(m)->capture_hud());
							r.small = coverage(a);
							r.big = coverage(b);
							r.ink_small = ink(a);
							r.ink_big = ink(b);
							s.measured[m][path] = r;
							if (m == 0 && path == kTiltPath) {
								s.tilt_base = a;
							}
						} });
			}
		}
		s.jobs.push_back({ []() {}, [&s]() {
							  State::Tally bounds;
							  State::Tally scale;
							  State::Tally text;
							  for (int m = 0; m < 2; ++m) {
								  const ui::SwfScene &sc = *s.small_view(m)->scene();
								  const ui::SwfScene &sb = *s.big_view(m)->scene();
								  for (const auto &[path, r] : s.measured[m]) {
									  const ui::SwfNodeId id = sc.find(path);
									  const ui::SwfNodeId idb = sb.find(path);
									  if (id == ui::kSwfNoNode || idb == ui::kSwfNoNode) {
										  continue;
									  }
									  const Expect es = expect_of(sc, s.fonts, id, s.small_view(m)->stage_fit());
									  const Expect eb = expect_of(sb, s.fonts, idb, s.big_view(m)->stage_fit());
									  ++bounds.checked;
									  const Box window{ kEdge, kEdge, double(s.small_size.x) - kEdge, double(s.small_size.y) - kEdge };
									  if (!r.small.inside(es.box, kEdge) || !r.big.inside(eb.box, kEdge) || (!es.box.intersect(window).empty() && r.small.empty())) {
										  bounds.fail(fmt("%s %s drawn %s / %s expected %s / %s", kMovies[m], path.c_str(), r.small.str().c_str(), r.big.str().c_str(),
												  es.box.str().c_str(), eb.box.str().c_str()));
									  }
									  if (!r.small.empty() || !r.big.empty()) {
										  ++scale.checked;
										  const Box twice{ r.small.x0 * 2.0, r.small.y0 * 2.0, r.small.x1 * 2.0, r.small.y1 * 2.0 };
										  if (!r.big.near(twice, kScaleEdge)) {
											  scale.fail(fmt("%s %s 1280 %s x2 vs 2560 %s", kMovies[m], path.c_str(), r.small.str().c_str(), r.big.str().c_str()));
										  }
									  }
									  const ui::SwfSceneNode &n = *sc.node(id);
									  if (n.edit_text != nullptr && !es.points.empty()) {
										  Box glyphs;
										  for (const auto &p : es.points) {
											  glyphs.add(p[0], p[1]);
										  }
										  ++text.checked;
										  const Box shown = glyphs.intersect(Box{ 0.0, 0.0, double(s.small_size.x), double(s.small_size.y) });
										  const bool exact = shown.empty() ? (r.small.empty() || es.margin > 0.0)
																		   : es.margin == 0.0 ? r.small.near(shown, kEdge)
																							  : shown.grown(-kEdge).inside(r.small, 0.0);
										  if (!exact) {
											  text.fail(fmt("%s %s drawn %s ink %s margin %.1f", kMovies[m], path.c_str(), r.small.str().c_str(), glyphs.str().c_str(), es.margin));
										  }
									  }
								  }
							  }
							  s.add("element bounds inside the resolved scene's geometry (1280x720 and 2560x1440)", bounds);
							  s.add("scale invariance 2560x1440 = 2 x 1280x720", scale);
							  s.add("DefineFont3 text ink matches the core layout", text);
						  } });

		for (int m = 0; m < 2; ++m) {
			const ui::SwfScene &sc = *s.small_view(m)->scene();
			for (const ui::SwfNodeId id : sc.draw_order()) {
				const ui::SwfSceneNode &n = *sc.node(id);
				const std::optional<ui::SwfColor> fill = single_solid_fill(n);
				const bool add_only = !fill && n.shape != nullptr && !n.shape->meshes.empty() && rgb_from_add_only(n.world_color);
				if ((!fill && !add_only) || !n.world_visible || !alpha_visible(n) || !rgb_add(n.world_color) || under_mask(sc, n) || additive_chain(sc, n)) {
					continue;
				}
				const ui::SwfColor want = ui::swf_apply(n.world_color, fill.value_or(ui::SwfColor{ 0, 0, 0, 255 }));
				if (fill && want.a == 0) {
					continue;
				}
				const std::string path = n.path;
				s.jobs.push_back({ [&s, isolate, m, path]() {
									  s.small_view(m)->set_filters_enabled(false);
									  isolate(m, path);
								  },
						[&s, m, path, want, fill]() {
							const Img a = img_of(s.small_view(m)->capture_hud());
							const ui::SwfScene &scn = *s.small_view(m)->scene();
							const Expect e = expect_of(scn, s.fonts, scn.find(path), s.small_view(m)->stage_fit());
							const Box window{ kEdge, kEdge, double(s.small_size.x) - kEdge, double(s.small_size.y) - kEdge };
							s.small_view(m)->set_filters_enabled(true);
							const auto [px, count] = dominant_pixel(a);
							if (count == 0 && e.box.intersect(window).empty()) {
								return;
							}
							const double alpha = fill ? double(want.a) : double(px[3]);
							const std::array<double, 4> premul{ double(want.r) * alpha / 255.0, double(want.g) * alpha / 255.0, double(want.b) * alpha / 255.0, alpha };
							double worst = 0.0;
							for (int c = 0; c < 4; ++c) {
								worst = std::max(worst, std::abs(double(px[size_t(c)]) - premul[size_t(c)]));
							}
							++s.color.checked;
							if (count == 0 || worst > double(kColorLevels)) {
								s.color.fail(fmt("%s %s %s drawn premultiplied %u,%u,%u,%u over %u px, swf_apply %02x%02x%02x premultiplied %.1f,%.1f,%.1f,%.0f", kMovies[m],
										path.c_str(), fill ? fmt("solid fill %02x%02x%02x%02x", fill->r, fill->g, fill->b, fill->a).c_str() : "fill under an add-only colour",
										px[0], px[1], px[2], px[3], count, want.r, want.g, want.b, premul[0], premul[1], premul[2], premul[3]));
							} else {
								++(fill ? s.color_solid : s.color_add_only);
							}
						} });
			}
		}
		s.jobs.push_back({ []() {}, [&s]() {
							  s.color.detail = fmt("%u solid fills, %u fills under an add-only colour matched", s.color_solid, s.color_add_only) +
									  (s.color.detail.empty() ? "" : "; " + s.color.detail);
							  s.add("colour transform add: drawn fills = swf_apply of the world colour transform", s.color);
						  } });

		{
			const ui::SwfScene &sc = *s.small_view(0)->scene();
			std::string add_path;
			for (const ui::SwfNodeId id : sc.draw_order()) {
				const ui::SwfSceneNode &n = *sc.node(id);
				if (add_path.empty() && n.blend_mode == ui::swf_blend::kAdd && n.world_visible && alpha_visible(n) && !under_mask(sc, n)) {
					add_path = n.path;
				}
			}
			const auto light_without_alpha = [](const Img &p_img, uint32_t &r_light) {
				uint32_t bare = 0;
				r_light = 0;
				for (size_t i = 0; i + 3 < p_img.px.size(); i += 4) {
					const int32_t rgb = std::max({ p_img.px[i], p_img.px[i + 1], p_img.px[i + 2] });
					r_light += rgb > kColorLevels ? 1 : 0;
					bare += rgb > int32_t(p_img.px[i + 3]) + kColorLevels ? 1 : 0;
				}
				return bare;
			};
			s.jobs.push_back({ [isolate, add_path]() { isolate(0, add_path); },
					[&s, add_path, light_without_alpha]() {
						uint32_t light = 0;
						s.additive_bare = light_without_alpha(img_of(s.small_view(0)->capture_hud()), light);
						s.additive_light = light;
						s.small_view(0)->set_additive_coverage(true);
					} });
			s.jobs.push_back({ []() {}, [&s, add_path, light_without_alpha]() {
								  uint32_t light = 0;
								  const uint32_t bare = light_without_alpha(img_of(s.small_view(0)->capture_hud()), light);
								  s.small_view(0)->set_additive_coverage(false);
								  s.add("add blend coverage: the layer pass carries the additive light in alpha", !add_path.empty() && s.additive_bare > 0 && bare == 0 && light > 0,
										  fmt("%s: %u lit px; drawn %u lit px without alpha, coverage pass %u lit px, %u without alpha", add_path.c_str(), s.additive_light,
												  s.additive_bare, light, bare));
							  } });
		}

		for (int m = 0; m < 2; ++m) {
			const ui::SwfScene &sc = *s.small_view(m)->scene();
			for (const ui::SwfNodeId id : sc.draw_order()) {
				const ui::SwfSceneNode &n = *sc.node(id);
				bool filtered = false;
				for (const ui::SwfFilter &f : n.filters) {
					filtered = filtered || supported(f);
				}
				if (!filtered || !n.world_visible || !alpha_visible(n) || under_mask(sc, n)) {
					continue;
				}
				const std::string path = n.path;
				s.jobs.push_back({ [isolate, m, path]() { isolate(m, path); },
						[&s, m, path]() {
							const Img a = img_of(s.small_view(m)->capture_hud());
							State::Measured &on = s.filtered[m][path];
							on.small = coverage(a);
							on.ink_small = ink(a);
						} });
				s.jobs.push_back({ [&s, m]() {
									  s.small_view(m)->set_filters_enabled(false);
									  s.big_view(m)->set_filters_enabled(false);
								  },
						[&s, m, path]() {
							const Img a = img_of(s.small_view(m)->capture_hud());
							State::Measured &on = s.filtered[m][path];
							const Box off = coverage(a);
							const double ink_off = ink(a);
							const ui::SwfScene &scn = *s.small_view(m)->scene();
							const Expect e = expect_of(scn, s.fonts, scn.find(path), s.small_view(m)->stage_fit());
							const bool changed = std::abs(on.ink_small - ink_off) > 0.5 || !on.small.near(off, 0.5);
							const bool pass = e.points.empty() ? on.small.empty() && off.empty() : changed && off.inside(on.small, kEdge) && on.small.inside(off.grown(e.margin), kEdge);
							s.add(fmt("filter pass %s %s", kMovies[m], path.c_str()), pass,
									fmt("ink with filters %.1f, without %.1f; bounds with %s without %s (margin %.1f)", on.ink_small, ink_off, on.small.str().c_str(),
											off.str().c_str(), e.margin));
							s.small_view(m)->set_filters_enabled(true);
							s.big_view(m)->set_filters_enabled(true);
						} });
			}
		}

		{
			const ui::SwfScene &sc = *s.small_view(1)->scene();
			for (const ui::SwfNodeId id : sc.draw_order()) {
				const ui::SwfSceneNode &mask = *sc.node(id);
				if (mask.clip_depth == 0 || under_mask(sc, *sc.node(mask.parent))) {
					continue;
				}
				const ui::SwfSceneNode &parent = *sc.node(mask.parent);
				std::string maskee;
				for (const ui::SwfNodeId c : parent.children) {
					const ui::SwfSceneNode &k = *sc.node(c);
					if (k.depth > mask.depth && k.depth <= int32_t(mask.clip_depth) && k.world_visible && alpha_visible(k)) {
						maskee = k.path;
						break;
					}
				}
				if (maskee.empty()) {
					continue;
				}
				const std::string mask_path = mask.path;
				s.jobs.push_back({ [isolate, maskee]() { isolate(1, maskee); },
						[&s, maskee, mask_path]() {
							const Img a = img_of(s.small_view(1)->capture_hud());
							const Box drawn = coverage(a);
							const ui::SwfScene &scn = *s.small_view(1)->scene();
							const SwfStageFit fit = s.small_view(1)->stage_fit();
							const Expect content = expect_of(scn, s.fonts, scn.find(maskee), fit);
							const Expect clip = expect_of(scn, s.fonts, scn.find(mask_path), fit, true);
							const bool cuts = !content.box.inside(clip.box, kEdge);
							const Box inner = clip.box.grown(-kEdge).intersect(Box{ kEdge, kEdge, double(s.small_size.x) - kEdge, double(s.small_size.y) - kEdge });
							size_t inside = 0;
							for (const auto &p : content.points) {
								inside += inner.contains(p[0], p[1]) ? 1 : 0;
							}
							const bool pass = (inside == 0 || !drawn.empty()) && drawn.inside(clip.box, kEdge) && drawn.inside(content.box, kEdge) &&
									(!cuts || !content.box.inside(drawn, kEdge));
							s.mask_drawn += drawn.empty() ? 0 : 1;
							s.mask_cut += cuts && !drawn.empty() ? 1 : 0;
							s.add(fmt("clip-depth mask %s over %s", mask_path.c_str(), maskee.c_str()), pass,
									fmt("drawn %s, mask %s, content %s, %zu content vertices inside the mask%s", drawn.str().c_str(), clip.box.str().c_str(),
											content.box.str().c_str(), inside, cuts ? " (mask cuts the content)" : ""));
						} });
			}
		}

		s.jobs.push_back({ []() {}, [&s]() {
							  s.add("clip-depth masks draw and cut", s.mask_drawn > 0 && s.mask_cut > 0,
									  fmt("%u mask groups drew their content, %u of them cut it", s.mask_drawn, s.mask_cut));
						  } });

		s.jobs.push_back({ [&s, isolate]() {
							  const bf3::core::Status a = s.small_view(0)->set_tilt(kTiltPath, SwfTilt{ kAs2TiltX, kAs2TiltY, kAs2Perspective });
							  (void)a;
							  isolate(0, kTiltPath);
						  },
				[&s]() {
					const Img tilted = img_of(s.small_view(0)->capture_hud());
					const ui::SwfScene &sc = *s.small_view(0)->scene();
					const ui::SwfSceneNode &n = *sc.find_node(kTiltPath);
					const SwfStageFit fit = s.small_view(0)->stage_fit();
					const std::array<double, 9> h = independent_tilt(n.world.tx, n.world.ty, sc.stage(), fit);
					const Img expected = warp(s.tilt_base, h);
					const Box tb = coverage(tilted);
					const Box eb = coverage(expected);
					Box both = tb;
					both.add(eb);
					double diff = 0.0;
					double count = 0.0;
					for (int32_t y = int32_t(std::max(0.0, both.y0)); y < int32_t(std::min(double(tilted.h), both.y1)); ++y) {
						for (int32_t x = int32_t(std::max(0.0, both.x0)); x < int32_t(std::min(double(tilted.w), both.x1)); ++x) {
							for (int c = 0; c < 4; ++c) {
								diff += std::abs(double(tilted.at(x, y, c)) - double(expected.at(x, y, c))) / 255.0;
							}
							count += 4.0;
						}
					}
					const double mean = count > 0.0 ? diff / count : 1.0;
					const Box base = coverage(s.tilt_base);
					const bool pass = !tb.empty() && tb.near(eb, kEdge) && mean <= kTiltMeanDiff && !tb.near(base, kEdge);
					s.add("3D tilt homography (Ammo_01 -15/15/25 per Widget.Hud.Ammo onClipLoad)", pass,
							fmt("drawn %s, independent projection %s, untilted %s, mean |diff| %.4f (limit %.2f)", tb.str().c_str(), eb.str().c_str(), base.str().c_str(),
									mean, kTiltMeanDiff));
					const bf3::core::Status a = s.small_view(0)->clear_tilt(kTiltPath);
					(void)a;
				} });

		for (size_t t = 0; t < kTargets.size(); ++t) {
			const int m = kTargets[t].first;
			const std::string path = kTargets[t].second;
			const std::string renderer = kTargetRenderers[t];
			const ui::SwfScene &sc = *s.small_view(m)->scene();
			if (sc.find(path) == ui::kSwfNoNode) {
				s.add("engine target " + renderer, false, "no node at " + path);
				continue;
			}
			s.jobs.push_back({ [&s, isolate, m, path, renderer]() {
								  const bf3::core::Status a = s.small_view(m)->set_renderer_string(path, renderer);
								  const bf3::core::Status b = s.big_view(m)->set_renderer_string(path, renderer);
								  (void)a;
								  (void)b;
								  isolate(m, path);
							  },
					[&s, m, path, renderer]() {
						const Img a = img_of(s.small_view(m)->capture_hud());
						const std::vector<SwfEngineTarget> ts = s.small_view(m)->engine_targets(renderer);
						const ui::SwfScene &scn = *s.small_view(m)->scene();
						const ui::SwfSceneNode &n = *scn.find_node(path);
						Window2 w{ 1.0, 1.0, 0.0, 0.0 };
						Expect local;
						ui::SwfMatrix inv;
						const double det = double(n.world.a) * n.world.d - double(n.world.b) * n.world.c;
						inv.a = float(n.world.d / det);
						inv.b = float(-n.world.b / det);
						inv.c = float(-n.world.c / det);
						inv.d = float(n.world.a / det);
						inv.tx = float(-(inv.a * n.world.tx + inv.c * n.world.ty));
						inv.ty = float(-(inv.b * n.world.tx + inv.d * n.world.ty));
						for (const ui::SwfNodeId c : n.children) {
							const ui::SwfSceneNode &k = *scn.node(c);
							const ui::SwfMatrix rel = ui::swf_concat(inv, k.world);
							expect_node(scn, s.fonts, c, w, true, 0.0, &rel, local);
						}
						const bool one = ts.size() == 1;
						const SwfEngineTarget t = one ? ts.front() : SwfEngineTarget{};
						const Box tb{ t.local_bounds.x_min, t.local_bounds.y_min, t.local_bounds.x_max, t.local_bounds.y_max };
						const double k = s.small_view(m)->stage_fit().scale_x;
						const bool sized = one && t.size.x == int32_t(std::ceil(double(t.local_bounds.width()) * std::hypot(n.world.a, n.world.b) * k - 1e-3)) &&
								t.texture.is_valid() && t.canvas_item.is_valid();
						const bool pass = one && ink(a) == 0.0 && tb.near(local.box, 0.01) && sized && s.small_view(m)->stats().engine_targets >= 1;
						s.add("engine target " + renderer, pass,
								fmt("%zu target(s) at %s; drawn ink %.1f; local bounds %s vs placeholder geometry %s; texture %dx%d", ts.size(), path.c_str(), ink(a),
										tb.str().c_str(), local.box.str().c_str(), t.size.x, t.size.y));
					} });
		}
		s.jobs.push_back({ [&s]() {
							  for (Bf3SwfView *v : s.views) {
								  v->clear_isolation();
							  }
						  },
				[&s]() {
					const Img a = img_of(s.small_view(1)->capture_hud());
					const ui::SwfScene &scn = *s.small_view(1)->scene();
					const Expect e = expect_of(scn, s.fonts, scn.find(kTargets[0].second), s.small_view(1)->stage_fit());
					double inside = 0.0;
					for (int32_t y = int32_t(std::max(0.0, e.box.y0)); y < int32_t(std::min(double(a.h), e.box.y1)); ++y) {
						for (int32_t x = int32_t(std::max(0.0, e.box.x0)); x < int32_t(std::min(double(a.w), e.box.x1)); ++x) {
							inside += a.at(x, y, 3);
						}
					}
					s.add("engine target leaves the rest of the HUD drawn", ink(a) > 0.0 && s.small_view(1)->stats().engine_targets == 2,
							fmt("hudtdmscreen ink %.1f with %u engine targets; alpha over the iBgDraw placeholder %.0f", ink(a), s.small_view(1)->stats().engine_targets,
									inside / 255.0));
					for (size_t t = 0; t < kTargets.size(); ++t) {
						for (int i : { kTargets[t].first, 2 + kTargets[t].first }) {
							if (s.views[size_t(i)]->scene()->find(kTargets[t].second) != ui::kSwfNoNode) {
								const bf3::core::Status st = s.views[size_t(i)]->set_renderer_string(kTargets[t].second, "t2");
								(void)st;
							}
						}
					}
				} });
		s.jobs.push_back({ [this, &s]() {
							  s.transmit = memnew(Bf3SwfView);
							  s.transmit->set_playing(false);
							  add_child(s.transmit);
							  const bf3::core::Status st = s.transmit->open(transmit_library(), kTransmitMovie);
							  s.add("open the synthetic transmittance movie", bool(st), st ? std::string("opened") : st.error().message);
						  },
				[]() {} });
		s.jobs.push_back({ []() {},
				[&s]() {
					const Ref<Image> colour = s.transmit->is_open() ? s.transmit->capture_hud() : Ref<Image>();
					const Ref<Image> trans = swf_view_transmittance(std::span<const Ref<Image>>(&colour, 1));
					const Img lit = img_of(colour);
					bool pass = trans.is_valid() && trans->get_format() == Image::FORMAT_L8 && lit.w == trans->get_width() && lit.h == trans->get_height();
					std::string detail = trans.is_valid() ? fmt("%dx%d L8", trans->get_width(), trans->get_height()) : std::string("no transmittance image");
					for (const TransmitProbe &p : kTransmitProbes) {
						if (!pass || p.x >= lit.w || p.y >= lit.h) {
							pass = false;
							break;
						}
						const int32_t got = int32_t(std::lround(trans->get_pixel(p.x, p.y).r * 255.0f));
						pass = pass && std::abs(got - int32_t(p.want)) <= p.tolerance;
						detail += fmt("; %s %d (want %u)", p.what, got, p.want);
					}
					const bool add_lit = pass && lit.at(kTransmitProbes[1].x, kTransmitProbes[1].y, 0) > 250;
					s.add("transmittance: add pieces keep the world, normal pieces multiply it by 1 - alpha", pass && add_lit,
							detail + fmt("; add piece colour %u", pass ? lit.at(kTransmitProbes[1].x, kTransmitProbes[1].y, 0) : 0));
				} });
		s.phase = 3;
		s.job = 0;
		s.job_armed = false;
		return;
	}
	if (s.phase == 3) {
		if (s.job_armed) {
			s.jobs[s.job].measure();
			++s.job;
			s.job_armed = false;
		}
		if (s.job < s.jobs.size()) {
			s.jobs[s.job].setup();
			s.job_armed = true;
			return;
		}
		for (int m = 0; m < 2; ++m) {
			const ui::SwfScene &sc = *s.small_view(m)->scene();
			for (const ui::SwfNodeId id : sc.draw_order()) {
				const ui::SwfSceneNode &n = *sc.node(id);
				if (n.edit_text != nullptr && int32_t(s.perf_fields.size()) < kPerfTextFields * (m + 1) && n.world_visible) {
					s.perf_fields.emplace_back(m, n.path);
				}
			}
		}
		s.phase = 4;
		s.perf_frame = 0;
		for (int m = 0; m < 2; ++m) {
			s.perf_structure0[m] = s.small_view(m)->stats().structure_passes;
			s.perf_builds0[m] = s.small_view(m)->stats().content_builds;
		}
		return;
	}
	if (s.phase == 4) {
		if (s.perf_frame > 0) {
			s.perf_ms.push_back((s.small_view(0)->stats().sync_seconds + s.small_view(1)->stats().sync_seconds) * 1000.0);
		}
		if (s.perf_frame < kPerfFrames) {
			for (size_t i = 0; i < s.perf_fields.size(); ++i) {
				const auto &[m, path] = s.perf_fields[i];
				const bf3::core::Status st = s.small_view(m)->scene()->set_text(path, std::to_string((s.perf_frame * 7 + int(i) * 13) % 1000));
				(void)st;
			}
			for (int m = 0; m < 2; ++m) {
				const bf3::core::Status st = s.small_view(m)->scene()->advance(1);
				(void)st;
			}
			++s.perf_frame;
			return;
		}
		std::vector<double> sorted = s.perf_ms;
		std::sort(sorted.begin(), sorted.end());
		double sum = 0.0;
		for (double v : sorted) {
			sum += v;
		}
		const double mean = sorted.empty() ? 0.0 : sum / double(sorted.size());
		const double p95 = sorted.empty() ? 0.0 : sorted[std::min(sorted.size() - 1, size_t(std::ceil(0.95 * double(sorted.size()))) - 1)];
		const SwfViewStats &a = s.small_view(0)->stats();
		const SwfViewStats &b = s.small_view(1)->stats();
		s.add("incremental sync CPU time", sorted.size() == size_t(kPerfFrames) && a.structure_passes > 0,
				fmt("hudmpscreen + hudtdmscreen at 1280x720, %zu text fields set and one movie frame per frame: mean %.3f ms, p95 %.3f ms, max %.3f ms over %zu "
					"frames; last frame refreshed %u + %u nodes, %u + %u viewports; %u + %u structure passes and %u + %u content builds during the run",
						s.perf_fields.size(), mean, p95, sorted.empty() ? 0.0 : sorted.back(), sorted.size(), a.nodes_refreshed, b.nodes_refreshed, a.viewports_updated,
						b.viewports_updated, a.structure_passes - s.perf_structure0[0], b.structure_passes - s.perf_structure0[1], a.content_builds - s.perf_builds0[0],
						b.content_builds - s.perf_builds0[1]));
		finish();
	}
}

}
