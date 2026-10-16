#pragma once
// Terrain collision checks shared by sim.terrain.unit and sim.terrain (tests/terrain_collision_tests.cpp,
// docs/design/terrain-collision.md §12.1): a soup terrain record from a grid, the exhaustive field + patch check
// (every Jolt triangle of the terrain's bodies against soup_terrain_cell, one to one) and the vertical-ray checks
// (lattice, mismatch cells, points of open cells).
#include "bf3/core/level/collision_soup.h"
#include "bf3/core/res/terrain.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/static_world.h"

#include "static_world_internal.h" // the terrain's Jolt shapes (StaticWorldShapes::Impl::Terrain)

#include <Jolt/Geometry/AABox.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace terrain_test {

using namespace bf3;
namespace level = bf3::core::level;
namespace res = bf3::core::res;

// A Built soup terrain record over p_grid at p_frame (what build_collision_soup keeps).
inline level::SoupTerrain make_record(res::HeightGrid p_grid, const res::Transform &p_frame = res::Transform::identity(), uint32_t p_object = 0) {
	level::SoupTerrain t;
	t.object = p_object;
	t.state = level::SoupTerrain::State::Built;
	t.side = std::max(p_grid.width, p_grid.height);
	for (uint8_t h : p_grid.holes) {
		t.holes += h != 0 ? 1 : 0;
	}
	t.grid = std::make_shared<const res::HeightGrid>(std::move(p_grid));
	t.frame = p_frame;
	return t;
}

// A flat-ish grid: heights from p_height(x, z), no holes.
template <class F>
res::HeightGrid make_grid(size_t p_width, size_t p_height, std::array<float, 2> p_origin, std::array<float, 2> p_spacing, F &&p_heights) {
	res::HeightGrid g;
	g.width = p_width;
	g.height = p_height;
	g.origin_xz = p_origin;
	g.sample_spacing = p_spacing;
	g.heights.resize(p_width * p_height);
	g.holes.assign(p_width * p_height, 0);
	for (size_t z = 0; z < p_height; ++z) {
		for (size_t x = 0; x < p_width; ++x) {
			g.heights[x + z * p_width] = p_heights(x, z);
		}
	}
	return g;
}

