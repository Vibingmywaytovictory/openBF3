#include "bf3/render/vehicle_meshes.h"

#include "bf3/core/util/elapsed.h"
#include "bf3/render/mesh_cache.h"
#include "bf3/render/slot_textures.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <limits>
#include <map>
#include <utility>

namespace bf3::render {

namespace {

template <class T>
void gather(const std::vector<T> &p_from, std::vector<T> &r_to, size_t p_width, std::span<const int32_t> p_vertices) {
	if (p_from.empty()) {
		return;
	}
	r_to.reserve(p_vertices.size() * p_width);
	for (const int32_t v : p_vertices) {
		for (size_t k = 0; k < p_width; ++k) {
			r_to.push_back(p_from[size_t(v) * p_width + k]);
		}
	}
}

PreparedSurface part_surface(const PreparedSurface &p_s, std::span<const int32_t> p_indices) {
	PreparedSurface out;
	out.subset = p_s.subset;
	out.material_index = p_s.material_index;
	out.material_name = p_s.material_name;
	out.category = p_s.category;
	std::map<int32_t, int32_t> remap;
	std::vector<int32_t> vertices;
	out.indices.reserve(p_indices.size());
	for (const int32_t i : p_indices) {
		const auto [at, fresh] = remap.emplace(i, int32_t(vertices.size()));
		if (fresh) {
			vertices.push_back(i);
		}
		out.indices.push_back(at->second);
	}
	gather(p_s.positions, out.positions, 3, vertices);
	gather(p_s.normals, out.normals, 3, vertices);
	gather(p_s.tangents, out.tangents, 4, vertices);
	gather(p_s.uv0, out.uv0, 2, vertices);
	gather(p_s.uv1, out.uv1, 2, vertices);
	gather(p_s.colors, out.colors, 4, vertices);
	gather(p_s.bones, out.bones, 4, vertices);
	gather(p_s.weights, out.weights, 4, vertices);
	gather(p_s.parts, out.parts, 1, vertices);
	out.aabb_min = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
		std::numeric_limits<float>::max() };
	out.aabb_max = { -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
		-std::numeric_limits<float>::max() };
	for (size_t v = 0; v < out.vertex_count(); ++v) {
		for (size_t k = 0; k < 3; ++k) {
			out.aabb_min[k] = std::min(out.aabb_min[k], out.positions[v * 3 + k]);
			out.aabb_max[k] = std::max(out.aabb_max[k], out.positions[v * 3 + k]);
		}
	}
	return out;
}

}

core::Vec3 VehicleMeshPart::centre() const {
	return { 0.5f * (bounds_min[0] + bounds_max[0]), 0.5f * (bounds_min[1] + bounds_max[1]),
		0.5f * (bounds_min[2] + bounds_max[2]) };
}

const VehicleMeshKind *VehicleMeshSet::kind(const std::string &p_blueprint) const {
	for (const VehicleMeshKind &k : kinds) {
		if (k.blueprint == p_blueprint) {
			return &k;
		}
	}
	return nullptr;
}

const PreparedTexture *VehicleMeshSet::texture(const std::string &p_name) const {
	for (const PreparedTexture &t : textures) {
		if (t.name == p_name) {
			return &t;
		}
	}
	return nullptr;
}

std::vector<VehicleMeshPart> split_mesh_parts(const PreparedMesh &p_mesh) {
	std::map<uint16_t, VehicleMeshPart> parts;
	for (const PreparedSurface &s : p_mesh.surfaces) {
		std::map<uint16_t, std::vector<int32_t>> triangles;
		for (size_t t = 0; t + 2 < s.indices.size(); t += 3) {
			const int32_t first = s.indices[t];
			const uint16_t part = s.parts.empty() ? uint16_t(0) : s.parts[size_t(first)];
			std::vector<int32_t> &list = triangles[part];
			list.insert(list.end(), s.indices.begin() + std::ptrdiff_t(t), s.indices.begin() + std::ptrdiff_t(t + 3));
		}
		for (const auto &[part, indices] : triangles) {
			VehicleMeshPart &p = parts[part];
			p.part = part;
			p.prepared.name = p_mesh.name;
			p.prepared.surfaces.push_back(part_surface(s, indices));
		}
	}
	std::vector<VehicleMeshPart> out;
	for (auto &[part, p] : parts) {
		p.bounds_min = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max() };
		p.bounds_max = { -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
			-std::numeric_limits<float>::max() };
		for (const PreparedSurface &s : p.prepared.surfaces) {
			p.prepared.vertices += s.vertex_count();
			p.prepared.triangles += s.indices.size() / 3;
			for (size_t k = 0; k < 3; ++k) {
				p.bounds_min[k] = std::min(p.bounds_min[k], s.aabb_min[k]);
				p.bounds_max[k] = std::max(p.bounds_max[k], s.aabb_max[k]);
			}
		}
		out.push_back(std::move(p));
	}
	return out;
}

core::Result<std::unique_ptr<VehicleMeshSet>> load_vehicle_meshes(const core::EbxResolver &p_db,
		const core::LevelMount &p_mount, std::span<const VehicleMeshSpec> p_specs, core::Cache *p_cache) {
	const auto start = std::chrono::steady_clock::now();
	auto set = std::make_unique<VehicleMeshSet>();
	const std::vector<std::string> databases = variation_database_names(p_mount);
	for (const VehicleMeshSpec &spec : p_specs) {
		const std::string what = "vehicle " + spec.blueprint;
		if (spec.mesh.empty()) {
			return core::make_error(what + ": no Mesh");
		}
		VehicleMeshKind kind;
		kind.blueprint = spec.blueprint;
		kind.mesh = spec.mesh;
		std::vector<std::pair<std::string, core::Transform>> meshes{ { spec.mesh, core::Transform::identity() } };
		meshes.insert(meshes.end(), spec.component_meshes.begin(), spec.component_meshes.end());
		for (size_t m = 0; m < meshes.size(); ++m) {
			const std::string &name = meshes[m].first;
			core::Result<MeshMaterials> materials = mesh_materials(p_db, name, 0, databases);
			if (!materials) {
				return core::error_at(what + ": " + name, materials.error());
			}
			MeshRequest request;
			request.lods = { 0 };
			request.filter = core::res::SubsetFilter::Drawn;
			core::Result<CachedMesh> mesh = load_mesh_cached(p_cache, p_mount, name, request);
			if (!mesh) {
				return core::error_at(what + ": " + name, mesh.error());
			}
			if (mesh->mesh.meshes.size() != 1) {
				return core::make_error(what + ": " + name + ": no LOD 0 prepared");
			}
			for (VehicleMeshPart &part : split_mesh_parts(mesh->mesh.meshes[0])) {
				part.component = m > 0;
				part.rest = meshes[m].second;
				kind.parts.push_back(std::move(part));
				kind.part_mesh.push_back(uint8_t(m));
			}
			kind.materials.push_back(std::move(*materials));
			if (m == 0) {
				kind.cache = mesh->item;
			}
		}
		set->kinds.push_back(std::move(kind));
	}
	SlotTextureWants wants;
	for (const VehicleMeshKind &kind : set->kinds) {
		for (size_t p = 0; p < kind.parts.size(); ++p) {
			wants.add(kind.materials[kind.part_mesh[p]], kind.parts[p].prepared.surfaces);
		}
	}
	set->textures = load_slot_textures(p_mount, wants, "vehicle", set->notes);
	set->seconds = core::seconds_since(start);
	return set;
}

}
