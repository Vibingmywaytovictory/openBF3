#include "bf3/render/water.h"

#include "bf3/core/superbundle_mount.h"
#include "bf3/core/util/text.h"
#include "bf3/render/dxbc.h"
#include "bf3/render/roads.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace bf3::render {

namespace {

using core::to_lower_ascii;

constexpr std::string_view kEnvmapBinding = "texture_outdoorLightSkyEnvmap";
constexpr std::string_view kHeightBinding = "texture_heightfieldAtlas";
constexpr std::string_view kHeightScale = "heightmapWorldSizeY";
constexpr std::string_view kHeightUv = "worldToHeightfieldUvTransform";
constexpr std::string_view kHeightPixel = "heightfieldPixelSizeAndAspect";

}

core::Result<TerrainMaterial> terrain_solution_material(const SceneWaterShader &p_shader,
		const ShaderSolution &p_solution, const LookEnvState &p_look, const SolutionBinding &p_binding) {
	const SceneTerrainTexture &p_envmap = p_binding.envmap;
	const std::array<float, 4> &p_height_uv = p_binding.height_uv;
	if (p_solution.pixel_program == nullptr || p_solution.vertex_program == nullptr) {
		return core::make_error("no pixel and vertex program");
	}
	core::Result<dxbc::Program> pixel = dxbc::parse(*p_solution.pixel_program);
	if (!pixel) {
		return pixel.error();
	}
	core::Result<dxbc::Program> vertex = dxbc::parse(p_solution.vertex_program->bytes);
	if (!vertex) {
		return vertex.error();
	}
	std::vector<TerrainTextureBinding> bindings;
	for (const dxbc::Binding &b : pixel->bindings) {
		if (b.type != uint32_t(dxbc::BindingType::Texture)) {
			continue;
		}
		TerrainTextureBinding binding;
		binding.slot = b.bind_point;
		if (b.name == kEnvmapBinding) {
			binding.texture = p_envmap.texture;
			binding.srgb = p_envmap.srgb;
		} else if (b.name == kHeightBinding) {
			binding.image = kTerrainHeightImage;
		} else {
			const auto data = std::find_if(p_shader.textures.begin(), p_shader.textures.end(),
					[&](const SceneTerrainTexture &t) { return t.slot == b.bind_point; });
			if (data == p_shader.textures.end()) {
				continue;
			}
			binding.texture = data->texture;
			binding.srgb = data->srgb;
		}
		if (binding.texture >= 0 || binding.image >= 0) {
			bindings.push_back(binding);
		}
	}
	std::vector<uint32_t> registers;
	for (const TerrainTextureBinding &b : bindings) {
		registers.push_back(b.slot);
	}
	ProgramRequest request;
	request.pixel = &*pixel;
	request.vertex = &*vertex;
	request.vertex_formats = p_solution.vertex_program->element_formats;
	request.vertex_lanes = p_solution.vertex_program->element_lanes;
	request.bound_textures = registers;
	request.target = p_binding.target;
	request.blend = p_binding.blend;
	request.view_pull = p_binding.view_pull;
	request.double_sided = (p_solution.flags & 1u) != 0;
	request.srgb = [&](uint32_t p_reg) {
		const auto b = std::find_if(
				bindings.begin(), bindings.end(), [&](const TerrainTextureBinding &t) { return t.slot == p_reg; });
		return b != bindings.end() && b->srgb;
	};
	request.engine_globals = kWaterGlobals;
	request.page_hidden = true;
	core::Result<TranslatedProgram> translated = translate_program(request);
	if (!translated) {
		return translated.error();
	}
	TerrainMaterial material;
	material.code = std::move(translated->code);
	material.render_priority = p_binding.render_priority;
	for (uint32_t reg : translated->textures) {
		const auto b = std::find_if(
				bindings.begin(), bindings.end(), [&](const TerrainTextureBinding &t) { return t.slot == reg; });
		material.textures.push_back(*b);
	}
	for (const ProgramConstant &c : translated->constants) {
		std::array<float, 4> value{};
		for (size_t lane = 0; lane < 4; ++lane) {
			const ProgramConstantLane &l = c.lanes[lane];
			if (l.value.empty()) {
				continue;
			}
			if (l.engine) {
				const std::optional<std::array<float, 4>> engine = look_engine_global(p_look, l.value);
				if (!engine) {
					return core::make_error("engine constant " + l.value + " without a value");
				}
				value[lane] = (*engine)[l.component & 3u];
			} else if (l.value == kHeightScale && l.component == 0) {
				// WHY: the height image holds metres, so heightmapWorldSizeY is 1.
				value[lane] = 1.0f;
			} else if (l.value == kHeightUv) {
				value[lane] = p_height_uv[l.component & 3u];
			} else if (l.value == kHeightPixel) {
				value[lane] = p_binding.height_pixel[l.component & 3u];
			} else {
				return core::make_error("unknown constant " + l.value);
			}
		}
		material.uniforms.emplace_back(c.uniform, value);
	}
	return material;
}

std::vector<SceneWaterShader> plan_terrain_water(const core::LevelMount &p_mount, const ShaderLookup &p_shaders,
		std::string_view p_asset, const std::function<int32_t(const std::string &)> &p_texture,
		std::vector<std::pair<std::string, std::string>> &r_failures) {
	return plan_terrain_record_shaders(
			p_mount, p_shaders, p_asset, TerrainDecalGroup::Water, {}, p_texture, r_failures);
}

