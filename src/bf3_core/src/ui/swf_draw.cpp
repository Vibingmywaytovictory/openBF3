#include "bf3/core/ui/swf_draw.h"

#include "bf3/core/ui/swf_geometry.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace bf3::core::ui {

namespace {

constexpr size_t kDrawBudgetUnits = size_t(1) << 20;

bool finite(SwfPoint p_p) {
	return std::isfinite(p_p.x) && std::isfinite(p_p.y);
}

}

SwfColor swf_draw_fill_color(uint32_t p_rgb, double p_alpha_percent) {
	const double a = std::isfinite(p_alpha_percent) ? std::clamp(p_alpha_percent, 0.0, 100.0) : 100.0;
	return SwfColor{ uint8_t((p_rgb >> 16) & 0xFFu), uint8_t((p_rgb >> 8) & 0xFFu), uint8_t(p_rgb & 0xFFu),
		uint8_t(std::lround(a * 2.55)) };
}

void SwfDrawing::close_contour() {
	if (fills_.empty()) {
		return;
	}
	Fill &f = fills_.back();
	const uint32_t begin = f.contour_ends.empty() ? 0 : f.contour_ends.back();
	if (f.outline.size() > begin + 1 && f.outline.back() == f.outline[begin]) {
		f.outline.pop_back();
	}
	if (f.outline.size() < size_t(begin) + 3) {
		f.outline.resize(begin);
		return;
	}
	f.contour_ends.push_back(uint32_t(f.outline.size()));
}

Status SwfDrawing::apply(const SwfDrawOp &p_op) {
	if (p_op.kind == SwfDrawOpKind::Clear) {
		fills_.clear();
		filling_ = false;
		pen_ = SwfPoint{};
		commands_ = 0;
		return Status::success();
	}
	if (!finite(p_op.to) || !finite(p_op.control)) {
		return make_error("swf drawing: a point is not finite");
	}
	if (commands_ >= kSwfDrawMaxCommands) {
		return make_error(
				"swf drawing: more than " + std::to_string(kSwfDrawMaxCommands) + " commands since the last clear");
	}
	++commands_;
	switch (p_op.kind) {
		case SwfDrawOpKind::BeginFill:
			if (filling_) {
				close_contour();
			}
			fills_.push_back(Fill{ p_op.color, { pen_ }, {} });
			filling_ = true;
			return Status::success();
		case SwfDrawOpKind::MoveTo:
			if (filling_) {
				close_contour();
				fills_.back().outline.push_back(p_op.to);
			}
			pen_ = p_op.to;
			return Status::success();
		case SwfDrawOpKind::LineTo:
			if (filling_) {
				fills_.back().outline.push_back(p_op.to);
			}
			pen_ = p_op.to;
			return Status::success();
		case SwfDrawOpKind::CurveTo:
			if (filling_) {
				const uint32_t n = swf_curve_segments(pen_, p_op.control, p_op.to);
				for (uint32_t i = 1; i <= n; ++i) {
					const double t = double(i) / double(n);
					const double u = 1.0 - t;
					fills_.back().outline.push_back(
							{ float(u * u * pen_.x + 2.0 * u * t * p_op.control.x + t * t * p_op.to.x),
									float(u * u * pen_.y + 2.0 * u * t * p_op.control.y + t * t * p_op.to.y) });
				}
			}
			pen_ = p_op.to;
			return Status::success();
		case SwfDrawOpKind::EndFill:
			if (filling_) {
				close_contour();
				filling_ = false;
			}
			return Status::success();
		case SwfDrawOpKind::Clear:
			break;
	}
	return Status::success();
}

Result<std::shared_ptr<const SwfShape>> SwfDrawing::shape() const {
	auto out = std::make_shared<SwfShape>();
	out->version = 3;
	WorkBudget budget(kDrawBudgetUnits);
	bool any = false;
	for (size_t i = 0; i < fills_.size(); ++i) {
		Fill f = fills_[i];
		if (filling_ && i + 1 == fills_.size()) {
			const uint32_t begin = f.contour_ends.empty() ? 0 : f.contour_ends.back();
			if (f.outline.size() > begin + 1 && f.outline.back() == f.outline[begin]) {
				f.outline.pop_back();
			}
			if (f.outline.size() >= size_t(begin) + 3) {
				f.contour_ends.push_back(uint32_t(f.outline.size()));
			} else {
				f.outline.resize(begin);
			}
		}
		SwfFillStyle style;
		style.kind = SwfFillKind::Solid;
		style.color = f.color;
		out->fills.push_back(style);
		if (f.contour_ends.empty()) {
			continue;
		}
		SwfFillMesh mesh;
		mesh.fill = uint32_t(i);
		SwfTriangulation t;
		BF3_TRY(swf_triangulate(f.outline, f.contour_ends, SwfFillRule::EvenOdd, budget, t));
		for (const SwfPoint &p : f.outline) {
			if (!any) {
				out->bounds = SwfRect{ p.x, p.x, p.y, p.y };
				any = true;
			}
			out->bounds.x_min = std::min(out->bounds.x_min, p.x);
			out->bounds.x_max = std::max(out->bounds.x_max, p.x);
			out->bounds.y_min = std::min(out->bounds.y_min, p.y);
			out->bounds.y_max = std::max(out->bounds.y_max, p.y);
		}
		mesh.outline = std::move(f.outline);
		mesh.contour_ends = std::move(f.contour_ends);
		mesh.vertices = std::move(t.vertices);
		mesh.indices = std::move(t.indices);
		out->meshes.push_back(std::move(mesh));
	}
	out->edge_bounds = out->bounds;
	return std::shared_ptr<const SwfShape>(std::move(out));
}

}
