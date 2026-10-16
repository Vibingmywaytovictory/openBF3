// validate_detour_tile (docs/design/s1.md §9) and nav_check_input_against_world (§5.1 proof 1).
//
// Detour 1.6.0 lays a tile out (dtCreateNavMeshData; dtNavMesh::addTile reads it back the same way) as aligned
// sections after the header: verts (vertCount x 3 floats), polys (polyCount dtPoly), links (maxLinkCount dtLink),
// detail meshes (detailMeshCount dtPolyDetail), detail verts (detailVertCount x 3 floats), detail tris
// (detailTriCount x 4 bytes), BV nodes (bvNodeCount dtBVNode), off-mesh connections (offMeshConCount). addTile checks
// only the magic and the version and then trusts every index, so this mirrors the layout and checks every count,
// index and coordinate first. A Recast/Detour bump changes recast_build_id() and with it every cache key.
#include "nav_internal.h"

#include "static_world_internal.h"

#include "bf3/core/level/collision_soup.h"

#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace bf3::sim {

namespace {

constexpr int kAlign = 4;

uint64_t align4(uint64_t p_v) {
	return (p_v + (kAlign - 1)) & ~uint64_t(kAlign - 1);
}

bool finite3(const float *p_v) {
	return std::isfinite(p_v[0]) && std::isfinite(p_v[1]) && std::isfinite(p_v[2]) && std::fabs(p_v[0]) < 1e7f && std::fabs(p_v[1]) < 1e7f &&
			std::fabs(p_v[2]) < 1e7f;
}

} // namespace

