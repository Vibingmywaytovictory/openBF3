#include "bf3/render/mesh_prep.h"

#include <algorithm>
#include <limits>

namespace bf3::render {

using core::res::DecodedSubset;

core::Result<PreparedMesh> prepare_mesh(const core::res::LoadedMeshSet &p_mesh, size_t p_lod, core::res::SubsetFilter p_filter,
		const std::vector<std::vector<int32_t>> *p_lane_binds) {
	const std::string where = "mesh " + p_mesh.res.name;
	if (p_lod >= p_mesh.mesh.lods.size()) {
		return core::make_error(where + ": no LOD " + std::to_string(p_lod));
	}
	std::vector<DecodedSubset> subsets;
	const core::res::ResError e = p_mesh.decode(p_lod, p_filter, subsets);
	if (!e.ok()) {
		return core::make_error(where + ": " + e.message());
	}
	PreparedMesh out;
	out.name = p_mesh.res.name;
	out.skinned = p_lane_binds != nullptr;
	for (const DecodedSubset &d : subsets) {
		PreparedSurface s;
		s.subset = d.subset;
		s.material_index = d.material_index;
		s.material_name = p_mesh.mesh.lods[p_lod].subsets[d.subset].material_name;
		s.category = d.category;
		const size_t n = d.positions.size();
		if (n == 0 || d.indices.size() < 3) {
			continue;
		}
		s.positions.reserve(n * 3);
		s.aabb_min = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
		s.aabb_max = { -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };
		for (const core::res::Vec3 &p : d.positions) {
			s.positions.insert(s.positions.end(), p.begin(), p.end());
			for (int a = 0; a < 3; ++a) {
				s.aabb_min[size_t(a)] = std::min(s.aabb_min[size_t(a)], p[size_t(a)]);
				s.aabb_max[size_t(a)] = std::max(s.aabb_max[size_t(a)], p[size_t(a)]);
			}
		}
		if (d.normals && d.normals->size() == n) {
			s.normals.reserve(n * 3);
			for (const core::res::Vec3 &v : *d.normals) {
				s.normals.insert(s.normals.end(), v.begin(), v.end());
			}
			if (d.tangents && d.tangents->size() == n) {
				const bool signs = d.binormal_signs && d.binormal_signs->size() == n;
				s.tangents.reserve(n * 4);
				for (size_t i = 0; i < n; ++i) {
					const core::res::Vec3 &t = (*d.tangents)[i];
					s.tangents.insert(s.tangents.end(), t.begin(), t.end());
					s.tangents.push_back(signs && (*d.binormal_signs)[i] < 0.0f ? -1.0f : 1.0f);
				}
			}
		}
		if (d.uv0 && d.uv0->size() == n) {
			s.uv0.reserve(n * 2);
			for (const core::res::Vec2 &v : *d.uv0) {
				s.uv0.insert(s.uv0.end(), v.begin(), v.end());
			}
		}
		if (d.uv1 && d.uv1->size() == n) {
			s.uv1.reserve(n * 2);
			for (const core::res::Vec2 &v : *d.uv1) {
				s.uv1.insert(s.uv1.end(), v.begin(), v.end());
			}
		}
		if (d.colors && d.colors->size() == n) {
			s.colors.reserve(n * 4);
			for (const std::array<uint8_t, 4> &c : *d.colors) {
				s.colors.insert(s.colors.end(), c.begin(), c.end());
			}
		}
		if (d.parts && d.parts->size() == n) {
			s.parts = *d.parts;
		}
		if (p_lane_binds != nullptr) {
			if (d.subset >= p_lane_binds->size() || !d.bone_indices || d.bone_indices->size() != n) {
				return core::make_error(where + ": subset " + std::to_string(d.subset) + " has no bone indices to skin");
			}
			const std::vector<int32_t> &lanes = (*p_lane_binds)[d.subset];
			const uint8_t per_vertex = std::clamp<uint8_t>(p_mesh.mesh.lods[p_lod].subsets[d.subset].bones_per_vertex, 1, 4);
			const bool weights = d.bone_weights && d.bone_weights->size() == n;
			s.bones.assign(n * 4, 0);
			s.weights.assign(n * 4, 0.0f);
			for (size_t i = 0; i < n; ++i) {
				float sum = 0.0f;
				for (uint8_t l = 0; l < per_vertex; ++l) {
					const float w = weights ? (*d.bone_weights)[i][l] : (l == 0 ? 1.0f : 0.0f);
					if (w <= 0.0f) {
						continue;
					}
					const uint16_t lane = (*d.bone_indices)[i][l];
					if (lane >= lanes.size()) {
						return core::make_error(where + ": subset " + std::to_string(d.subset) + " vertex " + std::to_string(i) + " lane " + std::to_string(lane) +
								" outside its " + std::to_string(lanes.size()) + " bone slots");
					}
					s.bones[i * 4 + l] = lanes[lane];
					s.weights[i * 4 + l] = w;
					sum += w;
				}
				if (sum <= 0.0f) {
					// One-bone subsets store no usable weights (the M9: BoneWeights all 0): the single lane is the bone.
					const uint16_t lane = (*d.bone_indices)[i][0];
					if (lane >= lanes.size()) {
						return core::make_error(where + ": subset " + std::to_string(d.subset) + " vertex " + std::to_string(i) + " lane " + std::to_string(lane) +
								" outside its " + std::to_string(lanes.size()) + " bone slots");
					}
					s.bones[i * 4] = lanes[lane];
					s.weights[i * 4] = 1.0f;
				} else if (sum != 1.0f) {
					for (int l = 0; l < 4; ++l) {
						s.weights[i * 4 + size_t(l)] /= sum;
					}
				}
			}
		}
		const size_t tris = d.indices.size() / 3;
		s.indices.reserve(tris * 3);
		for (size_t t = 0; t < tris; ++t) {
			const uint32_t i0 = d.indices[t * 3], i1 = d.indices[t * 3 + 1], i2 = d.indices[t * 3 + 2];
			if (i0 >= n || i1 >= n || i2 >= n) {
				return core::make_error(where + ": subset " + std::to_string(d.subset) + " index outside its vertices");
			}
			s.indices.push_back(int32_t(i0));
			s.indices.push_back(int32_t(i2));
			s.indices.push_back(int32_t(i1));
		}
		out.vertices += n;
		out.triangles += tris;
		out.surfaces.push_back(std::move(s));
	}
	return out;
}

} // namespace bf3::render