struct D3 {
	double x = 0, y = 0, z = 0;
};
inline D3 d3(const sim::Vec3 &p_v) {
	return { p_v[0], p_v[1], p_v[2] };
}
inline double dist(D3 a, D3 b) {
	return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

// World point -> grid coordinates (u along X samples, v along Z samples) through the inverse of the record's frame
// (a rotation times a uniform scale: the axes are orthogonal).
inline void to_grid(const level::SoupTerrain &p_t, D3 p_w, double &r_u, double &r_v) {
	const res::Transform &f = p_t.frame;
	const D3 d{ p_w.x - f.origin[0], p_w.y - f.origin[1], p_w.z - f.origin[2] };
	const auto local = [&](int p_axis) {
		const res::Vec3 &a = f.axis[size_t(p_axis)];
		const double n = double(a[0]) * a[0] + double(a[1]) * a[1] + double(a[2]) * a[2];
		return (d.x * a[0] + d.y * a[1] + d.z * a[2]) / n;
	};
	const res::HeightGrid &g = *p_t.grid;
	r_u = (local(0) - g.origin_xz[0]) / g.sample_spacing[0];
	r_v = (local(2) - g.origin_xz[1]) / g.sample_spacing[1];
}

// Which half of its cell a triangle is, from its centroid: 1 = (a, d, b) (below the diagonal in grid space), 0 = (a, c, d).
inline bool cell_of(const level::SoupTerrain &p_t, const std::array<D3, 3> &p_v, size_t &r_x, size_t &r_z, int &r_half) {
	const D3 c{ (p_v[0].x + p_v[1].x + p_v[2].x) / 3.0, (p_v[0].y + p_v[1].y + p_v[2].y) / 3.0, (p_v[0].z + p_v[1].z + p_v[2].z) / 3.0 };
	double u = 0, v = 0;
	to_grid(p_t, c, u, v);
	const double fx = std::floor(u), fz = std::floor(v);
	const res::HeightGrid &g = *p_t.grid;
	if (!(fx >= 0 && fz >= 0 && fx < double(g.width - 1) && fz < double(g.height - 1))) {
		return false;
	}
	r_x = size_t(fx);
	r_z = size_t(fz);
	r_half = (u - fx) > (v - fz) ? 1 : 0;
	return true;
}

struct FieldCheck {
	size_t cells = 0;
	size_t soup_triangles = 0; // soup_terrain_cell over every cell
	size_t field_triangles = 0; // Jolt's, read back from the field body
	size_t patch_triangles = 0;
	size_t missing = 0; // soup triangles with no Jolt triangle
	size_t extra = 0; // Jolt triangles with no soup triangle (or outside the grid)
	size_t duplicate = 0; // two Jolt triangles on one soup triangle
	size_t resumed = 0; // field triangles Jolt returned again after a full buffer (ignored)
	double worst = 0.0; // largest corner distance of a pair (metres)
	bool ok() const { return missing == 0 && extra == 0 && duplicate == 0; }
};

// Every triangle of terrain p_index's field (at its body transform) and patch, against soup_terrain_cell of every cell
// of p_record, one to one by (cell, half), with the corners compared.
inline FieldCheck exhaustive_check(const sim::StaticWorldShapes &p_world, size_t p_index, const level::SoupTerrain &p_record, uint32_t p_placement) {
	FieldCheck r;
	const sim::StaticWorldShapes::Impl::Terrain &terrain = p_world.impl().terrains[p_index];
	const res::HeightGrid &g = *p_record.grid;
	if (g.width < 2 || g.height < 2) {
		return r;
	}
	const size_t cw = g.width - 1, ch = g.height - 1;
	r.cells = cw * ch;
	std::vector<uint8_t> seen(r.cells, 0);
	std::vector<JPH::Float3> buffer(3 * 65536);
	const auto visit = [&](const JPH::Shape *p_shape, JPH::Vec3 p_position, JPH::Quat p_rotation, size_t &r_count) {
		if (p_shape == nullptr) {
			return;
		}
		JPH::Shape::GetTrianglesContext context;
		const JPH::AABox box(JPH::Vec3::sReplicate(-1e7f), JPH::Vec3::sReplicate(1e7f));
		p_shape->GetTrianglesStart(context, box, p_position, p_rotation, JPH::Vec3::sOne());
		while (true) {
			const int n = p_shape->GetTrianglesNext(context, int(buffer.size() / 3), buffer.data());
			if (n <= 0) {
				break;
			}
			for (int t = 0; t < n; ++t) {
				std::array<D3, 3> v;
				for (int k = 0; k < 3; ++k) {
					const JPH::Float3 &f = buffer[size_t(t) * 3 + size_t(k)];
					v[size_t(k)] = { f.x, f.y, f.z };
				}
				++r_count;
				size_t x = 0, z = 0;
				int half = 0;
				if (!cell_of(p_record, v, x, z, half)) {
					++r.extra;
					continue;
				}
				uint8_t &s = seen[x + z * cw];
				const uint8_t bit = uint8_t(1u << half);
				const uint8_t from_patch = uint8_t(8u << half);
				const bool patch = p_shape == terrain.patch.GetPtr();
				if ((s & bit) != 0) {
					// Jolt's HeightFieldShape resumes a walk that filled the buffer at the start of the block it stopped in,
					// so a field triangle comes again after a full buffer: not a duplicate. Field + patch, or the patch
					// twice, is one.
					if (!patch && (s & from_patch) == 0) {
						--r_count;
						++r.resumed;
					} else {
						++r.duplicate;
					}
					continue;
				}
				s |= bit;
				if (patch) {
					s |= from_patch;
				}
				const level::SoupTerrainCell cell = level::soup_terrain_cell(p_record, p_placement, x, z);
				bool found = false;
				for (uint8_t k = 0; k < cell.count; ++k) {
					std::array<D3, 3> sv;
					for (size_t c = 0; c < 3; ++c) {
						sv[c] = d3(cell.triangles[k].v[c]);
					}
					size_t sx = 0, sz = 0;
					int sh = 0;
					if (!cell_of(p_record, sv, sx, sz, sh) || sh != half) {
						continue;
					}
					found = true;
					for (const D3 &p : v) {
						r.worst = std::max(r.worst, std::min({ dist(p, sv[0]), dist(p, sv[1]), dist(p, sv[2]) }));
					}
				}
				if (!found) {
					s |= 4; // counted here, not again as missing
					++r.extra;
				}
			}
		}
	};
	const JPH::Quat q(terrain.rotation[0], terrain.rotation[1], terrain.rotation[2], terrain.rotation[3]);
	visit(terrain.field.GetPtr(), JPH::Vec3(terrain.position[0], terrain.position[1], terrain.position[2]), q, r.field_triangles);
	visit(terrain.patch.GetPtr(), JPH::Vec3::sZero(), JPH::Quat::sIdentity(), r.patch_triangles);
	for (size_t z = 0; z < ch; ++z) {
		for (size_t x = 0; x < cw; ++x) {
			const level::SoupTerrainCell cell = level::soup_terrain_cell(p_record, p_placement, x, z);
			for (uint8_t k = 0; k < cell.count; ++k) {
				std::array<D3, 3> sv;
				for (size_t c = 0; c < 3; ++c) {
					sv[c] = d3(cell.triangles[k].v[c]);
				}
				size_t sx = 0, sz = 0;
				int sh = 0;
				++r.soup_triangles;
				if (!cell_of(p_record, sv, sx, sz, sh) || (seen[x + z * cw] & (1u << sh)) == 0) {
					++r.missing;
				}
			}
		}
	}
	return r;
}

// One vertical ray into a cell triangle: the point at in-cell fractions (0.6, 0.3) for (a, d, b) or (0.3, 0.6) for
// (a, c, d), the expected height from the soup triangle's plane (doubles).
struct CellRay {
	D3 at; // world x, expected y, z
	bool ground = false; // a soup triangle is there
};
inline CellRay cell_ray(const level::SoupTerrain &p_record, uint32_t p_placement, size_t p_x, size_t p_z, int p_half) {
	CellRay out;
	const res::HeightGrid &g = *p_record.grid;
	const double fu = p_half == 1 ? 0.6 : 0.3, fv = p_half == 1 ? 0.3 : 0.6;
	const res::Vec3 w = p_record.frame.transform_point(
			{ float(g.origin_xz[0] + (double(p_x) + fu) * g.sample_spacing[0]), 0.0f, float(g.origin_xz[1] + (double(p_z) + fv) * g.sample_spacing[1]) });
	out.at = { w[0], 0.0, w[2] };
	const level::SoupTerrainCell cell = level::soup_terrain_cell(p_record, p_placement, p_x, p_z);
	for (uint8_t k = 0; k < cell.count; ++k) {
		std::array<D3, 3> v;
		for (size_t c = 0; c < 3; ++c) {
			v[c] = d3(cell.triangles[k].v[c]);
		}
		size_t sx = 0, sz = 0;
		int sh = 0;
		if (!cell_of(p_record, v, sx, sz, sh) || sh != p_half) {
			continue;
		}
		// The plane's height at (x, z).
		const D3 e1{ v[1].x - v[0].x, v[1].y - v[0].y, v[1].z - v[0].z }, e2{ v[2].x - v[0].x, v[2].y - v[0].y, v[2].z - v[0].z };
		const D3 n{ e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x };
		if (std::fabs(n.y) < 1e-12) {
			continue;
		}
		out.at.y = v[0].y - (n.x * (out.at.x - v[0].x) + n.z * (out.at.z - v[0].z)) / n.y;
		out.ground = true;
	}
	return out;
}

struct RayCheck {
	size_t rays = 0;
	size_t missed = 0; // ground expected, no terrain hit within the tolerance
	size_t open_rays = 0;
	size_t open_hits = 0; // a terrain hit in an open cell
	size_t occluded = 0; // a non-terrain hit first (full worlds): re-cast past it
	size_t coincident = 0; // a non-terrain hit within the tolerance of the terrain (Havok ground on the terrain plane)
	double worst = 0.0; // largest |hit y - expected y| of the hits
	// The first miss, for the message: cell, triangle, expected y, the last hit's y (NaN: none) and its occluders.
	size_t miss_x = 0, miss_z = 0;
	int miss_half = -1, miss_occluders = 0;
	double miss_expected = 0.0, miss_hit_y = 0.0;
};

// A ray into a cell triangle against p_world: ground expected -> a terrain hit within p_tol of the plane (a ray from
// 0.25 m above to 0.25 m below; non-terrain hits are passed and counted as occluded); an open cell -> no terrain hit
// over [p_lo_y, p_hi_y].
inline void cast_cell(const sim::CollisionWorld &p_world, const level::SoupTerrain &p_record, uint32_t p_placement, size_t p_x, size_t p_z, int p_half,
		double p_lo_y, double p_hi_y, double p_tol, RayCheck &r_check) {
	const CellRay ray = cell_ray(p_record, p_placement, p_x, p_z, p_half);
	const bool open = level::soup_terrain_cell(p_record, p_placement, p_x, p_z).open;
	if (!ray.ground && !open) {
		return; // a degenerate triangle dropped by the quantise rule: nothing to expect
	}
	double top = ray.ground ? ray.at.y + 0.25 : p_hi_y;
	const double bottom = ray.ground ? ray.at.y - 0.25 : p_lo_y;
	(ray.ground ? r_check.rays : r_check.open_rays) += 1;
	const auto note_miss = [&](double p_hit_y, int p_occluders) {
		if (r_check.missed++ == 0) {
			r_check.miss_x = p_x;
			r_check.miss_z = p_z;
			r_check.miss_half = p_half;
			r_check.miss_expected = ray.at.y;
			r_check.miss_hit_y = p_hit_y;
			r_check.miss_occluders = p_occluders;
		}
	};
	int occluders = 0;
	double last_y = std::numeric_limits<double>::quiet_NaN();
	for (int pass = 0; pass < 8 && top > bottom; ++pass) {
		sim::StaticHit hit;
		const sim::Vec3 origin{ float(ray.at.x), float(top), float(ray.at.z) };
		if (!p_world.ray(origin, sim::Vec3{ 0.0f, float(bottom - top), 0.0f }, sim::query_mask::kAll, &hit)) {
			break;
		}
		if (hit.terrain == sim::kNoStaticTerrain) {
			if (ray.ground && std::fabs(double(hit.position[1]) - ray.at.y) <= p_tol) {
				// Havok ground lies on the terrain plane: the ray cannot tell the two apart and re-casting 1 mm below
				// would step past the terrain. Ground is there; the terrain-only world proves the field (G1).
				++r_check.coincident;
				return;
			}
			++r_check.occluded;
			++occluders;
			last_y = double(hit.position[1]);
			top = double(hit.position[1]) - 1e-3;
			continue;
		}
		if (!ray.ground) {
			++r_check.open_hits;
			return;
		}
		const double e = std::fabs(double(hit.position[1]) - ray.at.y);
		r_check.worst = std::max(r_check.worst, e);
		if (e > p_tol) {
			note_miss(double(hit.position[1]), occluders);
		}
		return;
	}
	if (ray.ground) {
		note_miss(last_y, occluders);
	}
}

// The cells whose soup and Jolt hole rules differ before the cook marks them (the only hole is b or c), with their 8
// neighbours: where the patch lives.
inline std::vector<std::array<size_t, 2>> mismatch_cells(const res::HeightGrid &p_g) {
	std::vector<std::array<size_t, 2>> out;
	if (p_g.width < 2 || p_g.height < 2) {
		return out;
	}
	const size_t cw = p_g.width - 1, ch = p_g.height - 1;
	std::vector<uint8_t> mark(cw * ch, 0);
	for (size_t z = 0; z < ch; ++z) {
		for (size_t x = 0; x < cw; ++x) {
			const bool a = p_g.hole(x, z), b = p_g.hole(x + 1, z), c = p_g.hole(x, z + 1), d = p_g.hole(x + 1, z + 1);
			if (!a && !d && (b != c)) {
				for (size_t zz = z == 0 ? 0 : z - 1; zz <= std::min(ch - 1, z + 1); ++zz) {
					for (size_t xx = x == 0 ? 0 : x - 1; xx <= std::min(cw - 1, x + 1); ++xx) {
						mark[xx + zz * cw] = 1;
					}
				}
			}
		}
	}
	for (size_t z = 0; z < ch; ++z) {
		for (size_t x = 0; x < cw; ++x) {
			if (mark[x + z * cw] != 0) {
				out.push_back({ x, z });
			}
		}
	}
	return out;
}

} // namespace terrain_test
