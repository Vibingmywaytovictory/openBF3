#include "bf3/sim_harness/nav_input_check.h"

#include "nav_internal.h"
#include "static_world_internal.h"

#include "bf3/core/level/collision_soup.h"

#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace bf3::sim {

namespace {

struct Tri {
	std::array<Vec3, 3> v;
};

int64_t cell_of(float p_v, float p_size) {
	return int64_t(std::floor(double(p_v) / p_size));
}

uint64_t cell_key(int64_t p_x, int64_t p_y, int64_t p_z) {
	return (uint64_t(p_x) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(p_y) * 0xC2B2AE3D27D4EB4Full) ^ (uint64_t(p_z) * 0x165667B19E3779F9ull);
}

Vec3 centroid(const Tri &p_t) {
	return { (p_t.v[0][0] + p_t.v[1][0] + p_t.v[2][0]) / 3.0f, (p_t.v[0][1] + p_t.v[1][1] + p_t.v[2][1]) / 3.0f,
		(p_t.v[0][2] + p_t.v[1][2] + p_t.v[2][2]) / 3.0f };
}

float corner_error(const Tri &p_a, const Tri &p_b) {

	static constexpr int kPerm[6][3] = { { 0, 1, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 0, 2, 1 }, { 2, 1, 0 }, { 1, 0, 2 } };
	float best = 1e30f;
	for (const auto &perm : kPerm) {
		float worst = 0.0f;
		for (int k = 0; k < 3; ++k) {
			for (int c = 0; c < 3; ++c) {
				worst = std::max(worst, std::fabs(p_a.v[k][c] - p_b.v[perm[k]][c]));
			}
		}
		best = std::min(best, worst);
	}
	return best;
}

}

