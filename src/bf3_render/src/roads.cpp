#include "bf3/render/roads.h"

#include "bf3/core/superbundle_mount.h"
#include "bf3/core/util/text.h"
#include "bf3/render/dxbc.h"
#include "bf3/render/shader_database.h"
#include "bf3/render/terrain_program.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace bf3::render {

namespace {

constexpr double kMinArea = 1e-9;

using core::to_lower_ascii;

struct Point {
	double u = 0.0;
	double w = 0.0;
};

double cross(const Point &p_a, const Point &p_b, const Point &p_c) {
	return (p_b.u - p_a.u) * (p_c.w - p_a.w) - (p_b.w - p_a.w) * (p_c.u - p_a.u);
}

template <class Inside, class Cut>
void clip(std::vector<Point> &r_polygon, std::vector<Point> &r_scratch, Inside p_inside, Cut p_cut) {
	r_scratch.clear();
	const size_t n = r_polygon.size();
	for (size_t i = 0; i < n; ++i) {
		const Point &a = r_polygon[i];
		const Point &b = r_polygon[(i + 1) % n];
		const double da = p_inside(a);
		const double db = p_inside(b);
		if (da >= 0.0) {
			r_scratch.push_back(a);
		}
		if ((da >= 0.0) != (db >= 0.0)) {
			const double t = da / (da - db);
			r_scratch.push_back(p_cut(Point{ a.u + (b.u - a.u) * t, a.w + (b.w - a.w) * t }));
		}
	}
	r_polygon.swap(r_scratch);
}

}

std::string terrain_decals_name(std::string_view p_asset) {
	return to_lower_ascii(p_asset) + ".decals";
}

std::vector<SceneDecalShader> plan_terrain_decals(const core::LevelMount &p_mount, const ShaderLookup &p_shaders,
		std::string_view p_asset, const std::function<int32_t(const std::string &)> &p_texture,
		std::vector<std::pair<std::string, std::string>> &r_failures) {
	std::vector<SceneDecalShader> out;
	const std::string name = terrain_decals_name(p_asset);
	core::Result<std::pair<core::ResInfo, std::vector<uint8_t>>> res = p_mount.read_res(name);
	if (!res) {
		return out;
	}
	core::res::TerrainDecals decals;
	const core::res::ResError parsed = core::res::TerrainDecals::parse(res->second, decals);
	if (!parsed.ok()) {
		r_failures.emplace_back("terrain decals " + name, parsed.message());
		return out;
	}
	std::vector<std::string> names;
	for (const core::res::TerrainDecalRecord &r : decals.planar.records) {
		names.push_back(to_lower_ascii(r.shader));
	}
	std::sort(names.begin(), names.end());
	names.erase(std::unique(names.begin(), names.end()), names.end());
	std::map<std::string, bool> srgb;
	for (const std::string &shader_name : names) {
		const SurfaceShader *shader = p_shaders.by_name(shader_name);
		if (shader == nullptr) {
			r_failures.emplace_back("terrain decal shader " + shader_name, "not in the shader database");
			continue;
		}
		SceneDecalShader decal;
		decal.shader = shader_name;
		const std::vector<std::pair<uint32_t, std::string>> *slots = nullptr;
		for (const ShaderSolution &s : shader->solutions) {
			if (s.pixel_program) {
				decal.program = s.pixel_program;
				slots = s.pixel_constants != nullptr ? &s.pixel_constants->textures : nullptr;
				break;
			}
		}
		if (decal.program == nullptr) {
			r_failures.emplace_back("terrain decal shader " + shader_name, "no pixel program");
			continue;
		}
		const std::vector<std::pair<uint32_t, std::string>> &bound = slots != nullptr ? *slots : shader->texture_slots;
		for (size_t a = 0; a < bound.size() && decal.refused.empty(); ++a) {
			for (size_t b = a + 1; b < bound.size(); ++b) {
				if (to_lower_ascii(bound[a].second) == to_lower_ascii(bound[b].second)) {
					decal.refused = "t" + std::to_string(bound[a].first) + " and t" + std::to_string(bound[b].first) +
							" sample " + to_lower_ascii(bound[a].second) +
							" through two samplers whose address modes are not decoded";
					break;
				}
			}
		}
		if (decal.refused.empty()) {
			for (const auto &[slot, texture] : bound) {
				decal.textures.push_back(plan_terrain_texture(p_mount, slot, texture, p_texture, srgb, r_failures));
			}
		}
		out.push_back(std::move(decal));
	}
	return out;
}