std::vector<SceneWaterShader> plan_terrain_record_shaders(const core::LevelMount &p_mount,
		const ShaderLookup &p_shaders, std::string_view p_asset, TerrainDecalGroup p_group,
		const std::function<bool(const std::string &)> &p_keep,
		const std::function<int32_t(const std::string &)> &p_texture,
		std::vector<std::pair<std::string, std::string>> &r_failures) {
	std::vector<SceneWaterShader> out;
	const std::string name = terrain_decals_name(p_asset);
	core::Result<std::pair<core::ResInfo, std::vector<uint8_t>>> res = p_mount.read_res(name);
	if (!res) {
		return out;
	}
	core::res::TerrainDecals decals;
	if (!core::res::TerrainDecals::parse(res->second, decals).ok()) {
		return out;
	}
	const core::res::TerrainDecalSet &set = p_group == TerrainDecalGroup::Water ? decals.water : decals.surface;
	std::vector<std::string> names;
	for (const core::res::TerrainDecalRecord &r : set.records) {
		const std::string lower = to_lower_ascii(r.shader);
		if (!p_keep || p_keep(lower)) {
			names.push_back(lower);
		}
	}
	std::sort(names.begin(), names.end());
	names.erase(std::unique(names.begin(), names.end()), names.end());
	std::map<std::string, bool> srgb;
	for (const std::string &shader_name : names) {
		const SurfaceShader *shader = p_shaders.by_name(shader_name);
		if (shader == nullptr) {
			r_failures.emplace_back(
					(p_group == TerrainDecalGroup::Water ? "terrain water shader " : "terrain surface decal shader ") +
							shader_name,
					"not in the shader database");
			continue;
		}
		SceneWaterShader water;
		water.shader = shader_name;
		water.solutions = shader->solutions;
		for (const ShaderSolution &s : shader->solutions) {
			if (s.pixel_constants == nullptr) {
				continue;
			}
			for (const auto &[slot, texture] : s.pixel_constants->textures) {
				const bool planned = std::any_of(water.textures.begin(), water.textures.end(),
						[&](const SceneTerrainTexture &t) { return t.slot == slot; });
				if (!planned) {
					water.textures.push_back(plan_terrain_texture(p_mount, slot, texture, p_texture, srgb, r_failures));
				}
			}
		}
		out.push_back(std::move(water));
	}
	return out;
}

core::Result<TerrainMaterial> terrain_water_material(const SceneWaterShader &p_shader, const LookEnvState &p_look,
		const SceneTerrainTexture &p_envmap, const std::array<float, 4> &p_height_uv) {
	std::string refused;
	SolutionBinding binding;
	binding.envmap = p_envmap;
	binding.height_uv = p_height_uv;
	// INFERRED: water blends premultiplied, as its programs write colour times alpha whatever the solution's blend byte.
	// Settle: the water solutions' blend states read.
	binding.blend = ProgramBlend::Premultiplied;
	// STAND-IN: the first solution that translates stands for the one the engine picks. Settle: the solution choice read.
	for (size_t i = 0; i < p_shader.solutions.size(); ++i) {
		core::Result<TerrainMaterial> material =
				terrain_solution_material(p_shader, p_shader.solutions[i], p_look, binding);
		if (material) {
			return material;
		}
		refused += (refused.empty() ? "" : "; ") + std::string("solution ") + std::to_string(i) + ": " +
				material.error().message;
	}
	return core::make_error("terrain water shader " + p_shader.shader + ": " +
			(refused.empty() ? std::string("no solution") : refused));
}

TerrainWaterStats add_terrain_water(const core::res::TerrainDecals &p_decals,
		std::span<const SceneWaterShader> p_shaders, const LookEnvState &p_look, const SceneTerrainTexture &p_envmap,
		TerrainLook &r_look, PreparedMesh &r_mesh) {
	TerrainWaterStats stats;
	const core::res::TerrainDecalSet &set = p_decals.water;
	stats.records = uint32_t(set.records.size());
	std::map<std::string, std::vector<size_t>> groups;
	for (size_t i = 0; i < set.records.size(); ++i) {
		groups[to_lower_ascii(set.records[i].shader)].push_back(i);
	}
	stats.shaders = uint32_t(groups.size());
	for (const auto &[name, records] : groups) {
		const auto shader = std::find_if(
				p_shaders.begin(), p_shaders.end(), [&](const SceneWaterShader &s) { return s.shader == name; });
		if (shader == p_shaders.end()) {
			stats.undrawn.emplace_back(name, "no planned shader");
			continue;
		}
		core::Result<TerrainMaterial> material = terrain_water_material(*shader, p_look, p_envmap, r_look.height_uv);
		if (!material) {
			stats.undrawn.emplace_back(name, material.error().message);
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
				const core::Vec3 n{ e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
					e1[0] * e2[1] - e1[1] * e2[0] };
				if (n[0] * t[0]->normal[0] + n[1] * t[0]->normal[1] + n[2] * t[0]->normal[2] > 0.0f) {
					std::swap(t[1], t[2]);
				}
				for (const core::res::TerrainDecalVertex *v : t) {
					surface.indices.push_back(int32_t(surface.positions.size() / 3));
					surface.positions.insert(
							surface.positions.end(), { v->position[0], v->position[1], v->position[2] });
					surface.normals.insert(surface.normals.end(), { v->normal[0], v->normal[1], v->normal[2] });
					surface.tangents.insert(
							surface.tangents.end(), { v->tangent[0], v->tangent[1], v->tangent[2], 1.0f });
					surface.uv0.insert(surface.uv0.end(), { v->uv[0], v->uv[1] });
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
