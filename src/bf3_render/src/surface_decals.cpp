#include "bf3/render/surface_decals.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/util/text.h"
#include "bf3/render/roads.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace bf3::render {

namespace {

using core::to_lower_ascii;

constexpr std::array<std::string_view, 2> kShapeTypes{ "RoadData", "TerrainQuadDecalData" };

std::optional<std::string> shader_name(
		const core::EbxResolver &p_db, const core::EbxInstance &p_shape, std::string_view p_field) {
	const std::optional<core::EbxInstance> shader = p_db.follow(p_shape.data(), p_field);
	if (!shader) {
		return std::nullopt;
	}
	const std::optional<std::string_view> name = p_db.name_of(shader->partition().guid());
	if (!name) {
		return std::nullopt;
	}
	return to_lower_ascii(*name);
}

}

SurfaceDecalShapes surface_decal_shapes(const core::EbxResolver &p_db, std::string_view p_asset) {
	SurfaceDecalShapes out;
	const core::EbxPartition *partition = p_db.partition_by_name(terrain_decals_name(p_asset));
	if (partition == nullptr) {
		return out;
	}
	for (const std::string_view type : kShapeTypes) {
		for (const core::EbxInstance &shape : partition->instances_of(type)) {
			++out.shapes;
			const std::optional<std::string> planar = shader_name(p_db, shape, "Shader2d");
			const std::optional<std::string> surface = shader_name(p_db, shape, "Shader3d");
			if (!planar && !surface) {
				++out.without_shader;
			} else if (surface) {
				(planar ? out.paired : out.standalone).insert(*surface);
			}
		}
	}
	// INFERRED: a Shader3d shape that also has a Shader2d is the planar shape's twin and reads the terrain's virtual texture,
	// so only Shader3d-only shapes draw. Settle: the twins' programs translated.
	for (const std::string &name : out.paired) {
		out.standalone.erase(name);
	}
	return out;
}

TerrainSurfaceDecalStats add_terrain_surface_decals(const core::res::TerrainDecals &p_decals,
		std::span<const SceneWaterShader> p_shaders, const LookEnvState &p_look, TerrainLook &r_look,
		PreparedMesh &r_mesh) {
	TerrainSurfaceDecalStats stats;
	const core::res::TerrainDecalSet &set = p_decals.surface;
	std::map<std::string, std::vector<size_t>> groups;
	for (size_t i = 0; i < set.records.size(); ++i) {
		const std::string name = to_lower_ascii(set.records[i].shader);
		const bool planned = std::any_of(
				p_shaders.begin(), p_shaders.end(), [&](const SceneWaterShader &s) { return s.shader == name; });
		if (planned) {
			groups[name].push_back(i);
			++stats.records;
		}
	}
	stats.shaders = uint32_t(groups.size());
	for (const auto &[name, records] : groups) {
		const SceneWaterShader &shader = *std::find_if(
				p_shaders.begin(), p_shaders.end(), [&](const SceneWaterShader &s) { return s.shader == name; });
		std::string refused;
		std::optional<TerrainMaterial> material;
		// STAND-IN: the first solution that translates stands for the one the engine picks. Settle: the solution choice read.
		for (size_t i = 0; i < shader.solutions.size() && !material; ++i) {
			SolutionBinding binding;
			binding.height_uv = r_look.height_uv;
			binding.height_pixel = r_look.height_pixel;
			binding.target = ProgramTarget::DeferredDecal;
			binding.view_pull = kTerrainDecalViewPull;
			core::Result<TerrainMaterial> m = terrain_solution_material(shader, shader.solutions[i], p_look, binding);
			if (m) {
				material = std::move(*m);
			} else {
				refused += (refused.empty() ? "" : "; ") + std::string("solution ") + std::to_string(i) + ": " +
						m.error().message;
			}
		}
		if (!material) {
			stats.undrawn.emplace_back(name, refused.empty() ? std::string("no solution") : refused);
			continue;
		}
		PreparedSurface surface;
		surface.material_name = name;
		surface.material_index = uint32_t(r_look.materials.size());
		uint32_t drawn = 0;
		for (const size_t index : records) {
			const core::res::TerrainDecalRecord &r = set.records[index];
			if (uint64_t(r.first_index) + uint64_t(r.triangle_count) * 3 > set.indices.size()) {
				continue;
			}
			bool inside = true;
			for (uint32_t k = 0; k < r.triangle_count * 3; ++k) {
				inside = inside && set.indices[r.first_index + k] < set.vertices.size();
			}
			if (!inside) {
				continue;
			}
			for (uint32_t tri = 0; tri < r.triangle_count; ++tri) {
				std::array<const core::res::TerrainDecalVertex *, 3> t;
				for (uint32_t k = 0; k < 3; ++k) {
					t[k] = &set.vertices[set.indices[r.first_index + tri * 3 + k]];
				}
				const core::Vec3 &a = t[0]->position, &b = t[1]->position, &c = t[2]->position;
				const core::Vec3 e1{ b[0] - a[0], b[1] - a[1], b[2] - a[2] };
				const core::Vec3 e2{ c[0] - a[0], c[1] - a[1], c[2] - a[2] };
				core::Vec3 n{ e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
					e1[0] * e2[1] - e1[1] * e2[0] };
				if (n[1] > 0.0f) {
					std::swap(t[1], t[2]);
				} else {
					n = { -n[0], -n[1], -n[2] };
				}
				const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				n = len > 0.0f ? core::Vec3{ n[0] / len, n[1] / len, n[2] / len } : core::Vec3{ 0.0f, 1.0f, 0.0f };
				for (const core::res::TerrainDecalVertex *v : t) {
					surface.indices.push_back(int32_t(surface.positions.size() / 3));
					surface.positions.insert(
							surface.positions.end(), { v->position[0], v->position[1], v->position[2] });
					surface.normals.insert(surface.normals.end(), { n[0], n[1], n[2] });
					surface.tangents.insert(
							surface.tangents.end(), { v->tangent[0], v->tangent[1], v->tangent[2], 1.0f });
					surface.uv0.insert(surface.uv0.end(), { v->uv[0], v->uv[1] });
					surface.uv1.insert(surface.uv1.end(), { v->position[0], v->position[2] });
					for (size_t m = 0; m < 4; ++m) {
						surface.colors.push_back(uint8_t(std::lround(std::clamp(v->mask[m], 0.0f, 1.0f) * 255.0f)));
					}
				}
			}
			drawn += r.triangle_count > 0 ? 1u : 0u;
		}
		if (surface.indices.empty()) {
			stats.undrawn.emplace_back(name, "no triangle");
			continue;
		}
		stats.drawn_records += drawn;
		stats.triangles += surface.indices.size() / 3;
		++stats.drawn_shaders;
		++stats.surfaces;
		r_mesh.vertices += surface.vertex_count();
		r_mesh.triangles += surface.indices.size() / 3;
		r_look.materials.push_back(std::move(*material));
		r_mesh.surfaces.push_back(std::move(surface));
	}
	return stats;
}

}
