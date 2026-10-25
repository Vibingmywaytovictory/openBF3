#include "bf3/core/ui/swf_geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace bf3::core::ui {

namespace {

struct SweepEdge {
	double x0 = 0.0;
	double y0 = 0.0;
	double y1 = 0.0;
	double slope = 0.0;
	int wind = 0;

	double x_at(double p_y) const { return x0 + (p_y - y0) * slope; }
};

constexpr uint32_t kNoTrapezoid = 0xffffffffu;

struct OpenTrapezoid {
	uint32_t left = 0;
	uint32_t right = 0;
	double top = 0.0;
	double bottom = 0.0;
};

class Sweep {
public:
	Sweep(SwfFillRule p_rule, WorkBudget &r_budget, SwfTriangulation &r_out) :
			rule_(p_rule), budget_(r_budget), out_(r_out) {}

	Status run(std::span<const SwfPoint> p_outline, std::span<const uint32_t> p_ends) {
		BF3_TRY(collect(p_outline, p_ends));
		if (edges_.empty()) {
			return Status::success();
		}
		open_by_left_.assign(edges_.size(), kNoTrapezoid);
		std::vector<double> ys;
		ys.reserve(edges_.size() * 2);
		for (const SweepEdge &e : edges_) {
			ys.push_back(e.y0);
			ys.push_back(e.y1);
		}
		std::sort(ys.begin(), ys.end());
		ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
		std::vector<uint32_t> by_top(edges_.size());
		for (uint32_t i = 0; i < by_top.size(); ++i) {
			by_top[i] = i;
		}
		std::sort(by_top.begin(), by_top.end(), [&](uint32_t a, uint32_t b) { return edges_[a].y0 < edges_[b].y0; });
		size_t next = 0;
		for (size_t k = 0; k + 1 < ys.size(); ++k) {
			const double ya = ys[k];
			const double yb = ys[k + 1];
			active_.erase(std::remove_if(active_.begin(), active_.end(), [&](uint32_t e) { return edges_[e].y1 <= ya; }), active_.end());
			while (next < by_top.size() && edges_[by_top[next]].y0 <= ya) {
				active_.push_back(by_top[next++]);
			}
			BF3_TRY(band(ya, yb));
		}
		flush(std::numeric_limits<double>::infinity());
		return Status::success();
	}

private:
	Status collect(std::span<const SwfPoint> p_outline, std::span<const uint32_t> p_ends) {
		uint32_t begin = 0;
		for (const uint32_t end : p_ends) {
			if (end < begin || end > p_outline.size()) {
				return make_error("swf triangulate: contour end " + std::to_string(end) + " outside the " + std::to_string(p_outline.size()) + " points");
			}
			const uint32_t n = end - begin;
			for (uint32_t i = 0; n >= 2 && i < n; ++i) {
				const SwfPoint a = p_outline[begin + i];
				const SwfPoint b = p_outline[begin + (i + 1) % n];
				if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y)) {
					return make_error("swf triangulate: a point is not finite");
				}
				if (a.y == b.y) {
					continue;
				}
				SweepEdge e;
				const bool down = a.y < b.y;
				const SwfPoint top = down ? a : b;
				const SwfPoint bottom = down ? b : a;
				e.x0 = top.x;
				e.y0 = top.y;
				e.y1 = bottom.y;
				e.slope = (double(bottom.x) - double(top.x)) / (double(bottom.y) - double(top.y));
				e.wind = down ? 1 : -1;
				edges_.push_back(e);
			}
			begin = end;
		}
		return budget_.charge(1 + edges_.size());
	}

	Status band(double p_top, double p_bottom) {
		double y = p_top;
		while (y < p_bottom) {
			if (++out_.slabs > kSwfShapeMaxSlabs) {
				return make_error("swf triangulate: more than " + std::to_string(kSwfShapeMaxSlabs) + " slabs");
			}
			BF3_TRY(budget_.charge(1 + active_.size()));
			std::sort(active_.begin(), active_.end(), [&](uint32_t a, uint32_t b) {
				const double xa = edges_[a].x_at(y);
				const double xb = edges_[b].x_at(y);
				return xa != xb ? xa < xb : edges_[a].slope < edges_[b].slope;
			});
			double end = p_bottom;
			const double span = p_bottom - p_top;
			for (size_t i = 0; i + 1 < active_.size(); ++i) {
				const SweepEdge &l = edges_[active_[i]];
				const SweepEdge &r = edges_[active_[i + 1]];
				if (l.x_at(end) <= r.x_at(end) || l.slope == r.slope) {
					continue;
				}
				const double cross = (r.x0 - l.x0 + l.y0 * l.slope - r.y0 * r.slope) / (l.slope - r.slope);
				if (cross > y + span * 1e-9 && cross < end) {
					end = cross;
				}
			}
			if (end < p_bottom) {
				++out_.crossings;
			}
			emit_slab(y, end);
			y = end;
		}
		return Status::success();
	}

	void emit_slab(double p_top, double p_bottom) {
		const double mid = 0.5 * (p_top + p_bottom);
		std::sort(active_.begin(), active_.end(), [&](uint32_t a, uint32_t b) { return edges_[a].x_at(mid) < edges_[b].x_at(mid); });
		flush(p_top);
		for (uint32_t i = 0; i < open_.size(); ++i) {
			open_by_left_[open_[i].left] = i;
		}
		std::vector<OpenTrapezoid> next;
		int wind = 0;
		for (size_t i = 0; i + 1 < active_.size(); ++i) {
			wind += edges_[active_[i]].wind;
			if (rule_ == SwfFillRule::EvenOdd ? (wind & 1) == 0 : wind == 0) {
				continue;
			}
			const uint32_t l = active_[i];
			const uint32_t r = active_[i + 1];
			const uint32_t found = open_by_left_[l];
			if (found != kNoTrapezoid && open_[found].right == r && open_[found].bottom == p_top) {
				OpenTrapezoid t = open_[found];
				t.bottom = p_bottom;
				next.push_back(t);
				open_[found].bottom = -std::numeric_limits<double>::infinity();
			} else {
				next.push_back({ l, r, p_top, p_bottom });
			}
		}
		for (const OpenTrapezoid &t : open_) {
			open_by_left_[t.left] = kNoTrapezoid;
			if (t.bottom == p_top) {
				emit(t);
			}
		}
		open_ = std::move(next);
	}

	void flush(double p_keep_bottom) {
		std::vector<OpenTrapezoid> kept;
		for (const OpenTrapezoid &t : open_) {
			if (t.bottom == p_keep_bottom) {
				kept.push_back(t);
			} else if (t.bottom != -std::numeric_limits<double>::infinity()) {
				emit(t);
			}
		}
		open_ = std::move(kept);
	}

	void emit(const OpenTrapezoid &p_t) {
		const SweepEdge &l = edges_[p_t.left];
		const SweepEdge &r = edges_[p_t.right];
		const double tl = l.x_at(p_t.top);
		const double tr = r.x_at(p_t.top);
		const double bl = l.x_at(p_t.bottom);
		const double br = r.x_at(p_t.bottom);
		const bool top_wide = tr > tl;
		const bool bottom_wide = br > bl;
		if (!top_wide && !bottom_wide) {
			return;
		}
		const uint32_t base = uint32_t(out_.vertices.size());
		out_.vertices.push_back({ float(tl), float(p_t.top) });
		out_.vertices.push_back({ float(tr), float(p_t.top) });
		out_.vertices.push_back({ float(br), float(p_t.bottom) });
		out_.vertices.push_back({ float(bl), float(p_t.bottom) });
		if (top_wide) {
			out_.indices.insert(out_.indices.end(), { base, base + 1, base + 2 });
		}
		if (bottom_wide) {
			out_.indices.insert(out_.indices.end(), { base, base + 2, base + 3 });
		}
	}

	SwfFillRule rule_;
	WorkBudget &budget_;
	SwfTriangulation &out_;
	std::vector<SweepEdge> edges_;
	std::vector<uint32_t> active_;
	std::vector<OpenTrapezoid> open_;
	std::vector<uint32_t> open_by_left_;
};

}

