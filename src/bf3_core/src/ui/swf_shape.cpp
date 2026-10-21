#include "swf_shape_detail.h"

#include "bf3/core/ui/swf_geometry.h"
#include "bf3/core/util/format.h"

#include <algorithm>
#include <string>

namespace bf3::core::ui {

namespace detail {

namespace {

struct ShapeEdge {
	int64_t x0 = 0;
	int64_t y0 = 0;
	int64_t cx = 0;
	int64_t cy = 0;
	int64_t x1 = 0;
	int64_t y1 = 0;
	bool curve = false;
	uint32_t fill0 = 0;
	uint32_t fill1 = 0;
	uint32_t line = 0;
};

struct Directed {
	uint32_t edge = 0;
	bool reversed = false;
};

struct PointKey {
	int64_t x = 0;
	int64_t y = 0;
	auto operator<=>(const PointKey &) const = default;
};

SwfColor read_color(SwfReader &r, bool p_alpha) {
	SwfColor c;
	c.r = r.u8();
	c.g = r.u8();
	c.b = r.u8();
	c.a = p_alpha ? r.u8() : 255;
	return c;
}

SwfPoint px(int64_t p_x, int64_t p_y) {
	return { float(double(p_x) / 20.0), float(double(p_y) / 20.0) };
}

class ShapeReader {
public:
	ShapeReader(SwfReader &r, int p_version, WorkBudget &r_budget, SwfShape &r_shape) :
			r_(r), v_(p_version), budget_(r_budget), s_(r_shape) {}

	Status run() {
		s_.version = uint8_t(v_);
		s_.id = r_.u16();
		s_.bounds = r_.rect();
		s_.edge_bounds = s_.bounds;
		if (v_ == 4) {
			s_.edge_bounds = r_.rect();
			s_.shape4_flags = uint8_t(r_.u8() & 0x07);
		}
		BF3_TRY(style_tables());
		BF3_TRY(records());
		if (r_.failed()) {
			return Status::success();
		}
		BF3_TRY(fill_meshes());
		BF3_TRY(line_strokes());
		return Status::success();
	}

private:
	Error fail(const std::string &p_what) const { return make_error("shape " + std::to_string(s_.id) + ": " + p_what); }

	Status style_tables() {
		size_t n = r_.u8();
		if (n == 0xff && v_ >= 2) {
			n = r_.u16();
		}
		if (r_.failed() || n * 3 > r_.left()) {
			r_.fail();
			return Status::success();
		}
		BF3_TRY(budget_.charge(1 + n));
		fill_base_ = uint32_t(s_.fills.size());
		fill_count_ = uint32_t(n);
		for (size_t i = 0; i < n && !r_.failed(); ++i) {
			SwfFillStyle f;
			BF3_TRY(read_fill_style(r_, v_, f));
			s_.fills.push_back(std::move(f));
		}
		size_t lines = r_.u8();
		if (lines == 0xff) {
			lines = r_.u16();
		}
		if (r_.failed() || lines * 5 > r_.left()) {
			r_.fail();
			return Status::success();
		}
		BF3_TRY(budget_.charge(1 + lines));
		line_base_ = uint32_t(s_.lines.size());
		line_count_ = uint32_t(lines);
		for (size_t i = 0; i < lines && !r_.failed(); ++i) {
			BF3_TRY(line_style());
		}
		s_.style_tables++;
		const uint8_t nb = r_.u8();
		fill_bits_ = nb >> 4;
		line_bits_ = nb & 0x0f;
		return Status::success();
	}

	Status line_style() {
		SwfLineStyle l;
		l.width = float(r_.u16()) / 20.0f;
		if (v_ == 4) {
			l.start_cap = uint8_t(r_.ub(2));
			l.join = uint8_t(r_.ub(2));
			l.has_fill = r_.ub(1) != 0;
			l.no_hscale = r_.ub(1) != 0;
			l.no_vscale = r_.ub(1) != 0;
			l.pixel_hinting = r_.ub(1) != 0;
			r_.ub(5);
			l.no_close = r_.ub(1) != 0;
			l.end_cap = uint8_t(r_.ub(2));
			if (l.join == 2) {
				l.miter_limit = float(r_.u16()) / 256.0f;
			}
			if (l.has_fill) {
				BF3_TRY(read_fill_style(r_, v_, l.fill));
				l.color = l.fill.color;
			} else {
				l.color = read_color(r_, true);
			}
		} else {
			l.color = read_color(r_, v_ >= 3);
		}
		s_.lines.push_back(std::move(l));
		return Status::success();
	}