DrapeCells drape_cells(const core::res::HeightGrid &p_grid, const std::optional<std::array<float, 4>> &p_crop_xz) {
	DrapeCells cells;
	if (!p_crop_xz || p_grid.width < 2 || p_grid.height < 2) {
		return cells;
	}
	const auto index = [](float p_v, float p_o, float p_s, size_t p_n, bool p_up) {
		const double f = (double(p_v) - double(p_o)) / double(p_s);
		const double r = p_up ? std::ceil(f) : std::floor(f);
		if (!(r > 0.0)) {
			return size_t(0);
		}
		return r >= double(p_n - 1) ? p_n - 1 : size_t(r);
	};
	cells.x0 = index((*p_crop_xz)[0], p_grid.origin_xz[0], p_grid.sample_spacing[0], p_grid.width, false);
	cells.z0 = index((*p_crop_xz)[1], p_grid.origin_xz[1], p_grid.sample_spacing[1], p_grid.height, false);
	cells.x1 = index((*p_crop_xz)[2], p_grid.origin_xz[0], p_grid.sample_spacing[0], p_grid.width, true);
	cells.z1 = index((*p_crop_xz)[3], p_grid.origin_xz[1], p_grid.sample_spacing[1], p_grid.height, true);
	return cells;
}

double drape_triangle(const core::res::HeightGrid &p_grid, const std::array<DrapeVertex, 3> &p_triangle,
		std::vector<DrapedVertex> &r_triangles, const DrapeCells &p_cells) {
	if (p_grid.width < 2 || p_grid.height < 2 || !(p_grid.sample_spacing[0] > 0.0f) ||
			!(p_grid.sample_spacing[1] > 0.0f)) {
		return 0.0;
	}
	const double ox = p_grid.origin_xz[0], oz = p_grid.origin_xz[1];
	const double sx = p_grid.sample_spacing[0], sz = p_grid.sample_spacing[1];
	std::array<Point, 3> t;
	for (size_t i = 0; i < 3; ++i) {
		t[i] = { (double(p_triangle[i].x) - ox) / sx, (double(p_triangle[i].z) - oz) / sz };
		if (!std::isfinite(t[i].u) || !std::isfinite(t[i].w)) {
			return 0.0;
		}
	}
	const double whole = cross(t[0], t[1], t[2]);
	if (std::fabs(whole) < kMinArea) {
		return 0.0;
	}
	const double lo_u = std::min({ t[0].u, t[1].u, t[2].u }), hi_u = std::max({ t[0].u, t[1].u, t[2].u });
	const double lo_w = std::min({ t[0].w, t[1].w, t[2].w }), hi_w = std::max({ t[0].w, t[1].w, t[2].w });
	const double max_u = double(p_grid.width - 1), max_w = double(p_grid.height - 1);
	if (hi_u <= 0.0 || hi_w <= 0.0 || lo_u >= max_u || lo_w >= max_w) {
		return 0.0;
	}
	const size_t x0 = std::max(p_cells.x0, size_t(std::clamp(std::floor(lo_u), 0.0, max_u - 1.0)));
	const size_t x1 = size_t(std::clamp(std::ceil(hi_u) - 1.0, 0.0, max_u - 1.0));
	const size_t z0 = std::max(p_cells.z0, size_t(std::clamp(std::floor(lo_w), 0.0, max_w - 1.0)));
	const size_t z1 = size_t(std::clamp(std::ceil(hi_w) - 1.0, 0.0, max_w - 1.0));
	std::vector<Point> polygon, scratch;
	double area = 0.0;
	for (size_t z = z0; z <= z1 && z < p_cells.z1; ++z) {
		for (size_t x = x0; x <= x1 && x < p_cells.x1; ++x) {
			if (p_grid.hole(x, z) || p_grid.hole(x + 1, z) || p_grid.hole(x, z + 1) || p_grid.hole(x + 1, z + 1)) {
				continue;
			}
			const double cu = double(x), cw = double(z);
			const double ha = p_grid.at(x, z), hb = p_grid.at(x + 1, z), hc = p_grid.at(x, z + 1),
						 hd = p_grid.at(x + 1, z + 1);
			for (int half = 0; half < 2; ++half) {
				polygon.assign(t.begin(), t.end());
				if (half == 0) {
					clip(
							polygon, scratch, [&](const Point &p) { return p.u - cu; },
							[&](Point p) {
								p.u = cu;
								return p;
							});
					clip(
							polygon, scratch, [&](const Point &p) { return p.w - cw; },
							[&](Point p) {
								p.w = cw;
								return p;
							});
					clip(
							polygon, scratch, [&](const Point &p) { return 1.0 - (p.u - cu) - (p.w - cw); },
							[](Point p) { return p; });
				} else {
					clip(
							polygon, scratch, [&](const Point &p) { return cu + 1.0 - p.u; },
							[&](Point p) {
								p.u = cu + 1.0;
								return p;
							});
					clip(
							polygon, scratch, [&](const Point &p) { return cw + 1.0 - p.w; },
							[&](Point p) {
								p.w = cw + 1.0;
								return p;
							});
					clip(
							polygon, scratch, [&](const Point &p) { return (p.u - cu) + (p.w - cw) - 1.0; },
							[](Point p) { return p; });
				}
				if (polygon.size() < 3) {
					continue;
				}
				double twice = 0.0;
				for (size_t i = 1; i + 1 < polygon.size(); ++i) {
					twice += cross(polygon[0], polygon[i], polygon[i + 1]);
				}
				if (std::fabs(twice) < kMinArea) {
					continue;
				}
				area += std::fabs(twice) * 0.5 * sx * sz;
				const auto emit = [&](const Point &p) {
					const double b1 = cross(t[0], p, t[2]) / whole;
					const double b2 = cross(t[0], t[1], p) / whole;
					const double b0 = 1.0 - b1 - b2;
					const double lu = p.u - cu, lw = p.w - cw;
					const double h = half == 0 ? ha + (hb - ha) * lu + (hc - ha) * lw
											   : hd + (hc - hd) * (1.0 - lu) + (hb - hd) * (1.0 - lw);
					DrapedVertex v;
					v.x = float(ox + p.u * sx);
					v.y = float(h);
					v.z = float(oz + p.w * sz);
					for (size_t k = 0; k < 2; ++k) {
						v.uv[k] = float(b0 * p_triangle[0].uv[k] + b1 * p_triangle[1].uv[k] + b2 * p_triangle[2].uv[k]);
					}
					for (size_t k = 0; k < 4; ++k) {
						v.mask[k] = float(
								b0 * p_triangle[0].mask[k] + b1 * p_triangle[1].mask[k] + b2 * p_triangle[2].mask[k]);
					}
					r_triangles.push_back(v);
				};
				for (size_t i = 1; i + 1 < polygon.size(); ++i) {
					if (std::fabs(cross(polygon[0], polygon[i], polygon[i + 1])) < kMinArea) {
						continue;
					}
					emit(polygon[0]);
					if (twice > 0.0) {
						emit(polygon[i]);
						emit(polygon[i + 1]);
					} else {
						emit(polygon[i + 1]);
						emit(polygon[i]);
					}
				}
			}
		}
	}
	return area;
}

