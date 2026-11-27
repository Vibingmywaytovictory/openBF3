#include "bf3/sim/water.h"

#include "bf3/core/util/fnv.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace bf3::sim {

namespace {

constexpr double kInsideSlack = 1e-9;

size_t cell_index(float p_v, float p_min, size_t p_count) {
	const float f = std::floor((p_v - p_min) / kWaterCell);
	if (!(f > 0.0f)) {
		return 0;
	}
	return std::min(p_count - 1, size_t(f));
}

}

core::Result<std::shared_ptr<const WaterSurface>> WaterSurface::build(std::span<const WaterBodyMesh> p_bodies) {
	auto out = std::shared_ptr<WaterSurface>(new WaterSurface());
	uint64_t h = core::kFnv1aOffset;
	core::fnv1a_value(h, uint64_t(p_bodies.size()));
	for (const WaterBodyMesh &mesh : p_bodies) {
		core::fnv1a_value(h, uint64_t(mesh.triangles.size()));
		if (mesh.triangles.empty()) {
			continue;
		}
		Body body;
		body.min = { std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
			std::numeric_limits<float>::infinity() };
		body.max = { -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
			-std::numeric_limits<float>::infinity() };
		for (const std::array<Vec3, 3> &t : mesh.triangles) {
			for (const Vec3 &v : t) {
				for (size_t k = 0; k < 3; ++k) {
					if (!std::isfinite(v[k]) || std::fabs(v[k]) > 1e6f) {
						return core::make_error("water surface: a vertex is out of range");
					}
					body.min[k] = std::min(body.min[k], v[k]);
					body.max[k] = std::max(body.max[k], v[k]);
					core::fnv1a_value(h, v[k]);
				}
			}
		}
		body.columns = size_t(std::floor((body.max[0] - body.min[0]) / kWaterCell)) + 1;
		body.rows = size_t(std::floor((body.max[2] - body.min[2]) / kWaterCell)) + 1;
		if (body.columns * body.rows > kMaxWaterCells) {
			return core::make_error(
					"water surface: a body spans more than " + std::to_string(kMaxWaterCells) + " cells");
		}
		std::vector<uint32_t> counts(body.columns * body.rows + 1, 0);
		const auto span_of = [&](const std::array<Vec3, 3> &p_t) {
			const float lo_x = std::min({ p_t[0][0], p_t[1][0], p_t[2][0] });
			const float hi_x = std::max({ p_t[0][0], p_t[1][0], p_t[2][0] });
			const float lo_z = std::min({ p_t[0][2], p_t[1][2], p_t[2][2] });
			const float hi_z = std::max({ p_t[0][2], p_t[1][2], p_t[2][2] });
			return std::array<size_t, 4>{ cell_index(lo_x, body.min[0], body.columns),
				cell_index(lo_z, body.min[2], body.rows), cell_index(hi_x, body.min[0], body.columns),
				cell_index(hi_z, body.min[2], body.rows) };
		};
		for (const std::array<Vec3, 3> &t : mesh.triangles) {
			const std::array<size_t, 4> s = span_of(t);
			for (size_t z = s[1]; z <= s[3]; ++z) {
				for (size_t x = s[0]; x <= s[2]; ++x) {
					++counts[z * body.columns + x + 1];
				}
			}
		}
		for (size_t i = 1; i < counts.size(); ++i) {
			counts[i] += counts[i - 1];
		}
		body.cell_start = counts;
		body.cell_items.resize(counts.back());
		for (uint32_t i = 0; i < mesh.triangles.size(); ++i) {
			const std::array<size_t, 4> s = span_of(mesh.triangles[i]);
			for (size_t z = s[1]; z <= s[3]; ++z) {
				for (size_t x = s[0]; x <= s[2]; ++x) {
					body.cell_items[counts[z * body.columns + x]++] = i;
				}
			}
		}
		body.triangles = mesh.triangles;
		out->bodies_.push_back(std::move(body));
	}
	out->digest_ = h;
	return std::shared_ptr<const WaterSurface>(std::move(out));
}

size_t WaterSurface::triangle_count() const {
	size_t n = 0;
	for (const Body &b : bodies_) {
		n += b.triangles.size();
	}
	return n;
}

// SOURCE: notes R4-B B (a vertical ray inside the water body's box gives the level, none outside).
// INFERRED: the highest triangle over the point is that ray's hit and the soldier reads this query. Settle: R4-B B.
std::optional<float> WaterSurface::height(float p_x, float p_z) const {
	std::optional<float> best;
	if (!std::isfinite(p_x) || !std::isfinite(p_z)) {
		return best;
	}
	for (const Body &b : bodies_) {
		if (p_x < b.min[0] || p_x > b.max[0] || p_z < b.min[2] || p_z > b.max[2]) {
			continue;
		}
		const size_t cell = cell_index(p_z, b.min[2], b.rows) * b.columns + cell_index(p_x, b.min[0], b.columns);
		for (uint32_t k = b.cell_start[cell]; k < b.cell_start[cell + 1]; ++k) {
			const std::array<Vec3, 3> &t = b.triangles[b.cell_items[k]];
			const double ax = t[0][0], az = t[0][2];
			const double e1x = double(t[1][0]) - ax, e1z = double(t[1][2]) - az;
			const double e2x = double(t[2][0]) - ax, e2z = double(t[2][2]) - az;
			const double area = e1x * e2z - e1z * e2x;
			if (std::fabs(area) < kInsideSlack) {
				continue;
			}
			const double px = double(p_x) - ax, pz = double(p_z) - az;
			const double b1 = (px * e2z - pz * e2x) / area;
			const double b2 = (e1x * pz - e1z * px) / area;
			const double b0 = 1.0 - b1 - b2;
			if (b0 < -kInsideSlack || b1 < -kInsideSlack || b2 < -kInsideSlack) {
				continue;
			}
			const float y = float(b0 * double(t[0][1]) + b1 * double(t[1][1]) + b2 * double(t[2][1]));
			if (!best || y > *best) {
				best = y;
			}
		}
	}
	return best;
}

}