	Status pick(uint32_t p_index, uint32_t p_base, uint32_t p_count, const char *p_what, uint32_t &r_selected) {
		if (p_index > p_count) {
			return fail(std::string(p_what) + " style " + std::to_string(p_index) + " past the " + std::to_string(p_count) + " styles of its table");
		}
		r_selected = p_index == 0 ? 0 : p_base + p_index;
		return Status::success();
	}

	Status records() {
		int64_t x = 0;
		int64_t y = 0;
		uint32_t fill0 = 0;
		uint32_t fill1 = 0;
		uint32_t line = 0;
		while (!r_.failed()) {
			BF3_TRY(budget_.charge(1));
			if (r_.ub(1) == 0) {
				const uint32_t flags = r_.ub(5);
				if (flags == 0) {
					break;
				}
				if ((flags & 0x01) != 0) {
					const unsigned n = r_.ub(5);
					x = r_.sb(n);
					y = r_.sb(n);
				}
				uint32_t f0 = 0;
				uint32_t f1 = 0;
				uint32_t ls = 0;
				if ((flags & 0x02) != 0) {
					f0 = r_.ub(fill_bits_);
				}
				if ((flags & 0x04) != 0) {
					f1 = r_.ub(fill_bits_);
				}
				if ((flags & 0x08) != 0) {
					ls = r_.ub(line_bits_);
				}
				if ((flags & 0x10) != 0) {
					if (v_ < 2) {
						r_.fail();
						break;
					}
					BF3_TRY(style_tables());
					fill0 = 0;
					fill1 = 0;
					line = 0;
				}
				if ((flags & 0x02) != 0) {
					BF3_TRY(pick(f0, fill_base_, fill_count_, "fill0", fill0));
				}
				if ((flags & 0x04) != 0) {
					BF3_TRY(pick(f1, fill_base_, fill_count_, "fill1", fill1));
				}
				if ((flags & 0x08) != 0) {
					BF3_TRY(pick(ls, line_base_, line_count_, "line", line));
				}
				continue;
			}
			ShapeEdge e;
			e.x0 = x;
			e.y0 = y;
			e.fill0 = fill0;
			e.fill1 = fill1;
			e.line = line;
			const bool straight = r_.ub(1) != 0;
			const unsigned n = r_.ub(4) + 2;
			if (straight) {
				if (r_.ub(1) != 0) {
					x += r_.sb(n);
					y += r_.sb(n);
				} else if (r_.ub(1) != 0) {
					y += r_.sb(n);
				} else {
					x += r_.sb(n);
				}
			} else {
				e.curve = true;
				e.cx = x + r_.sb(n);
				e.cy = y + r_.sb(n);
				x = e.cx + r_.sb(n);
				y = e.cy + r_.sb(n);
			}
			e.x1 = x;
			e.y1 = y;
			if (r_.failed()) {
				break;
			}
			edges_.push_back(e);
			s_.edges++;
		}
		r_.align();
		return Status::success();
	}

	Status flatten(const ShapeEdge &p_e, bool p_reversed, std::vector<SwfPoint> &r_points) {
		const SwfPoint from = p_reversed ? px(p_e.x1, p_e.y1) : px(p_e.x0, p_e.y0);
		const SwfPoint to = p_reversed ? px(p_e.x0, p_e.y0) : px(p_e.x1, p_e.y1);
		if (!p_e.curve) {
			r_points.push_back(to);
		} else {
			const SwfPoint c = px(p_e.cx, p_e.cy);
			const uint32_t n = swf_curve_segments(from, c, to);
			for (uint32_t i = 1; i <= n; ++i) {
				const double t = double(i) / double(n);
				const double u = 1.0 - t;
				r_points.push_back({ float(u * u * from.x + 2.0 * u * t * c.x + t * t * to.x), float(u * u * from.y + 2.0 * u * t * c.y + t * t * to.y) });
			}
		}
		points_ += p_e.curve ? kSwfCurveMaxSegments : 1;
		if (points_ > kSwfShapeMaxPoints) {
			return fail("more than " + std::to_string(kSwfShapeMaxPoints) + " flattened points");
		}
		return budget_.charge(1);
	}