core::Result<std::string> terrain_decal_shader_code(
		const PixelTranslation &p_translation, const TerrainMaterial &p_material) {
	if (p_translation.outputs < 3) {
		return core::make_error("terrain decal writes " + std::to_string(p_translation.outputs) + " targets, not 3");
	}
	for (uint32_t input : p_translation.inputs) {
		if (input < 1 || input > 3) {
			return core::make_error("terrain decal reads input v" + std::to_string(input));
		}
	}
	core::Result<std::string> head = terrain_shader_head(p_translation, p_material, "terrain decal",
			"blend_mix, depth_draw_never, cull_disabled, skip_vertex_transform");
	if (!head) {
		return head.error();
	}
	std::string code = std::move(*head);
	code += "void vertex() {\nVERTEX = (MODELVIEW_MATRIX * vec4(VERTEX, 1.0)).xyz * " +
			std::to_string(1.0f - kTerrainDecalViewPull) +
			";\nNORMAL = normalize((MODELVIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);\n"
			"TANGENT = normalize((MODELVIEW_MATRIX * vec4(TANGENT, 0.0)).xyz);\n"
			"BINORMAL = normalize((MODELVIEW_MATRIX * vec4(BINORMAL, 0.0)).xyz);\n}\n";
	code += "void fragment() {\nvec4 v1 = COLOR;\nvec4 v2 = vec4(UV2.x, UV2.y, 0.9, 0.0);\nvec4 v3 = vec4(UV.x, UV.y, "
			"0.0, 0.0);\n";
	code += terrain_shader_locals(p_translation, p_material);
	code += p_translation.body;
	code += "float decal_cover = max(o0.w, 0.0001);\n"
			"vec3 decal_colour = vec3(o0.y, o0.z, o2.x) / decal_cover;\n"
			"ALBEDO = decal_colour * decal_colour;\n"
			"vec3 decal_normal = normalize(vec3(o0.x, o1.z, o1.x) / decal_cover * 2.0 - 1.0);\n"
			"NORMAL = normalize((VIEW_MATRIX * (MODEL_MATRIX * vec4(decal_normal, 0.0))).xyz);\n"
			"ROUGHNESS = 1.0;\n"
			"ALPHA = clamp(o0.w, 0.0, 1.0);\n}\n";
	return code;
}

