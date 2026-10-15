// nav_components (nav.h; docs/design/s1.md §5.5): the strongly connected components of the directed poly graph that
// Detour's links describe. Internal neighbours and tile portals are linked from both polys (both ways); an off-mesh
// drop link is start <-> connection poly -> landing (Detour links the landing back only for bidirectional
// connections), so a drop is one way. Iterative Tarjan over a CSR adjacency, O(polys + links); the weakly connected
// count by union-find over the same edges.
#include "nav_internal.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace bf3::sim {

namespace {

double poly_area(const dtMeshTile &p_tile, const dtPoly &p_poly) {
	double a = 0.0;
	for (int i = 0; i < p_poly.vertCount; ++i) {
		const float *p = &p_tile.verts[p_poly.verts[i] * 3];
		const float *q = &p_tile.verts[p_poly.verts[(i + 1) % p_poly.vertCount] * 3];
		a += double(p[0]) * q[2] - double(q[0]) * p[2];
	}
	return 0.5 * std::fabs(a);
}

uint32_t find(std::vector<uint32_t> &r_parent, uint32_t p_x) {
	while (r_parent[p_x] != p_x) {
		r_parent[p_x] = r_parent[r_parent[p_x]];
		p_x = r_parent[p_x];
	}
	return p_x;
}

} // namespace

NavComponents nav_components(const NavWorld &p_world) {
	NavComponents out;
	const NavWorld::Impl &w = p_world.impl();
	const size_t n = w.polys;
	if (w.mesh == nullptr || n == 0) {
		return out;
	}
	const dtNavMesh &mesh = *w.mesh;
	// CSR adjacency.
	std::vector<uint32_t> start(n + 1, 0);
	std::vector<double> area(n, 0.0);
	for (int ti = 0; ti < mesh.getMaxTiles(); ++ti) {
		const dtMeshTile *tile = mesh.getTile(ti);
		if (tile == nullptr || tile->header == nullptr) {
			continue;
		}
		for (int pi = 0; pi < tile->header->polyCount; ++pi) {
			const uint32_t g = w.poly_base[size_t(ti)] + uint32_t(pi);
			const dtPoly &poly = tile->polys[pi];
			uint32_t count = 0;
			for (unsigned int l = poly.firstLink; l != DT_NULL_LINK; l = tile->links[l].next) {
				++count;
			}
			start[g + 1] = count;
			if (poly.getType() == DT_POLYTYPE_GROUND) {
				area[g] = poly_area(*tile, poly);
			}
		}
	}
	for (size_t i = 0; i < n; ++i) {
		start[i + 1] += start[i];
	}
	std::vector<uint32_t> edges(start[n]);
	std::vector<uint32_t> parent(n);
	std::iota(parent.begin(), parent.end(), 0u);
	for (int ti = 0; ti < mesh.getMaxTiles(); ++ti) {
		const dtMeshTile *tile = mesh.getTile(ti);
		if (tile == nullptr || tile->header == nullptr) {
			continue;
		}
		for (int pi = 0; pi < tile->header->polyCount; ++pi) {
			const uint32_t g = w.poly_base[size_t(ti)] + uint32_t(pi);
			uint32_t at = start[g];
			for (unsigned int l = tile->polys[pi].firstLink; l != DT_NULL_LINK; l = tile->links[l].next) {
				const uint32_t h = w.global(NavPolyRef(tile->links[l].ref));
				edges[at++] = h == UINT32_MAX ? g : h; // a dangling ref becomes a self loop
				if (h != UINT32_MAX) {
					const uint32_t a = find(parent, g), b = find(parent, h);
					if (a != b) {
						parent[std::max(a, b)] = std::min(a, b);
					}
				}
			}
		}
	}
	// Weak ids, compact in poly order (a root's index is the smallest of its set, so roots come first).
	out.weak_component.assign(n, 0);
	for (uint32_t i = 0; i < n; ++i) {
		const uint32_t r = find(parent, i);
		if (r == i) {
			out.weak_component[i] = uint32_t(out.weak_count++);
		} else {
			out.weak_component[i] = out.weak_component[r];
		}
	}

	// Iterative Tarjan.
	constexpr uint32_t kUnvisited = UINT32_MAX;
	out.component.assign(n, kUnvisited);
	std::vector<uint32_t> index(n, kUnvisited), low(n, 0), edge_at(n, 0);
	std::vector<uint32_t> stack, call;
	std::vector<uint8_t> on_stack(n, 0);
	stack.reserve(n);
	call.reserve(n);
	uint32_t next_index = 0;
	uint32_t next_component = 0;
	for (uint32_t root = 0; root < n; ++root) {
		if (index[root] != kUnvisited) {
			continue;
		}
		call.push_back(root);
		index[root] = low[root] = next_index++;
		edge_at[root] = start[root];
		stack.push_back(root);
		on_stack[root] = 1;
		while (!call.empty()) {
			const uint32_t v = call.back();
			if (edge_at[v] < start[v + 1]) {
				const uint32_t u = edges[edge_at[v]++];
				if (index[u] == kUnvisited) {
					index[u] = low[u] = next_index++;
					edge_at[u] = start[u];
					stack.push_back(u);
					on_stack[u] = 1;
					call.push_back(u);
				} else if (on_stack[u] != 0) {
					low[v] = std::min(low[v], index[u]);
				}
				continue;
			}
			call.pop_back();
			if (!call.empty()) {
				const uint32_t p = call.back();
				low[p] = std::min(low[p], low[v]);
			}
			if (low[v] == index[v]) {
				uint32_t polys = 0;
				double sum = 0.0;
				while (true) {
					const uint32_t x = stack.back();
					stack.pop_back();
					on_stack[x] = 0;
					out.component[x] = next_component;
					++polys;
					sum += area[x];
					if (x == v) {
						break;
					}
				}
				out.polys.push_back(polys);
				out.area.push_back(sum);
				++next_component;
			}
		}
	}
	for (uint32_t c = 1; c < out.area.size(); ++c) {
		if (out.area[c] > out.area[out.largest]) {
			out.largest = c;
		}
	}
	return out;
}

} // namespace bf3::sim