	PointKey start_of(const Directed &p_d) const {
		const ShapeEdge &e = edges_[p_d.edge];
		return p_d.reversed ? PointKey{ e.x1, e.y1 } : PointKey{ e.x0, e.y0 };
	}
	PointKey end_of(const Directed &p_d) const {
		const ShapeEdge &e = edges_[p_d.edge];
		return p_d.reversed ? PointKey{ e.x0, e.y0 } : PointKey{ e.x1, e.y1 };
	}

	Status fill_meshes() {
		std::vector<std::vector<Directed>> per_fill(s_.fills.size());
		for (uint32_t i = 0; i < edges_.size(); ++i) {
			const ShapeEdge &e = edges_[i];
			if (e.fill0 == e.fill1) {
				continue;
			}
			if (e.fill1 != 0) {
				per_fill[e.fill1 - 1].push_back({ i, false });
			}
			if (e.fill0 != 0) {
				per_fill[e.fill0 - 1].push_back({ i, true });
			}
		}
		for (uint32_t f = 0; f < per_fill.size(); ++f) {
			if (per_fill[f].empty()) {
				continue;
			}
			SwfFillMesh mesh;
			mesh.fill = f;
			BF3_TRY(chain(per_fill[f], mesh));
			SwfTriangulation t;
			Status st = swf_triangulate_nonzero(mesh.outline, mesh.contour_ends, budget_, t);
			if (!st) {
				return fail("fill " + std::to_string(f) + ": " + st.error().message);
			}
			mesh.vertices = std::move(t.vertices);
			mesh.indices = std::move(t.indices);
			s_.meshes.push_back(std::move(mesh));
		}
		return Status::success();
	}

	Status chain(const std::vector<Directed> &p_list, SwfFillMesh &r_mesh) {
		std::vector<std::pair<PointKey, uint32_t>> starts;
		starts.reserve(p_list.size());
		for (uint32_t i = 0; i < p_list.size(); ++i) {
			starts.push_back({ start_of(p_list[i]), i });
		}
		std::sort(starts.begin(), starts.end());
		std::vector<bool> used(p_list.size(), false);
		for (uint32_t first = 0; first < p_list.size(); ++first) {
			if (used[first]) {
				continue;
			}
			const PointKey origin = start_of(p_list[first]);
			const ShapeEdge &e0 = edges_[p_list[first].edge];
			r_mesh.outline.push_back(p_list[first].reversed ? px(e0.x1, e0.y1) : px(e0.x0, e0.y0));
			const size_t contour_begin = r_mesh.outline.size() - 1;
			uint32_t cur = first;
			bool closed = false;
			while (true) {
				used[cur] = true;
				BF3_TRY(flatten(edges_[p_list[cur].edge], p_list[cur].reversed, r_mesh.outline));
				const PointKey at = end_of(p_list[cur]);
				if (at == origin) {
					closed = true;
					break;
				}
				uint32_t next = UINT32_MAX;
				if (cur + 1 < p_list.size() && !used[cur + 1] && start_of(p_list[cur + 1]) == at) {
					next = cur + 1;
				} else {
					auto it = std::lower_bound(starts.begin(), starts.end(), std::pair<PointKey, uint32_t>{ at, 0 });
					for (; it != starts.end() && it->first == at; ++it) {
						if (!used[it->second]) {
							next = it->second;
							break;
						}
					}
				}
				if (next == UINT32_MAX) {
					break;
				}
				cur = next;
			}
			if (closed && r_mesh.outline.size() - contour_begin > 1) {
				r_mesh.outline.pop_back();
			} else if (!closed) {
				r_mesh.open_contours++;
			}
			r_mesh.contour_ends.push_back(uint32_t(r_mesh.outline.size()));
		}
		return Status::success();
	}