core::Result<TerrainMaterial> terrain_decal_material(
		const SceneDecalShader &p_shader, const TerrainLook &p_look, int32_t p_priority) {
	const std::string where = "terrain decal shader " + p_shader.shader;
	if (p_shader.program == nullptr) {
		return core::make_error(where + ": no pixel program");
	}
	if (!p_shader.refused.empty()) {
		return core::make_error(p_shader.refused);
	}
	core::Result<dxbc::Program> program = dxbc::parse(*p_shader.program);
	if (!program) {
		return core::error_at(where, program.error());
	}
	core::Result<PixelTranslation> translation = translate_pixel_program(*program);
	if (!translation) {
		return core::error_at(where, translation.error());
	}
	TerrainMaterial material;
	material.render_priority = p_priority;
	const core::Status bound = bind_terrain_program(
			where, *program, *translation, p_shader.textures, p_look.height_uv, p_look.height_pixel, {}, material);
	if (!bound) {
		return bound.error();
	}
	core::Result<std::string> code = terrain_decal_shader_code(*translation, material);
	if (!code) {
		return core::error_at(where, code.error());
	}
	material.code = std::move(*code);
	return material;
}

TerrainDecalStats add_terrain_decals(const core::res::TerrainDecals &p_decals, const core::res::HeightGrid &p_grid,
		const core::level::LevelTransform &p_transform, const std::optional<std::array<float, 4>> &p_crop_xz,
		std::span<const SceneDecalShader> p_shaders, TerrainLook &r_look, PreparedMesh &r_mesh) {
	using core::Vec3;
	TerrainDecalStats stats;
	const core::res::TerrainDecalSet &set = p_decals.planar;
	stats.records = uint32_t(set.records.size());
	const bool axis_aligned = p_transform.right == Vec3{ 1.0f, 0.0f, 0.0f } &&
			p_transform.up == Vec3{ 0.0f, 1.0f, 0.0f } && p_transform.forward == Vec3{ 0.0f, 0.0f, 1.0f } &&
			p_transform.scale > 0.0f;
	std::map<std::pair<int32_t, std::string>, std::vector<size_t>> groups;
	std::map<std::string, bool> seen;
	const double metres = double(p_transform.scale) * double(p_transform.scale);
	std::vector<double> record_area(set.records.size(), 0.0);
	for (size_t i = 0; i < set.records.size(); ++i) {
		const core::res::TerrainDecalRecord &r = set.records[i];
		const std::string name = to_lower_ascii(r.shader);
		groups[{ r.draw_order(), name }].push_back(i);
		seen.emplace(name, false);
		stats.data_triangles += r.triangle_count;
		double area = 0.0;
		for (uint32_t tri = 0; tri < r.triangle_count; ++tri) {
			const core::res::TerrainDecalVertex &a = set.vertices[set.indices[r.first_index + tri * 3]];
			const core::res::TerrainDecalVertex &b = set.vertices[set.indices[r.first_index + tri * 3 + 1]];
			const core::res::TerrainDecalVertex &c = set.vertices[set.indices[r.first_index + tri * 3 + 2]];
			area += std::fabs(double(b.position[0] - a.position[0]) * double(c.position[2] - a.position[2]) -
							double(b.position[2] - a.position[2]) * double(c.position[0] - a.position[0])) *
					0.5;
		}
		record_area[i] = area;
		stats.data_area += area;
	}
	stats.shaders = uint32_t(seen.size());
	if (!axis_aligned) {
		for (const auto &[name, drawn] : seen) {
			stats.undrawn.emplace_back(name, "the terrain transform is not axis aligned");
		}
		return stats;
	}
	std::vector<int32_t> orders;
	for (const auto &[key, records] : groups) {
		if (orders.empty() || orders.back() != key.first) {
			orders.push_back(key.first);
		}
	}
	const auto vertex = [&](uint32_t p_index) {
		const core::res::TerrainDecalVertex &v = set.vertices[p_index];
		DrapeVertex d;
		d.x = (v.position[0] - p_transform.trans[0]) / p_transform.scale;
		d.z = (v.position[2] - p_transform.trans[2]) / p_transform.scale;
		d.uv = v.uv;
		d.mask = v.mask;
		return d;
	};
	const DrapeCells cells = drape_cells(p_grid, p_crop_xz);
	std::map<std::string, std::string> refused;
	std::vector<DrapedVertex> draped;
	uint64_t total = 0;
	for (const auto &[key, records] : groups) {
		const auto &[order, name] = key;
		const auto shader = std::find_if(
				p_shaders.begin(), p_shaders.end(), [&](const SceneDecalShader &s) { return s.shader == name; });
		if (shader == p_shaders.end()) {
			refused.emplace(name, "no planned shader");
			continue;
		}
		const size_t rank = size_t(std::find(orders.begin(), orders.end(), order) - orders.begin());
		const int32_t priority = std::min(kTerrainDecalLastPriority, kTerrainDecalFirstPriority + int32_t(rank));
		core::Result<TerrainMaterial> material = terrain_decal_material(*shader, r_look, priority);
		if (!material) {
			refused.emplace(name, material.error().message);
			stats.refused_records += uint32_t(records.size());
			for (const size_t index : records) {
				stats.refused_area += record_area[index];
			}
			continue;
		}
		draped.clear();
		double drawn_area = 0.0;
		uint32_t drawn_records = 0;
		for (const size_t index : records) {
			const core::res::TerrainDecalRecord &r = set.records[index];
			const size_t before = draped.size();
			for (uint32_t tri = 0; tri < r.triangle_count; ++tri) {
				const std::array<DrapeVertex, 3> t = { vertex(set.indices[r.first_index + tri * 3]),
					vertex(set.indices[r.first_index + tri * 3 + 1]),
					vertex(set.indices[r.first_index + tri * 3 + 2]) };
				if (total + draped.size() / 3 < kMaxTerrainDecalTriangles) {
					drawn_area += drape_triangle(p_grid, t, draped, cells);
				}
			}
			drawn_records += draped.size() > before ? 1u : 0u;
		}
		if (draped.empty()) {
			refused.emplace(name, "no triangle lies on the terrain");
			continue;
		}
		PreparedSurface surface;
		surface.material_name = name;
		surface.material_index = uint32_t(r_look.materials.size());
		const bool mirror = p_transform.determinant() < 0.0f;
		surface.positions.reserve(draped.size() * 3);
		surface.normals.reserve(draped.size() * 3);
		surface.uv0.reserve(draped.size() * 2);
		surface.uv1.reserve(draped.size() * 2);
		surface.colors.reserve(draped.size() * 4);
		surface.indices.reserve(draped.size());
		for (size_t t = 0; t + 2 < draped.size(); t += 3) {
			const DrapedVertex &a = draped[t], &b = draped[t + 1], &c = draped[t + 2];
			const Vec3 e1{ b.x - a.x, b.y - a.y, b.z - a.z };
			const Vec3 e2{ c.x - a.x, c.y - a.y, c.z - a.z };
			Vec3 n{ e1[2] * e2[1] - e1[1] * e2[2], e1[0] * e2[2] - e1[2] * e2[0], e1[1] * e2[0] - e1[0] * e2[1] };
			const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			n = len > 0.0f ? Vec3{ n[0] / len, n[1] / len, n[2] / len } : Vec3{ 0.0f, 1.0f, 0.0f };
			const Vec3 wn = p_transform.rotate(n);
			for (size_t k = 0; k < 3; ++k) {
				const DrapedVertex &v = draped[t + (mirror && k > 0 ? 3 - k : k)];
				const Vec3 p = p_transform.transform_point({ v.x, v.y, v.z });
				surface.indices.push_back(int32_t(surface.positions.size() / 3));
				surface.positions.insert(surface.positions.end(), { p[0], p[1], p[2] });
				surface.normals.insert(surface.normals.end(), { wn[0], wn[1], wn[2] });
				surface.uv0.insert(surface.uv0.end(), { v.uv[0], v.uv[1] });
				surface.uv1.insert(surface.uv1.end(), { v.x, v.z });
				for (size_t m = 0; m < 4; ++m) {
					surface.colors.push_back(uint8_t(std::lround(std::clamp(v.mask[m], 0.0f, 1.0f) * 255.0f)));
				}
			}
		}
		total += draped.size() / 3;
		stats.drawn_triangles += draped.size() / 3;
		stats.drawn_area += drawn_area * metres;
		stats.drawn_records += drawn_records;
		++stats.surfaces;
		seen[name] = true;
		r_mesh.vertices += surface.vertex_count();
		r_mesh.triangles += surface.indices.size() / 3;
		r_look.materials.push_back(std::move(*material));
		r_mesh.surfaces.push_back(std::move(surface));
	}
	for (const auto &[name, drawn] : seen) {
		stats.drawn_shaders += drawn ? 1u : 0u;
		if (!drawn) {
			const auto why = refused.find(name);
			stats.undrawn.emplace_back(name, why == refused.end() ? std::string("not drawn") : why->second);
		}
	}
	return stats;
}

}