core::Status validate_detour_tile(std::span<const uint8_t> p_data, const NavParams &p_params, const NavTileKey &p_tile) {
	const std::string where = "detour tile (" + std::to_string(p_tile[0]) + ", " + std::to_string(p_tile[1]) + ")";
	if (p_data.size() < sizeof(dtMeshHeader)) {
		return core::make_error(where + ": " + std::to_string(p_data.size()) + " bytes, shorter than the header");
	}
	if (p_data.size() > (size_t(1) << 30)) {
		return core::make_error(where + ": larger than 1 GiB");
	}
	dtMeshHeader h;
	std::memcpy(&h, p_data.data(), sizeof(h));
	if (h.magic != DT_NAVMESH_MAGIC || h.version != DT_NAVMESH_VERSION) {
		return core::make_error(where + ": bad magic or version");
	}
	if (h.x != p_tile[0] || h.y != p_tile[1] || h.layer != 0) {
		return core::make_error(where + ": the header names tile (" + std::to_string(h.x) + ", " + std::to_string(h.y) + ", layer " + std::to_string(h.layer) + ")");
	}
	if (h.polyCount <= 0 || h.polyCount > (1 << DT_POLY_BITS) || h.vertCount < 3 || h.vertCount > 0xffff || h.maxLinkCount <= 0 ||
			h.detailMeshCount < 0 || h.detailVertCount < 0 || h.detailTriCount < 0 || h.bvNodeCount < 0 || h.offMeshConCount < 0 ||
			h.offMeshConCount > h.polyCount || h.offMeshBase != h.polyCount - h.offMeshConCount || h.detailMeshCount != h.offMeshBase ||
			h.bvNodeCount > 2 * h.polyCount + 1) {
		return core::make_error(where + ": inconsistent counts");
	}
	if (h.walkableHeight != p_params.crouch_height || h.walkableRadius != p_params.agent_radius || h.walkableClimb != p_params.agent_climb) {
		return core::make_error(where + ": baked for another agent");
	}
	if (!finite3(h.bmin) || !finite3(h.bmax) || !std::isfinite(h.bvQuantFactor) || h.bvQuantFactor <= 0.0f || h.bmin[0] > h.bmax[0] ||
			h.bmin[1] > h.bmax[1] || h.bmin[2] > h.bmax[2]) {
		return core::make_error(where + ": bad bounds");
	}
	const float tx0 = float(p_tile[0]) * kNavTileSize, tz0 = float(p_tile[1]) * kNavTileSize;
	if (std::fabs(h.bmin[0] - tx0) > 0.01f || std::fabs(h.bmin[2] - tz0) > 0.01f || std::fabs(h.bmax[0] - (tx0 + kNavTileSize)) > 0.01f ||
			std::fabs(h.bmax[2] - (tz0 + kNavTileSize)) > 0.01f) {
		return core::make_error(where + ": the bounds are not the tile's square");
	}
	const uint64_t header_size = align4(sizeof(dtMeshHeader));
	const uint64_t verts_size = align4(sizeof(float) * 3 * uint64_t(h.vertCount));
	const uint64_t polys_size = align4(sizeof(dtPoly) * uint64_t(h.polyCount));
	const uint64_t links_size = align4(sizeof(dtLink) * uint64_t(h.maxLinkCount));
	const uint64_t detail_meshes_size = align4(sizeof(dtPolyDetail) * uint64_t(h.detailMeshCount));
	const uint64_t detail_verts_size = align4(sizeof(float) * 3 * uint64_t(h.detailVertCount));
	const uint64_t detail_tris_size = align4(4 * uint64_t(h.detailTriCount));
	const uint64_t bv_size = align4(sizeof(dtBVNode) * uint64_t(h.bvNodeCount));
	const uint64_t offmesh_size = align4(sizeof(dtOffMeshConnection) * uint64_t(h.offMeshConCount));
	const uint64_t total = header_size + verts_size + polys_size + links_size + detail_meshes_size + detail_verts_size + detail_tris_size + bv_size + offmesh_size;
	if (total != p_data.size()) {
		return core::make_error(where + ": " + std::to_string(p_data.size()) + " bytes, the header's layout needs " + std::to_string(total));
	}
	const uint8_t *base = p_data.data();
	uint64_t at = header_size;
	const auto section = [&](uint64_t p_size) {
		const uint8_t *p = base + at;
		at += p_size;
		return p;
	};
	// Read through memcpy into aligned copies (the span may not be aligned).
	std::vector<float> verts(size_t(h.vertCount) * 3);
	std::memcpy(verts.data(), section(verts_size), verts.size() * sizeof(float));
	std::vector<dtPoly> polys(size_t(h.polyCount));
	std::memcpy(polys.data(), section(polys_size), polys.size() * sizeof(dtPoly));
	section(links_size);
	std::vector<dtPolyDetail> details(size_t(h.detailMeshCount));
	std::memcpy(details.data(), section(detail_meshes_size), details.size() * sizeof(dtPolyDetail));
	std::vector<float> dverts(size_t(h.detailVertCount) * 3);
	std::memcpy(dverts.data(), section(detail_verts_size), dverts.size() * sizeof(float));
	std::vector<uint8_t> dtris(size_t(h.detailTriCount) * 4);
	std::memcpy(dtris.data(), section(detail_tris_size), dtris.size());
	std::vector<dtBVNode> nodes(size_t(h.bvNodeCount));
	std::memcpy(nodes.data(), section(bv_size), nodes.size() * sizeof(dtBVNode));
	std::vector<dtOffMeshConnection> cons(size_t(h.offMeshConCount));
	std::memcpy(cons.data(), section(offmesh_size), cons.size() * sizeof(dtOffMeshConnection));

	constexpr float kSlack = 1.0f; // detail heights may stand a little off the header's y range
	for (int i = 0; i < h.vertCount; ++i) {
		if (!finite3(&verts[size_t(i) * 3])) {
			return core::make_error(where + ": vertex " + std::to_string(i) + " is not finite");
		}
	}
	const int ground = h.offMeshBase;
	for (int i = 0; i < h.polyCount; ++i) {
		const dtPoly &p = polys[size_t(i)];
		const bool offmesh = i >= ground;
		if (p.getType() != (offmesh ? DT_POLYTYPE_OFFMESH_CONNECTION : DT_POLYTYPE_GROUND)) {
			return core::make_error(where + ": poly " + std::to_string(i) + " has the wrong type");
		}
		if (offmesh ? p.vertCount != 2 : (p.vertCount < 3 || p.vertCount > DT_VERTS_PER_POLYGON)) {
			return core::make_error(where + ": poly " + std::to_string(i) + " has " + std::to_string(p.vertCount) + " vertices");
		}
		for (int j = 0; j < p.vertCount; ++j) {
			if (p.verts[j] >= h.vertCount) {
				return core::make_error(where + ": poly " + std::to_string(i) + " names vertex " + std::to_string(p.verts[j]));
			}
			if (!offmesh) {
				const float *v = &verts[size_t(p.verts[j]) * 3];
				if (v[0] < h.bmin[0] - kSlack || v[0] > h.bmax[0] + kSlack || v[2] < h.bmin[2] - kSlack || v[2] > h.bmax[2] + kSlack ||
						v[1] < h.bmin[1] - kSlack || v[1] > h.bmax[1] + kSlack) {
					return core::make_error(where + ": poly " + std::to_string(i) + " has a vertex outside the tile");
				}
			}
			const unsigned short nei = p.neis[j];
			if (offmesh) {
				continue;
			}
			if (nei & DT_EXT_LINK) {
				const unsigned side = nei & 0xff;
				if ((nei & 0x7f00) != 0 || (side != 0 && side != 2 && side != 4 && side != 6)) {
					return core::make_error(where + ": poly " + std::to_string(i) + " has a bad portal flag");
				}
			} else if (nei > ground) {
				return core::make_error(where + ": poly " + std::to_string(i) + " names neighbour " + std::to_string(nei));
			}
		}
		if (!offmesh) {
			const dtPolyDetail &d = details[size_t(i)];
			if (uint64_t(d.vertBase) + d.vertCount > uint64_t(h.detailVertCount) || uint64_t(d.triBase) + d.triCount > uint64_t(h.detailTriCount)) {
				return core::make_error(where + ": detail mesh " + std::to_string(i) + " is outside the detail arrays");
			}
			// Detour's closest-point search over the boundary edges of the detail triangles dereferences the edge it
			// found, so a poly needs at least one triangle and one boundary edge (found by the sim.nav.unit fuzz:
			// cleared edge flags crashed dtNavMesh::closestPointOnPoly inside addTile).
			bool boundary = false;
			if (d.triCount == 0) {
				return core::make_error(where + ": poly " + std::to_string(i) + " has no detail triangle");
			}
			for (unsigned t = 0; t < d.triCount; ++t) {
				const uint8_t *tri = &dtris[(size_t(d.triBase) + t) * 4];
				for (int k = 0; k < 3; ++k) {
					if (tri[k] >= p.vertCount + d.vertCount) {
						return core::make_error(where + ": detail triangle of poly " + std::to_string(i) + " names vertex " + std::to_string(tri[k]));
					}
					boundary = boundary || (dtGetDetailTriEdgeFlags(tri[3], k) & DT_DETAIL_EDGE_BOUNDARY) != 0;
				}
			}
			if (!boundary) {
				return core::make_error(where + ": the detail triangles of poly " + std::to_string(i) + " have no boundary edge");
			}
		}
	}
	for (int i = 0; i < h.detailVertCount; ++i) {
		if (!finite3(&dverts[size_t(i) * 3])) {
			return core::make_error(where + ": detail vertex " + std::to_string(i) + " is not finite");
		}
	}
	for (int i = 0; i < h.bvNodeCount; ++i) {
		const dtBVNode &n = nodes[size_t(i)];
		if (n.i >= 0 && n.i >= ground) {
			return core::make_error(where + ": BV node " + std::to_string(i) + " names poly " + std::to_string(n.i));
		}
		if (n.i < 0 && int64_t(i) - int64_t(n.i) > int64_t(h.bvNodeCount)) {
			return core::make_error(where + ": BV node " + std::to_string(i) + " escapes past the tree");
		}
		for (int k = 0; k < 3; ++k) {
			if (n.bmin[k] > n.bmax[k]) {
				return core::make_error(where + ": BV node " + std::to_string(i) + " has inverted bounds");
			}
		}
	}
	for (int i = 0; i < h.offMeshConCount; ++i) {
		const dtOffMeshConnection &c = cons[size_t(i)];
		if (c.poly != ground + i || !finite3(c.pos) || !finite3(c.pos + 3) || !std::isfinite(c.rad) || c.rad < 0.0f || c.rad > 16.0f) {
			return core::make_error(where + ": off-mesh connection " + std::to_string(i) + " is bad");
		}
		// The start must lie in the tile (dtCreateNavMeshData keeps only those).
		if (c.pos[0] < h.bmin[0] || c.pos[0] > h.bmax[0] || c.pos[2] < h.bmin[2] || c.pos[2] > h.bmax[2]) {
			return core::make_error(where + ": off-mesh connection " + std::to_string(i) + " starts outside the tile");
		}
	}
	return core::Status::success();
}

