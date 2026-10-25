#pragma once

#include "bf3/core/level/budget.h"
#include "bf3/core/result.h"
#include "bf3/core/ui/swf_movie.h"

#include <cstdint>
#include <span>
#include <vector>

namespace bf3::core::ui {

double swf_signed_area(std::span<const SwfPoint> p_outline, std::span<const uint32_t> p_contour_ends);

double swf_triangle_area(std::span<const SwfPoint> p_vertices, std::span<const uint32_t> p_indices);

uint32_t swf_curve_segments(SwfPoint p_from, SwfPoint p_control, SwfPoint p_to, float p_tolerance = kSwfFlattenTolerance);

struct SwfTriangulation {
	std::vector<SwfPoint> vertices;
	std::vector<uint32_t> indices;
	uint32_t slabs = 0;
	uint32_t crossings = 0;
};

enum class SwfFillRule : uint8_t {
	NonZero,
	EvenOdd,
};

Status swf_triangulate_nonzero(std::span<const SwfPoint> p_outline, std::span<const uint32_t> p_contour_ends, WorkBudget &r_budget,
		SwfTriangulation &r_out);

Status swf_triangulate(std::span<const SwfPoint> p_outline, std::span<const uint32_t> p_contour_ends,
		SwfFillRule p_rule, WorkBudget &r_budget, SwfTriangulation &r_out);

}
