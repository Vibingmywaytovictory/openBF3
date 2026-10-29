#include "nav_internal.h"

namespace bf3::sim {

namespace {

Vec3 vertex(const dtMeshTile &p_tile, const dtPoly &p_poly, int p_index) {
	const float *v = &p_tile.verts[p_poly.verts[p_index] * 3];
	return { v[0], v[1], v[2] };
}

}

NavPolyGraph nav_poly_graph(const NavWorld &p_world) {
	NavPolyGraph out;
	const NavWorld::Impl &w = p_world.impl();
	if (w.mesh == nullptr || w.polys == 0) {
		return out;
	}
	const dtNavMesh &mesh = *w.mesh;
	out.polys.resize(w.polys);
	out.first_link.assign(w.polys + 1, 0);
	for (int ti = 0; ti < mesh.getMaxTiles(); ++ti) {
		const dtMeshTile *tile = mesh.getTile(ti);
		if (tile == nullptr || tile->header == nullptr) {
			continue;
		}
		for (int pi = 0; pi < tile->header->polyCount; ++pi) {
			const uint32_t gi = w.poly_base[size_t(ti)] + uint32_t(pi);
			const dtPoly &poly = tile->polys[pi];
			NavGraphPoly &node = out.polys[gi];
			node.ref = NavPolyRef(mesh.getPolyRefBase(tile) | dtPolyRef(pi));
			node.area = poly.getArea();
			node.ground = poly.getType() == DT_POLYTYPE_GROUND;
			Vec3 sum{};
			for (int i = 0; i < poly.vertCount; ++i) {
				const Vec3 p = vertex(*tile, poly, i);
				sum.x += p.x;
				sum.y += p.y;
				sum.z += p.z;
			}
			const float n = poly.vertCount > 0 ? float(poly.vertCount) : 1.0f;
			node.centre = { sum.x / n, sum.y / n, sum.z / n };
			node.area_m2 = node.ground ? float(poly_area(*tile, poly)) : 0.0f;
			uint32_t count = 0;
			for (unsigned int l = poly.firstLink; l != DT_NULL_LINK; l = tile->links[l].next) {
				++count;
			}
			out.first_link[gi + 1] = count;
		}
	}
	for (size_t i = 0; i < w.polys; ++i) {
		out.first_link[i + 1] += out.first_link[i];
	}
	out.links.resize(out.first_link[w.polys]);
	for (int ti = 0; ti < mesh.getMaxTiles(); ++ti) {
		const dtMeshTile *tile = mesh.getTile(ti);
		if (tile == nullptr || tile->header == nullptr) {
			continue;
		}
		for (int pi = 0; pi < tile->header->polyCount; ++pi) {
			const uint32_t gi = w.poly_base[size_t(ti)] + uint32_t(pi);
			const dtPoly &poly = tile->polys[pi];
			uint32_t at = out.first_link[gi];
			for (unsigned int l = poly.firstLink; l != DT_NULL_LINK; l = tile->links[l].next) {
				const dtLink &link = tile->links[l];
				NavGraphLink &edge = out.links[at++];
				edge.to = w.global(NavPolyRef(link.ref));
				const int a = link.edge < poly.vertCount ? link.edge : 0;
				const int b = poly.vertCount > 1 ? (a + 1) % poly.vertCount : a;
				const Vec3 p = vertex(*tile, poly, a);
				const Vec3 q = vertex(*tile, poly, b);
				edge.portal = { 0.5f * (p.x + q.x), 0.5f * (p.y + q.y), 0.5f * (p.z + q.z) };
			}
		}
	}
	return out;
}

uint32_t nav_poly_index(const NavWorld &p_world, NavPolyRef p_poly) {
	return p_world.impl().global(p_poly);
}

}