// ---- input = motor set --------------------------------------------------------------------------------------------

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
	// The best of the six corner correspondences (rotations and both windings).
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

} // namespace

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
	// TC: the same with the box grown (p_slack > 0) or shrunk (< 0) by p_slack metres on every side.
	constexpr float kTerrainEdge = 1e-3f;
	const auto overlaps_by = [&](const Tri &p_t, float p_slack) {
		const float x0 = std::min({ p_t.v[0][0], p_t.v[1][0], p_t.v[2][0] }), x1 = std::max({ p_t.v[0][0], p_t.v[1][0], p_t.v[2][0] });
		const float z0 = std::min({ p_t.v[0][2], p_t.v[1][2], p_t.v[2][2] }), z1 = std::max({ p_t.v[0][2], p_t.v[1][2], p_t.v[2][2] });
		return x0 < bx1 + p_slack && x1 >= bx0 - p_slack && z0 < bz1 + p_slack && z1 >= bz0 - p_slack;
	};
	// The motor's triangles: read back from every cooked MeshShape whose tile bounds overlap the box.
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
	// TC: the terrain's triangles, read back from every terrain field (at its body transform) and patch whose bounds
	// overlap the box, when the Collision layer is in the mask; matched against the input's terrain triangles (the
	// last NavTileInput::terrain_triangles) within the terrain tolerance.
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
				// Jolt's HeightFieldShape resumes a walk that filled the buffer at the start of the block it stopped in, so
				// the triangles of that block already returned come again (HeightFieldShape.cpp VisitTriangle): a field
				// triangle equal bit for bit to one returned before is dropped.
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
	// The nav triangles of the mask (Havok, then terrain).
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
	// Hash the world triangles by centroid cell (cells of 4 x the tolerance, so a match is in the 27 around).
	const float cell = std::max(p_tolerance * 4.0f, 1e-4f);
	std::unordered_multimap<uint64_t, uint32_t> grid;
	grid.reserve(world.size());
	for (uint32_t i = 0; i < world.size(); ++i) {
		const Vec3 c = centroid(world[i]);
		grid.emplace(cell_key(cell_of(c[0], cell), cell_of(c[1], cell), cell_of(c[2], cell)), i);
	}
	std::vector<uint8_t> used(world.size(), 0);
	// Jolt's sanitize rule for an unmatched nav triangle: degenerate relative to its static tile's origin (the cook's
	// frame; the tile is the soup's filing rule), degenerate after the 21-bit quantisation over the shape's bounds, or
	// a duplicate (the same three corners) of an earlier triangle of the same tile and layer.
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
		// Quantised degeneracy over the cooked shape's bounds (when that tile has the shape).
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
		// Duplicate: the corners as a set (Jolt compares vertex index triples with the lowest first; the cook shares
		// vertices by exact bits, so equal corners are equal indices).
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
			// Register the matched triangle too, so a later duplicate of it counts as Jolt's.
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
	// TC: the terrain group, one to one within the terrain tolerance (a terrain triangle is never degenerate for Jolt
	// on a sane grid, so an unmatched one counts as nav_only or world_only without the sanitise rule).
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
	// An unmatched field triangle counts only when it overlaps the box by more than the edge slack: the field's corners
	// (offset + scale x sample, in float) and the soup's (origin + sample x spacing) can differ by an ulp, which moves a
	// triangle that only touches the box edge in or out of one side's set.
	for (size_t i = 0; i < terrain_used.size(); ++i) {
		out.world_only += terrain_used[i] == 0 && overlaps_by(world_terrain[i], -kTerrainEdge) ? 1 : 0;
	}
	return out;
}

} // namespace bf3::sim
