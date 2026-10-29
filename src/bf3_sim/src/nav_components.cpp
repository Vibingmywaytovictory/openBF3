#include "nav_internal.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>

namespace bf3::sim {

double poly_area(const dtMeshTile &p_tile, const dtPoly &p_poly) {
	double a = 0.0;
	for (int i = 0; i < p_poly.vertCount; ++i) {
		const float *p = &p_tile.verts[p_poly.verts[i] * 3];
		const float *q = &p_tile.verts[p_poly.verts[(i + 1) % p_poly.vertCount] * 3];
		a += double(p[0]) * q[2] - double(q[0]) * p[2];
	}
	return 0.5 * std::fabs(a);
}

namespace {

uint32_t find(std::vector<uint32_t> &r_parent, uint32_t p_x) {
	while (r_parent[p_x] != p_x) {
		r_parent[p_x] = r_parent[r_parent[p_x]];
		p_x = r_parent[p_x];
	}
	return p_x;
}

struct Graph {
	size_t n = 0;
	std::vector<uint32_t> start;
	std::vector<uint32_t> edges;
	std::vector<double> area;
};
Graph poly_graph(const NavWorld::Impl &w, std::vector<uint32_t> *r_parent) {
	Graph g;
	const size_t n = w.polys;
	g.n = n;
	if (w.mesh == nullptr || n == 0) {
		return g;
	}
	const dtNavMesh &mesh = *w.mesh;
	std::vector<uint32_t> &start = g.start;
	std::vector<double> &area = g.area;
	start.assign(n + 1, 0);
	area.assign(n, 0.0);
	for (int ti = 0; ti < mesh.getMaxTiles(); ++ti) {
		const dtMeshTile *tile = mesh.getTile(ti);
		if (tile == nullptr || tile->header == nullptr) {
			continue;
		}
		for (int pi = 0; pi < tile->header->polyCount; ++pi) {
			const uint32_t gi = w.poly_base[size_t(ti)] + uint32_t(pi);
			const dtPoly &poly = tile->polys[pi];
			uint32_t count = 0;
			for (unsigned int l = poly.firstLink; l != DT_NULL_LINK; l = tile->links[l].next) {
				++count;
			}
			start[gi + 1] = count;
			if (poly.getType() == DT_POLYTYPE_GROUND) {
				area[gi] = poly_area(*tile, poly);
			}
		}
	}
	for (size_t i = 0; i < n; ++i) {
		start[i + 1] += start[i];
	}
	std::vector<uint32_t> &edges = g.edges;
	edges.assign(start[n], 0);
	if (r_parent != nullptr) {
		r_parent->resize(n);
		std::iota(r_parent->begin(), r_parent->end(), 0u);
	}
	for (int ti = 0; ti < mesh.getMaxTiles(); ++ti) {
		const dtMeshTile *tile = mesh.getTile(ti);
		if (tile == nullptr || tile->header == nullptr) {
			continue;
		}
		for (int pi = 0; pi < tile->header->polyCount; ++pi) {
			const uint32_t gi = w.poly_base[size_t(ti)] + uint32_t(pi);
			uint32_t at = start[gi];
			for (unsigned int l = tile->polys[pi].firstLink; l != DT_NULL_LINK; l = tile->links[l].next) {
				const uint32_t h = w.global(NavPolyRef(tile->links[l].ref));
				edges[at++] = h == UINT32_MAX ? gi : h;
				if (h != UINT32_MAX && r_parent != nullptr) {
					const uint32_t a = find(*r_parent, gi), b = find(*r_parent, h);
					if (a != b) {
						(*r_parent)[std::max(a, b)] = std::min(a, b);
					}
				}
			}
		}
	}
	return g;
}

}

NavComponents nav_components(const NavWorld &p_world) {
	NavComponents out;
	const NavWorld::Impl &w = p_world.impl();
	const size_t n = w.polys;
	if (w.mesh == nullptr || n == 0) {
		return out;
	}
	std::vector<uint32_t> parent;
	const Graph graph = poly_graph(w, &parent);
	const std::vector<uint32_t> &start = graph.start;
	const std::vector<uint32_t> &edges = graph.edges;
	const std::vector<double> &area = graph.area;

	out.weak_component.assign(n, 0);
	for (uint32_t i = 0; i < n; ++i) {
		const uint32_t r = find(parent, i);
		if (r == i) {
			out.weak_component[i] = uint32_t(out.weak_count++);
		} else {
			out.weak_component[i] = out.weak_component[r];
		}
	}

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

std::vector<uint8_t> nav_components_reaching(const NavWorld &p_world, const NavComponents &p_components, uint32_t p_target) {
	std::vector<uint8_t> out(p_components.area.size(), 0);
	const NavWorld::Impl &w = p_world.impl();
	if (p_target >= out.size() || p_components.component.size() != w.polys) {
		return out;
	}
	const Graph g = poly_graph(w, nullptr);
	const size_t n = g.n;

	std::vector<uint32_t> rstart(n + 1, 0);
	for (uint32_t v = 0; v < n; ++v) {
		for (uint32_t e = g.start[v]; e < g.start[v + 1]; ++e) {
			++rstart[g.edges[e] + 1];
		}
	}
	for (size_t i = 0; i < n; ++i) {
		rstart[i + 1] += rstart[i];
	}
	std::vector<uint32_t> redges(rstart[n]);
	std::vector<uint32_t> fill(rstart.begin(), rstart.end() - 1);
	for (uint32_t v = 0; v < n; ++v) {
		for (uint32_t e = g.start[v]; e < g.start[v + 1]; ++e) {
			redges[fill[g.edges[e]]++] = v;
		}
	}
	std::vector<uint8_t> seen(n, 0);
	std::vector<uint32_t> todo;
	for (uint32_t v = 0; v < n; ++v) {
		if (p_components.component[v] == p_target) {
			seen[v] = 1;
			todo.push_back(v);
		}
	}
	while (!todo.empty()) {
		const uint32_t v = todo.back();
		todo.pop_back();
		out[p_components.component[v]] = 1;
		for (uint32_t e = rstart[v]; e < rstart[v + 1]; ++e) {
			const uint32_t u = redges[e];
			if (seen[u] == 0) {
				seen[u] = 1;
				todo.push_back(u);
			}
		}
	}
	return out;
}

std::vector<double> nav_component_area_in(const NavWorld &p_world, const NavComponents &p_components, const std::function<bool(float, float)> &p_inside) {
	std::vector<double> out(p_components.area.size(), 0.0);
	const NavWorld::Impl &w = p_world.impl();
	if (w.mesh == nullptr || p_components.component.size() != w.polys) {
		return out;
	}
	const dtNavMesh &mesh = *w.mesh;
	for (int ti = 0; ti < mesh.getMaxTiles(); ++ti) {
		const dtMeshTile *tile = mesh.getTile(ti);
		if (tile == nullptr || tile->header == nullptr) {
			continue;
		}
		for (int pi = 0; pi < tile->header->polyCount; ++pi) {
			const dtPoly &poly = tile->polys[pi];
			if (poly.getType() != DT_POLYTYPE_GROUND || poly.vertCount == 0) {
				continue;
			}
			float cx = 0.0f, cz = 0.0f;
			for (int i = 0; i < poly.vertCount; ++i) {
				cx += tile->verts[poly.verts[i] * 3 + 0];
				cz += tile->verts[poly.verts[i] * 3 + 2];
			}
			cx /= float(poly.vertCount);
			cz /= float(poly.vertCount);
			const uint32_t c = p_components.component[w.poly_base[size_t(ti)] + uint32_t(pi)];
			if (c < out.size() && p_inside(cx, cz)) {
				out[c] += poly_area(*tile, poly);
			}
		}
	}
	return out;
}

}
