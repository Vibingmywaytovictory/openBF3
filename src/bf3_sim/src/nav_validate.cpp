#include "nav_internal.h"

#include <cmath>
#include <cstring>
#include <string>

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

}

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

	constexpr float kSlack = 1.0f;
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

		if (c.pos[0] < h.bmin[0] || c.pos[0] > h.bmax[0] || c.pos[2] < h.bmin[2] || c.pos[2] > h.bmax[2]) {
			return core::make_error(where + ": off-mesh connection " + std::to_string(i) + " starts outside the tile");
		}
	}
	return core::Status::success();
}

}