double swf_signed_area(std::span<const SwfPoint> p_outline, std::span<const uint32_t> p_contour_ends) {
	double area = 0.0;
	uint32_t begin = 0;
	for (const uint32_t end : p_contour_ends) {
		const uint32_t n = end >= begin && end <= p_outline.size() ? end - begin : 0;
		for (uint32_t i = 0; n >= 3 && i < n; ++i) {
			const SwfPoint a = p_outline[begin + i];
			const SwfPoint b = p_outline[begin + (i + 1) % n];
			area += double(a.x) * double(b.y) - double(b.x) * double(a.y);
		}
		begin = end;
	}
	return area * 0.5;
}

double swf_triangle_area(std::span<const SwfPoint> p_vertices, std::span<const uint32_t> p_indices) {
	double area = 0.0;
	for (size_t i = 0; i + 2 < p_indices.size(); i += 3) {
		if (p_indices[i] >= p_vertices.size() || p_indices[i + 1] >= p_vertices.size() || p_indices[i + 2] >= p_vertices.size()) {
			continue;
		}
		const SwfPoint a = p_vertices[p_indices[i]];
		const SwfPoint b = p_vertices[p_indices[i + 1]];
		const SwfPoint c = p_vertices[p_indices[i + 2]];
		area += std::fabs((double(b.x) - a.x) * (double(c.y) - a.y) - (double(c.x) - a.x) * (double(b.y) - a.y)) * 0.5;
	}
	return area;
}

uint32_t swf_curve_segments(SwfPoint p_from, SwfPoint p_control, SwfPoint p_to, float p_tolerance) {
	const double dx = double(p_from.x) - 2.0 * p_control.x + p_to.x;
	const double dy = double(p_from.y) - 2.0 * p_control.y + p_to.y;
	const double bend = std::sqrt(dx * dx + dy * dy);
	if (!(bend > 0.0) || !(p_tolerance > 0.0f)) {
		return 1;
	}
	const double n = std::ceil(std::sqrt(bend / (4.0 * double(p_tolerance))));
	return n >= double(kSwfCurveMaxSegments) ? kSwfCurveMaxSegments : std::max<uint32_t>(1, uint32_t(n));
}

Status swf_triangulate_nonzero(std::span<const SwfPoint> p_outline, std::span<const uint32_t> p_contour_ends, WorkBudget &r_budget,
		SwfTriangulation &r_out) {
	return swf_triangulate(p_outline, p_contour_ends, SwfFillRule::NonZero, r_budget, r_out);
}

Status swf_triangulate(std::span<const SwfPoint> p_outline, std::span<const uint32_t> p_contour_ends, SwfFillRule p_rule, WorkBudget &r_budget,
		SwfTriangulation &r_out) {
	r_out = SwfTriangulation{};
	Sweep sweep(p_rule, r_budget, r_out);
	return sweep.run(p_outline, p_contour_ends);
}

double SwfFillMesh::outline_area() const {
	return swf_signed_area(outline, contour_ends);
}

double SwfFillMesh::triangle_area() const {
	return swf_triangle_area(vertices, indices);
}

}