	Status line_strokes() {
		std::vector<int64_t> last(s_.lines.size(), -1);
		std::vector<int32_t> stroke_of(s_.lines.size(), -1);
		for (uint32_t i = 0; i < edges_.size(); ++i) {
			const ShapeEdge &e = edges_[i];
			if (e.line == 0) {
				continue;
			}
			const uint32_t l = e.line - 1;
			if (stroke_of[l] < 0) {
				stroke_of[l] = int32_t(s_.strokes.size());
				SwfStroke st;
				st.line = l;
				s_.strokes.push_back(std::move(st));
			}
			SwfStroke &st = s_.strokes[size_t(stroke_of[l])];
			const bool continues = last[l] >= 0 && edges_[size_t(last[l])].x1 == e.x0 && edges_[size_t(last[l])].y1 == e.y0;
			if (!continues) {
				close_path(st);
				st.points.push_back(px(e.x0, e.y0));
			}
			BF3_TRY(flatten(e, false, st.points));
			last[l] = int64_t(i);
		}
		for (SwfStroke &st : s_.strokes) {
			close_path(st);
		}
		for (SwfStroke &st : s_.strokes) {
			uint32_t begin = 0;
			for (const uint32_t end : st.path_ends) {
				st.closed.push_back(end - begin > 2 && st.points[begin] == st.points[end - 1]);
				begin = end;
			}
		}
		return Status::success();
	}

	void close_path(SwfStroke &r_stroke) {
		const uint32_t n = uint32_t(r_stroke.points.size());
		if (n > 0 && (r_stroke.path_ends.empty() || r_stroke.path_ends.back() != n)) {
			r_stroke.path_ends.push_back(n);
		}
	}

	SwfReader &r_;
	const int v_;
	WorkBudget &budget_;
	SwfShape &s_;
	std::vector<ShapeEdge> edges_;
	uint32_t fill_base_ = 0;
	uint32_t fill_count_ = 0;
	uint32_t line_base_ = 0;
	uint32_t line_count_ = 0;
	unsigned fill_bits_ = 0;
	unsigned line_bits_ = 0;
	uint64_t points_ = 0;
};

}

Status read_fill_style(SwfReader &r, int p_version, SwfFillStyle &r_fill) {
	r_fill.type = r.u8();
	switch (r_fill.type) {
		case 0x00:
			r_fill.kind = SwfFillKind::Solid;
			r_fill.color = read_color(r, p_version >= 3);
			return Status::success();
		case 0x10:
		case 0x12:
		case 0x13: {
			r_fill.kind = r_fill.type == 0x10 ? SwfFillKind::LinearGradient : (r_fill.type == 0x12 ? SwfFillKind::RadialGradient : SwfFillKind::FocalGradient);
			r_fill.matrix = r.matrix();
			r_fill.spread = uint8_t(r.ub(2));
			r_fill.interpolation = uint8_t(r.ub(2));
			const uint32_t n = r.ub(4);
			for (uint32_t i = 0; i < n && !r.failed(); ++i) {
				SwfGradientStop s;
				s.ratio = r.u8();
				s.color = read_color(r, p_version >= 3);
				r_fill.stops.push_back(s);
			}
			if (r_fill.type == 0x13) {
				r_fill.focal = float(r.s16()) / 256.0f;
			}
			return Status::success();
		}
		case SwfFillStyle::kRepeatingBitmap:
		case SwfFillStyle::kClippedBitmap:
		case SwfFillStyle::kRepeatingBitmapNoSmoothing:
		case SwfFillStyle::kClippedBitmapNoSmoothing:
			r_fill.kind = SwfFillKind::Bitmap;
			r_fill.bitmap_id = r.u16();
			r_fill.matrix = r.matrix();
			return Status::success();
		default:
			if (r.failed()) {
				return Status::success();
			}
			return make_error(fmt("fill type 0x%02x is not a SWF fill type", unsigned(r_fill.type)));
	}
}

Status read_shape(SwfReader &r, int p_version, WorkBudget &r_budget, SwfShape &r_shape) {
	ShapeReader reader(r, p_version, r_budget, r_shape);
	return reader.run();
}

}

const char *swf_fill_kind_name(SwfFillKind p_kind) {
	switch (p_kind) {
		case SwfFillKind::Solid: return "solid";
		case SwfFillKind::LinearGradient: return "linear_gradient";
		case SwfFillKind::RadialGradient: return "radial_gradient";
		case SwfFillKind::FocalGradient: return "focal_gradient";
		case SwfFillKind::Bitmap: return "bitmap";
	}
	return "?";
}

SwfMatrix SwfFillStyle::texel_to_shape() const {
	SwfMatrix m = matrix;
	m.a /= 20.0f;
	m.b /= 20.0f;
	m.c /= 20.0f;
	m.d /= 20.0f;
	return m;
}

const SwfFillMesh *SwfShape::mesh_of(uint32_t p_fill) const {
	for (const SwfFillMesh &m : meshes) {
		if (m.fill == p_fill) {
			return &m;
		}
	}
	return nullptr;
}

}
