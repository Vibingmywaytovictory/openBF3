// The soup's terrain record (TC, docs/design/terrain-collision.md §5.1, §6): one terrain cell's two triangles, a tile's
// terrain triangles and the whole grid's hash, made on demand from SoupTerrain::grid and frame. The cell rule is the one
// build_collision_soup applied to the cells around the Havok tiles before TC (ported from IW4L level_bf3
// collision/terrain.rs, Apache-2.0; research/probes/tools/level-oracle `terrain` is its parity oracle), now over every
// cell: corners a = (x, z), b = (x + 1, z), c = (x, z + 1), d = (x + 1, z + 1) give (a, d, b) and (a, c, d) at the
// object's frame, each wound to face +Y; a cell with a hole corner is open; rows along +Z, cells along +X. Float
// operations keep the order of the code before TC, so the triangles and hashes are bit for bit the same.
#include "soup_terrain_internal.h"

#include "bf3/core/level/collision_soup.h"
#include "bf3/core/thread_pool.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <string>

namespace bf3::core::level {

namespace {

using res::Transform;

// Tile index of a cell centre (grid coordinates, f64); false when it is not a sane coordinate (the cell is skipped,
// as before TC and in the oracle).
bool centre_tile(float p_origin, float p_spacing, size_t p_index, int32_t &r_tile) {
	const double centre = double(p_origin) + (double(p_index) + 0.5) * double(p_spacing);
	const double tile = std::floor(centre / 64.0);
	if (!(std::fabs(tile) < 1e7)) {
		return false;
	}
	r_tile = int32_t(tile);
	return true;
}

// Winds a terrain triangle to face +Y (Havok's counter-clockwise front).
std::array<res::Vec3, 3> face_up(const std::array<res::Vec3, 3> &p_t) {
	const res::Vec3 &a = p_t[0];
	const res::Vec3 &b = p_t[1];
	const res::Vec3 &c = p_t[2];
	const res::Vec3 u = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
	const res::Vec3 v = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
	if (u[2] * v[0] - u[0] * v[2] < 0.0f) {
		return { a, c, b };
	}
	return p_t;
}

bool grid_ok(const res::HeightGrid &p_grid) {
	if (p_grid.width > (size_t(1) << 20) || p_grid.height > (size_t(1) << 20)) {
		return false;
	}
	const size_t samples = p_grid.width * p_grid.height;
	return p_grid.heights.size() == samples && p_grid.holes.size() == samples;
}

bool hole_cell(const res::HeightGrid &p_grid, size_t p_x, size_t p_z) {
	return p_grid.hole(p_x, p_z) || p_grid.hole(p_x + 1, p_z) || p_grid.hole(p_x, p_z + 1) || p_grid.hole(p_x + 1, p_z + 1);
}

// One cell with the quantised corners of its kept triangles (the hash and the tile need them).
struct CellQ {
	SoupTerrainCell cell;
	std::array<std::array<int64_t, 9>, 2> q{};
};

// The cell of a valid grid (0 <= x < width - 1, 0 <= z < height - 1): the code before TC, line for line.
void make_cell(const res::HeightGrid &p_grid, const Transform &p_frame, uint32_t p_placement, size_t p_x, size_t p_z, CellQ &r_out) {
	r_out.cell = SoupTerrainCell();
	if (hole_cell(p_grid, p_x, p_z)) {
		r_out.cell.open = true;
		return;
	}
	const float ox = p_grid.origin_xz[0];
	const float oz = p_grid.origin_xz[1];
	const float sx = p_grid.sample_spacing[0];
	const float sz = p_grid.sample_spacing[1];
	const auto point = [&](size_t p_i, size_t p_j) {
		return p_frame.transform_point({ ox + float(p_x + p_i) * sx, p_grid.at(p_x + p_i, p_z + p_j), oz + float(p_z + p_j) * sz });
	};
	const res::Vec3 a = point(0, 0);
	const res::Vec3 b = point(1, 0);
	const res::Vec3 c = point(0, 1);
	const res::Vec3 d = point(1, 1);
	const std::array<std::array<res::Vec3, 3>, 2> pair = { { { a, d, b }, { a, c, d } } };
	for (const std::array<res::Vec3, 3> &t : pair) {
		SoupTriangle tri;
		tri.v = face_up(t);
		std::array<int64_t, 9> &q = r_out.q[r_out.cell.count];
		if (!soup_quantise(tri.v, q)) {
			++r_out.cell.nonfinite;
			continue;
		}
		if (soup_degenerate(q)) {
			++r_out.cell.degenerate;
			continue;
		}
		tri.material = 0;
		tri.placement = p_placement;
		tri.surface = surface::kDirt;
		tri.body = uint8_t(CollisionBody::Collision);
		tri.flags = kSoupTerrain;
		r_out.cell.triangles[r_out.cell.count++] = tri;
	}
}

uint64_t triangle_hash(const std::array<int64_t, 9> &p_q, const SoupTriangle &p_t) {
	uint64_t h = kFnvOffset;
	soup_hash_bytes(h, p_q, p_t.material, p_t.surface, p_t.body, p_t.flags);
	return h;
}

// Per column: its centre tile, or none (the cell is skipped).
void column_tiles(const res::HeightGrid &p_grid, std::vector<int32_t> &r_tiles, std::vector<uint8_t> &r_ok) {
	const size_t cells = p_grid.width - 1;
	r_tiles.assign(cells, 0);
	r_ok.assign(cells, 0);
	for (size_t x = 0; x < cells; ++x) {
		r_ok[x] = centre_tile(p_grid.origin_xz[0], p_grid.sample_spacing[0], x, r_tiles[x]) ? 1 : 0;
	}
}

// The cell rows and columns that can hold a triangle whose soup tile is p_tile: for a frame that keeps Y vertical (any
// rotation about Y, any scale; every retail terrain: the identity) the tile square mapped back into grid coordinates,
// one cell of margin; for any other frame the whole grid. Half-open ranges.
void tile_cell_range(const res::HeightGrid &p_grid, const Transform &p_frame, const std::array<int32_t, 2> &p_tile, size_t &r_x0, size_t &r_x1,
		size_t &r_z0, size_t &r_z1) {
	const size_t cw = p_grid.width - 1;
	const size_t ch = p_grid.height - 1;
	r_x0 = 0;
	r_x1 = cw;
	r_z0 = 0;
	r_z1 = ch;
	const res::Vec3 &ax = p_frame.axis[0];
	const res::Vec3 &ay = p_frame.axis[1];
	const res::Vec3 &az = p_frame.axis[2];
	if (ax[1] != 0.0f || az[1] != 0.0f || ay[0] != 0.0f || ay[2] != 0.0f) {
		return;
	}
	// World (wx, wz) = origin + lx * ax + lz * az over X / Z; local = inverse of that 2 x 2 map.
	const double a = ax[0], b = az[0], c = ax[2], d = az[2];
	const double det = a * d - b * c;
	const double ox = p_grid.origin_xz[0], oz = p_grid.origin_xz[1];
	const double sx = p_grid.sample_spacing[0], sz = p_grid.sample_spacing[1];
	if (!std::isfinite(det) || std::fabs(det) < 1e-12 || !std::isfinite(ox) || !std::isfinite(oz) || !std::isfinite(sx) || !std::isfinite(sz) ||
			std::fabs(sx) < 1e-9 || std::fabs(sz) < 1e-9 || !std::isfinite(p_frame.origin[0]) || !std::isfinite(p_frame.origin[2])) {
		return;
	}
	double gx0 = std::numeric_limits<double>::infinity(), gx1 = -gx0, gz0 = gx0, gz1 = -gx0;
	for (int k = 0; k < 4; ++k) {
		const double wx = (double(p_tile[0]) + double(k & 1)) * 64.0 - double(p_frame.origin[0]);
		const double wz = (double(p_tile[1]) + double(k >> 1)) * 64.0 - double(p_frame.origin[2]);
		const double lx = (d * wx - b * wz) / det;
		const double lz = (-c * wx + a * wz) / det;
		const double gx = (lx - ox) / sx;
		const double gz = (lz - oz) / sz;
		gx0 = std::min(gx0, gx);
		gx1 = std::max(gx1, gx);
		gz0 = std::min(gz0, gz);
		gz1 = std::max(gz1, gz);
	}
	const auto clamp_range = [](double p_lo, double p_hi, size_t p_cells, size_t &r_lo, size_t &r_hi) {
		const double lo = std::floor(p_lo) - 1.0;
		const double hi = std::ceil(p_hi) + 1.0;
		if (!(hi >= 0.0) || !(lo < double(p_cells))) {
			r_lo = r_hi = 0;
			return;
		}
		r_lo = lo <= 0.0 ? 0 : size_t(lo);
		r_hi = hi >= double(p_cells) ? p_cells : size_t(hi);
		if (r_hi < r_lo) {
			r_hi = r_lo;
		}
	};
	clamp_range(gx0, gx1, cw, r_x0, r_x1);
	clamp_range(gz0, gz1, ch, r_z0, r_z1);
}

struct RowSums {
	size_t cells = 0;
	size_t open = 0;
	size_t triangles = 0;
	size_t degenerate = 0;
	size_t nonfinite = 0;
	std::vector<SoupTerrainTileSum> tiles; // first-appearance order within the row
};

} // namespace

void soup_terrain_census(const res::HeightGrid &p_grid, unsigned p_threads, size_t &r_cells, size_t &r_open) {
	r_cells = 0;
	r_open = 0;
	if (p_grid.width < 2 || p_grid.height < 2) {
		return;
	}
	std::vector<int32_t> tiles;
	std::vector<uint8_t> ok;
	column_tiles(p_grid, tiles, ok);
	std::vector<std::array<size_t, 2>> rows(p_grid.height - 1);
	parallel_for(
			p_grid.height - 1,
			[&](size_t p_z) {
				int32_t tz = 0;
				if (!centre_tile(p_grid.origin_xz[1], p_grid.sample_spacing[1], p_z, tz)) {
					return;
				}
				for (size_t x = 0; x + 1 < p_grid.width; ++x) {
					if (ok[x] == 0) {
						continue;
					}
					++rows[p_z][0];
					rows[p_z][1] += hole_cell(p_grid, x, p_z) ? 1 : 0;
				}
			},
			p_threads);
	for (const std::array<size_t, 2> &r : rows) {
		r_cells += r[0];
		r_open += r[1];
	}
}

SoupTerrainCell soup_terrain_cell(const SoupTerrain &p_terrain, uint32_t p_placement, size_t p_x, size_t p_z) {
	SoupTerrainCell out;
	out.open = true;
	if (p_terrain.grid == nullptr) {
		return out;
	}
	const res::HeightGrid &g = *p_terrain.grid;
	if (!grid_ok(g) || g.width < 2 || g.height < 2 || p_x >= g.width - 1 || p_z >= g.height - 1) {
		return out;
	}
	CellQ c;
	make_cell(g, p_terrain.frame, p_placement, p_x, p_z, c);
	return c.cell;
}

Result<size_t> append_soup_terrain_tile(const SoupTerrain &p_terrain, uint32_t p_placement, const std::array<int32_t, 2> &p_tile,
		std::vector<SoupTriangle> &r_out, WorkBudget &r_budget) {
	if (p_terrain.grid == nullptr) {
		return make_error("soup terrain: object " + std::to_string(p_terrain.object) + " has no terrain record");
	}
	const res::HeightGrid &g = *p_terrain.grid;
	if (!grid_ok(g)) {
		return make_error("soup terrain: object " + std::to_string(p_terrain.object) + ": the grid's sample arrays do not match its size");
	}
	if (g.width < 2 || g.height < 2) {
		return size_t(0);
	}
	size_t x0 = 0, x1 = 0, z0 = 0, z1 = 0;
	tile_cell_range(g, p_terrain.frame, p_tile, x0, x1, z0, z1);
	if (x1 <= x0 || z1 <= z0) {
		return size_t(0);
	}
	BF3_TRY(r_budget.charge((x1 - x0) * (z1 - z0)));
	size_t added = 0;
	CellQ c;
	for (size_t z = z0; z < z1; ++z) {
		int32_t tz = 0;
		if (!centre_tile(g.origin_xz[1], g.sample_spacing[1], z, tz)) {
			continue;
		}
		for (size_t x = x0; x < x1; ++x) {
			int32_t tx = 0;
			if (!centre_tile(g.origin_xz[0], g.sample_spacing[0], x, tx)) {
				continue;
			}
			make_cell(g, p_terrain.frame, p_placement, x, z, c);
			for (uint8_t k = 0; k < c.cell.count; ++k) {
				if (soup_tile_of(c.q[k]) == p_tile) {
					r_out.push_back(c.cell.triangles[k]);
					++added;
				}
			}
		}
	}
	return added;
}

Result<SoupTerrainHash> hash_soup_terrain(const SoupTerrain &p_terrain, uint32_t p_placement, WorkBudget &r_budget, unsigned p_threads) {
	if (p_terrain.grid == nullptr) {
		return make_error("soup terrain: object " + std::to_string(p_terrain.object) + " has no terrain record");
	}
	const res::HeightGrid &g = *p_terrain.grid;
	if (!grid_ok(g)) {
		return make_error("soup terrain: object " + std::to_string(p_terrain.object) + ": the grid's sample arrays do not match its size");
	}
	SoupTerrainHash out;
	if (g.width < 2 || g.height < 2) {
		return out;
	}
	// Every cell is visited twice (the tile sums on the lanes, then the serial hash): paid before anything runs.
	BF3_TRY(r_budget.charge(2 * (g.width - 1) * (g.height - 1)));
	std::vector<int32_t> tiles;
	std::vector<uint8_t> ok;
	column_tiles(g, tiles, ok);
	std::vector<RowSums> rows(g.height - 1);
	parallel_for(
			g.height - 1,
			[&](size_t p_z) {
				int32_t tz = 0;
				if (!centre_tile(g.origin_xz[1], g.sample_spacing[1], p_z, tz)) {
					return;
				}
				RowSums &row = rows[p_z];
				CellQ c;
				size_t last = SIZE_MAX;
				for (size_t x = 0; x + 1 < g.width; ++x) {
					if (ok[x] == 0) {
						continue;
					}
					++row.cells;
					make_cell(g, p_terrain.frame, p_placement, x, p_z, c);
					row.open += c.cell.open ? 1 : 0;
					row.degenerate += c.cell.degenerate;
					row.nonfinite += c.cell.nonfinite;
					for (uint8_t k = 0; k < c.cell.count; ++k) {
						const std::array<int32_t, 2> tile = soup_tile_of(c.q[k]);
						if (last == SIZE_MAX || row.tiles[last].tile != tile) {
							last = SIZE_MAX;
							for (size_t i = row.tiles.size(); i-- > 0;) {
								if (row.tiles[i].tile == tile) {
									last = i;
									break;
								}
							}
							if (last == SIZE_MAX) {
								last = row.tiles.size();
								row.tiles.push_back({ tile, 0, 0 });
							}
						}
						row.tiles[last].triangles += 1;
						row.tiles[last].sum += triangle_hash(c.q[k], c.cell.triangles[k]);
						++row.triangles;
					}
				}
			},
			p_threads);
	std::map<std::array<int32_t, 2>, std::pair<size_t, uint64_t>> merged;
	for (RowSums &row : rows) {
		out.cells += row.cells;
		out.open += row.open;
		out.triangles += row.triangles;
		out.degenerate += row.degenerate;
		out.nonfinite += row.nonfinite;
		for (const SoupTerrainTileSum &t : row.tiles) {
			std::pair<size_t, uint64_t> &m = merged[t.tile];
			m.first += t.triangles;
			m.second += t.sum;
		}
		row = RowSums();
	}
	out.tiles.reserve(merged.size());
	for (const auto &[tile, m] : merged) {
		out.tiles.push_back({ tile, m.first, m.second });
	}
	// The running hash: one serial pass in emit order.
	CellQ c;
	for (size_t z = 0; z + 1 < g.height; ++z) {
		int32_t tz = 0;
		if (!centre_tile(g.origin_xz[1], g.sample_spacing[1], z, tz)) {
			continue;
		}
		for (size_t x = 0; x + 1 < g.width; ++x) {
			if (ok[x] == 0) {
				continue;
			}
			make_cell(g, p_terrain.frame, p_placement, x, z, c);
			for (uint8_t k = 0; k < c.cell.count; ++k) {
				const SoupTriangle &t = c.cell.triangles[k];
				soup_hash_bytes(out.hash, c.q[k], t.material, t.surface, t.body, t.flags);
			}
		}
	}
	return out;
}

} // namespace bf3::core::level