core::Result<NavInputCheck> nav_check_input_against_world(const StaticWorldShapes &p_world, const NavTileInput &p_input, const NavParams &p_params,
		uint32_t p_layer_mask, float p_tolerance) {
	NavInputCheck out;
	if (p_input.vertices.size() != p_input.layers.size() * 9) {
		return core::make_error("nav input check: vertices and layers differ");
	}
	const float b = nav_input_border(p_params);
	const float bx0 = float(p_input.tile[0]) * kNavTileSize - b, bx1 = float(p_input.tile[0] + 1) * kNavTileSize + b;
	const float bz0 = float(p_input.tile[1]) * kNavTileSize - b, bz1 = float(p_input.tile[1] + 1) * kNavTileSize + b;
	const auto overlaps = [&](const Tri &p_t) {
		const float x0 = std::min({ p_t.v[0][0], p_t.v[1][0], p_t.v[2][0] }), x1 = std::max({ p_t.v[0][0], p_t.v[1][0], p_t.v[2][0] });
		const float z0 = std::min({ p_t.v[0][2], p_t.v[1][2], p_t.v[2][2] }), z1 = std::max({ p_t.v[0][2], p_t.v[1][2], p_t.v[2][2] });
		return x0 < bx1 && x1 >= bx0 && z0 < bz1 && z1 >= bz0;
	};

	constexpr float kTerrainEdge = 1e-3f;
	const auto overlaps_by = [&](const Tri &p_t, float p_slack) {
		const float x0 = std::min({ p_t.v[0][0], p_t.v[1][0], p_t.v[2][0] }), x1 = std::max({ p_t.v[0][0], p_t.v[1][0], p_t.v[2][0] });
		const float z0 = std::min({ p_t.v[0][2], p_t.v[1][2], p_t.v[2][2] }), z1 = std::max({ p_t.v[0][2], p_t.v[1][2], p_t.v[2][2] });
		return x0 < bx1 + p_slack && x1 >= bx0 - p_slack && z0 < bz1 + p_slack && z1 >= bz0 - p_slack;
	};

	std::vector<Tri> world;
	const StaticWorldShapes::Impl &s = p_world.impl();
	const JPH::AABox box(JPH::Vec3(bx0, -1e6f, bz0), JPH::Vec3(bx1, 1e6f, bz1));
	std::vector<JPH::Float3> buffer(size_t(JPH::Shape::cGetTrianglesMinTrianglesRequested) * 8 * 3);
	for (const StaticWorldShapes::Impl::Tile &tile : s.tiles) {
		const StaticTileInfo &info = tile.info;
		if (info.max[0] < bx0 || info.min[0] >= bx1 || info.max[2] < bz0 || info.min[2] >= bz1) {
			continue;
		}
		const JPH::Vec3 origin = static_tile_origin(info.x, info.z);
		for (size_t layer = 0; layer < kStaticLayerCount; ++layer) {
			if (((p_layer_mask >> layer) & 1u) == 0 || tile.shapes[layer] == nullptr) {
				continue;
			}
			JPH::Shape::GetTrianglesContext context;
			tile.shapes[layer]->GetTrianglesStart(context, box, origin, JPH::Quat::sIdentity(), JPH::Vec3::sOne());
			while (true) {
				const int n = tile.shapes[layer]->GetTrianglesNext(context, int(buffer.size() / 3), buffer.data());
				if (n <= 0) {
					break;
				}
				for (int t = 0; t < n; ++t) {
					Tri tri;
					for (int k = 0; k < 3; ++k) {
						const JPH::Float3 &f = buffer[size_t(t) * 3 + size_t(k)];
						tri.v[size_t(k)] = { f.x, f.y, f.z };
					}
					if (overlaps(tri)) {
						world.push_back(tri);
					}
				}
			}
		}
	}

	std::vector<Tri> world_terrain;
	float terrain_tolerance = p_tolerance;
	if (((p_layer_mask >> uint32_t(StaticLayer::Collision)) & 1u) != 0) {
		for (const StaticWorldShapes::Impl::Terrain &terrain : s.terrains) {
			const StaticTerrainInfo &info = terrain.info;
			if (info.max[0] < bx0 || info.min[0] >= bx1 || info.max[2] < bz0 || info.min[2] >= bz1) {
				continue;
			}
			terrain_tolerance = std::max(terrain_tolerance, info.max_error + 1e-4f);
			const JPH::Quat q(terrain.rotation[0], terrain.rotation[1], terrain.rotation[2], terrain.rotation[3]);
			const JPH::Vec3 position(terrain.position[0], terrain.position[1], terrain.position[2]);
			for (const auto &[shape, at, rotation] : { std::tuple{ terrain.field.GetPtr(), position, q },
						 std::tuple{ terrain.patch.GetPtr(), JPH::Vec3::sZero(), JPH::Quat::sIdentity() } }) {
				if (shape == nullptr) {
					continue;
				}
				JPH::Shape::GetTrianglesContext context;
				shape->GetTrianglesStart(context, box, at, rotation, JPH::Vec3::sOne());

				std::unordered_set<uint64_t> field_seen;
				const bool is_field = shape == terrain.field.GetPtr();
				while (true) {
					const int n = shape->GetTrianglesNext(context, int(buffer.size() / 3), buffer.data());
					if (n <= 0) {
						break;
					}
					for (int t = 0; t < n; ++t) {
						if (is_field) {
							uint64_t h = 0xcbf29ce484222325ull;
							for (int k = 0; k < 3; ++k) {
								const JPH::Float3 &f = buffer[size_t(t) * 3 + size_t(k)];
								for (const float c : { f.x, f.y, f.z }) {
									uint32_t bits;
									std::memcpy(&bits, &c, sizeof(bits));
									h = (h ^ bits) * 0x100000001b3ull;
								}
							}
							if (!field_seen.insert(h).second) {
								continue;
							}
						}
						Tri tri;
						for (int k = 0; k < 3; ++k) {
							const JPH::Float3 &f = buffer[size_t(t) * 3 + size_t(k)];
							tri.v[size_t(k)] = { f.x, f.y, f.z };
						}
						if (overlaps_by(tri, kTerrainEdge)) {
							world_terrain.push_back(tri);
						}
					}
				}
			}
		}
	}
	out.world_triangles = world.size() + world_terrain.size();
	out.terrain_tolerance = terrain_tolerance;
	if (p_input.terrain_triangles > p_input.layers.size()) {
		return core::make_error("nav input check: more terrain triangles than triangles");
	}
	const size_t first_terrain = p_input.layers.size() - p_input.terrain_triangles;

	std::vector<Tri> nav;
	std::vector<uint8_t> nav_layer;
	std::vector<Tri> nav_terrain;
	for (size_t t = 0; t < p_input.layers.size(); ++t) {
		if (((p_layer_mask >> p_input.layers[t]) & 1u) == 0) {
			continue;
		}
		Tri tri;
		for (int k = 0; k < 3; ++k) {
			for (int c = 0; c < 3; ++c) {
				tri.v[size_t(k)][size_t(c)] = p_input.vertices[t * 9 + size_t(k) * 3 + size_t(c)];
			}
		}
		if (t >= first_terrain) {
			nav_terrain.push_back(tri);
			continue;
		}
		nav.push_back(tri);
		nav_layer.push_back(p_input.layers[t]);
	}
	out.nav_triangles = nav.size() + nav_terrain.size();

	const float cell = std::max(p_tolerance * 4.0f, 1e-4f);
	std::unordered_multimap<uint64_t, uint32_t> grid;
	grid.reserve(world.size());
	for (uint32_t i = 0; i < world.size(); ++i) {
		const Vec3 c = centroid(world[i]);
		grid.emplace(cell_key(cell_of(c[0], cell), cell_of(c[1], cell), cell_of(c[2], cell)), i);
	}
	std::vector<uint8_t> used(world.size(), 0);

	std::unordered_set<uint64_t> seen;
	const auto dropped_by_jolt = [&](const Tri &p_t, uint8_t p_layer) {
		std::array<int64_t, 9> q;
		if (!core::level::soup_quantise(p_t.v, q)) {
			return true;
		}
		const std::array<int32_t, 2> st = core::level::soup_tile_of(q);
		const JPH::Vec3 o = static_tile_origin(st[0], st[1]);
		const JPH::Vec3 a(p_t.v[0][0] - o.GetX(), p_t.v[0][1] - o.GetY(), p_t.v[0][2] - o.GetZ());
		const JPH::Vec3 bb(p_t.v[1][0] - o.GetX(), p_t.v[1][1] - o.GetY(), p_t.v[1][2] - o.GetZ());
		const JPH::Vec3 cc(p_t.v[2][0] - o.GetX(), p_t.v[2][1] - o.GetY(), p_t.v[2][2] - o.GetZ());
		if ((bb - a).Cross(cc - a).IsNearZero()) {
			return true;
		}

		const auto found = std::lower_bound(s.tiles.begin(), s.tiles.end(), st,
				[](const StaticWorldShapes::Impl::Tile &p_tile, const std::array<int32_t, 2> &p_key) { return std::array<int32_t, 2>{ p_tile.info.x, p_tile.info.z } < p_key; });
		if (found != s.tiles.end() && found->info.x == st[0] && found->info.z == st[1] && found->shapes[p_layer] != nullptr) {
			const JPH::AABox bounds = found->shapes[p_layer]->GetLocalBounds();
			const JPH::Vec3 scale = JPH::Vec3::sReplicate(float((1u << 21) - 1)) / JPH::Vec3::sMax(bounds.GetSize(), JPH::Vec3::sReplicate(1.0e-20f));
			const JPH::UVec4 qa = ((a - bounds.mMin) * scale + JPH::Vec3::sReplicate(0.5f)).ToInt();
			const JPH::UVec4 qb = ((bb - bounds.mMin) * scale + JPH::Vec3::sReplicate(0.5f)).ToInt();
			const JPH::UVec4 qc = ((cc - bounds.mMin) * scale + JPH::Vec3::sReplicate(0.5f)).ToInt();
			const auto eq = [](JPH::UVec4Arg p_x, JPH::UVec4Arg p_y) { return p_x.GetX() == p_y.GetX() && p_x.GetY() == p_y.GetY() && p_x.GetZ() == p_y.GetZ(); };
			if (eq(qa, qb) || eq(qb, qc) || eq(qa, qc)) {
				return true;
			}
		}

		std::array<std::array<int64_t, 3>, 3> corners{};
		for (int k = 0; k < 3; ++k) {
			corners[size_t(k)] = { q[size_t(k) * 3], q[size_t(k) * 3 + 1], q[size_t(k) * 3 + 2] };
		}
		const size_t lowest = size_t(std::min_element(corners.begin(), corners.end()) - corners.begin());
		uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t(p_layer) << 56) ^ (uint64_t(uint32_t(st[0])) << 20) ^ uint64_t(uint32_t(st[1]));
		for (int k = 0; k < 3; ++k) {
			for (int64_t c : corners[(lowest + size_t(k)) % 3]) {
				h = (h ^ uint64_t(c)) * 0x100000001b3ull;
			}
		}
		return !seen.insert(h).second;
	};
	for (size_t t = 0; t < nav.size(); ++t) {
		const Vec3 c = centroid(nav[t]);
		const int64_t cx = cell_of(c[0], cell), cy = cell_of(c[1], cell), cz = cell_of(c[2], cell);
		int64_t best = -1;
		float best_error = p_tolerance;
		for (int64_t dz = -1; dz <= 1; ++dz) {
			for (int64_t dy = -1; dy <= 1; ++dy) {
				for (int64_t dx = -1; dx <= 1; ++dx) {
					const auto range = grid.equal_range(cell_key(cx + dx, cy + dy, cz + dz));
					for (auto it = range.first; it != range.second; ++it) {
						if (used[it->second] != 0) {
							continue;
						}
						const float e = corner_error(nav[t], world[it->second]);
						if (e <= best_error) {
							best_error = e;
							best = int64_t(it->second);
						}
					}
				}
			}
		}
		if (best >= 0) {
			used[size_t(best)] = 1;
			++out.matched;
			out.max_corner_error = std::max(out.max_corner_error, best_error);

			dropped_by_jolt(nav[t], nav_layer[t]);
			continue;
		}
		++out.nav_only;
		if (dropped_by_jolt(nav[t], nav_layer[t])) {
			++out.nav_only_degenerate;
		}
	}
	for (uint8_t u : used) {
		out.world_only += u == 0 ? 1 : 0;
	}

	const float terrain_cell = std::max(terrain_tolerance * 4.0f, 1e-4f);
	std::unordered_multimap<uint64_t, uint32_t> terrain_grid;
	terrain_grid.reserve(world_terrain.size());
	for (uint32_t i = 0; i < world_terrain.size(); ++i) {
		const Vec3 c = centroid(world_terrain[i]);
		terrain_grid.emplace(cell_key(cell_of(c[0], terrain_cell), cell_of(c[1], terrain_cell), cell_of(c[2], terrain_cell)), i);
	}
	std::vector<uint8_t> terrain_used(world_terrain.size(), 0);
	for (const Tri &t : nav_terrain) {
		const Vec3 c = centroid(t);
		const int64_t cx = cell_of(c[0], terrain_cell), cy = cell_of(c[1], terrain_cell), cz = cell_of(c[2], terrain_cell);
		int64_t best = -1;
		float best_error = terrain_tolerance;
		for (int64_t dz = -1; dz <= 1; ++dz) {
			for (int64_t dy = -1; dy <= 1; ++dy) {
				for (int64_t dx = -1; dx <= 1; ++dx) {
					const auto range = terrain_grid.equal_range(cell_key(cx + dx, cy + dy, cz + dz));
					for (auto it = range.first; it != range.second; ++it) {
						if (terrain_used[it->second] != 0) {
							continue;
						}
						const float e = corner_error(t, world_terrain[it->second]);
						if (e <= best_error) {
							best_error = e;
							best = int64_t(it->second);
						}
					}
				}
			}
		}
		if (best >= 0) {
			terrain_used[size_t(best)] = 1;
			++out.matched;
			++out.terrain_matched;
			out.max_corner_error = std::max(out.max_corner_error, best_error);
			continue;
		}
		++out.nav_only;
	}

	for (size_t i = 0; i < terrain_used.size(); ++i) {
		out.world_only += terrain_used[i] == 0 && overlaps_by(world_terrain[i], -kTerrainEdge) ? 1 : 0;
	}
	return out;
}

}
