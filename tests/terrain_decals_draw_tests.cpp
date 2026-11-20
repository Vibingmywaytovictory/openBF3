#include "bf3/core/level/census.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "bf3/core/res/terrain_decals.h"
#include "bf3/core/res/terrain_mask_field.h"
#include "bf3/core/util/text.h"
#include "bf3/render/level_scene.h"
#include "bf3/render/light_pass.h"
#include "bf3/render/roads.h"
#include "bf3/render/terrain_page.h"
#include "bf3/render/terrain_program.h"
#include "dxbc_test_support.h"
#include "terrain_test_support.h"
#include "test_support.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <vector>

using namespace bf3;
using namespace bf3::core;

namespace {

struct DrawnDecals {
	std::vector<std::array<float, 6>> triangles;
	std::map<std::pair<int32_t, int32_t>, std::vector<uint32_t>> cells;

	static constexpr float kCell = 16.0f;

	void add(const render::PreparedSurface &p_surface) {
		for (size_t i = 0; i + 2 < p_surface.indices.size(); i += 3) {
			std::array<float, 6> t{};
			for (size_t k = 0; k < 3; ++k) {
				const size_t v = size_t(p_surface.indices[i + k]) * 3;
				t[k * 2] = p_surface.positions[v];
				t[k * 2 + 1] = p_surface.positions[v + 2];
			}
			const float lo_x = std::min({ t[0], t[2], t[4] }), hi_x = std::max({ t[0], t[2], t[4] });
			const float lo_z = std::min({ t[1], t[3], t[5] }), hi_z = std::max({ t[1], t[3], t[5] });
			for (int32_t cz = int32_t(std::floor(lo_z / kCell)); cz <= int32_t(std::floor(hi_z / kCell)); ++cz) {
				for (int32_t cx = int32_t(std::floor(lo_x / kCell)); cx <= int32_t(std::floor(hi_x / kCell)); ++cx) {
					cells[{ cx, cz }].push_back(uint32_t(triangles.size()));
				}
			}
			triangles.push_back(t);
		}
	}

	bool covers(float p_x, float p_z) const {
		const auto found = cells.find({ int32_t(std::floor(p_x / kCell)), int32_t(std::floor(p_z / kCell)) });
		if (found == cells.end()) {
			return false;
		}
		for (const uint32_t index : found->second) {
			const std::array<float, 6> &t = triangles[index];
			const auto side = [&](size_t p_a, size_t p_b) {
				return double(t[p_b * 2] - t[p_a * 2]) * double(p_z - t[p_a * 2 + 1]) -
						double(t[p_b * 2 + 1] - t[p_a * 2 + 1]) * double(p_x - t[p_a * 2]);
			};
			const double a = side(0, 1), b = side(1, 2), c = side(2, 0);
			const double slack = 1e-3;
			if ((a >= -slack && b >= -slack && c >= -slack) || (a <= slack && b <= slack && c <= slack)) {
				return true;
			}
		}
		return false;
	}
};

struct DecalCounts {
	size_t shapes = 0;
	size_t shapes_without_length = 0;
	uint32_t records = 0;
	uint32_t shaders = 0;
	std::vector<std::string> refused;
	std::string asphalt;
	std::vector<uint8_t> layers_under_asphalt;
	uint32_t asphalt_records = 0;
	uint32_t water_records = 0;
	size_t water_shapes = 0;
	size_t surface_shapes = 0;
	uint32_t surface_records = 0;
	size_t without_shader = 0;
	size_t shaderless_under_decal = 0;
	size_t shaderless_under_water = 0;
};

constexpr double kRoadLayerMargin = 64.0;

struct RasterContrast {
	size_t pairs = 0;
	double colour_across = 0.0;
	double colour_sides = 0.0;
	std::vector<double> mask_road;
	std::vector<double> mask_sides;
};

RasterContrast road_raster_contrast(
		const level::LoadedLevel &p_level, const render::SceneTerrainPlan &p_terrain, const std::string &p_shader) {
	RasterContrast out;
	auto res = p_level.mount().read_res(render::terrain_decals_name(p_terrain.asset));
	REQUIRE(res.ok());
	res::TerrainDecals decals;
	REQUIRE(res::TerrainDecals::parse(res->second, decals).ok());
	test::LoadedTerrainTree tree;
	REQUIRE(test::load_terrain_tree(p_level.mount(), p_terrain.asset, tree));
	REQUIRE(tree.tree.colour.has_value());
	test::TerrainChunks chunks(p_level.mount());
	const res::TerrainChunkFetch fetch = chunks.fetch();
	res::TerrainMaskField masks;
	REQUIRE(res::TerrainMaskField::build(tree.tree, fetch, masks).ok());
	const res::TerrainColourTree &colour = *tree.tree.colour;
	const uint8_t depth = colour.max_level();
	const float texel = (colour.bounds[2] - colour.bounds[0]) / float(uint32_t(1) << depth) /
			float(colour.texels_per_side - res::kTerrainColourBorder);
	const auto luma = [&](const std::array<float, 2> &p_at) {
		res::TerrainRegion region;
		region.origin = p_at;
		region.texel = texel;
		region.width = 1;
		region.height = 1;
		std::vector<uint8_t> rgba;
		REQUIRE(tree.tree.colour_grid(region, depth, fetch, rgba).ok());
		REQUIRE(rgba.size() == 4);
		return (0.299 * rgba[0] + 0.587 * rgba[1] + 0.114 * rgba[2]) / 255.0;
	};
	const level::LevelTransform &t = p_terrain.transform;
	const auto local = [&](const Vec3 &p_at) {
		return std::array<float, 2>{ (p_at[0] - t.trans[0]) / t.scale, (p_at[2] - t.trans[2]) / t.scale };
	};
	out.mask_road.assign(masks.layers(), 0.0);
	out.mask_sides.assign(masks.layers(), 0.0);
	const res::TerrainDecalSet &set = decals.planar;
	for (const res::TerrainDecalRecord &r : set.records) {
		if (to_lower_ascii(r.shader) != p_shader) {
			continue;
		}
		for (uint32_t a = r.first_vertex; a < r.end_vertex; ++a) {
			for (uint32_t b = r.first_vertex; b < r.end_vertex; ++b) {
				const res::TerrainDecalVertex &left = set.vertices[a], &right = set.vertices[b];
				if (!(left.uv[0] == 0.0f && right.uv[0] == 1.0f && left.uv[1] == right.uv[1])) {
					continue;
				}
				const std::array<float, 2> l = local(left.position), q = local(right.position);
				const std::array<float, 2> on{ (l[0] + q[0]) * 0.5f, (l[1] + q[1]) * 0.5f };
				const std::array<float, 2> beyond_left{ 2.0f * l[0] - q[0], 2.0f * l[1] - q[1] };
				const std::array<float, 2> beyond_right{ 2.0f * q[0] - l[0], 2.0f * q[1] - l[1] };
				const double c_on = luma(on), c_left = luma(beyond_left), c_right = luma(beyond_right);
				out.colour_across += std::fabs(c_on - (c_left + c_right) * 0.5);
				out.colour_sides += std::fabs(c_left - c_right);
				for (uint8_t layer = 0; layer < masks.layers(); ++layer) {
					out.mask_road[layer] += masks.coverage(layer, on[0], on[1]);
					out.mask_sides[layer] += (masks.coverage(layer, beyond_left[0], beyond_left[1]) +
													 masks.coverage(layer, beyond_right[0], beyond_right[1])) *
							0.5;
				}
				++out.pairs;
			}
		}
	}
	REQUIRE(out.pairs > 0);
	out.colour_across /= double(out.pairs);
	out.colour_sides /= double(out.pairs);
	for (size_t layer = 0; layer < out.mask_road.size(); ++layer) {
		out.mask_road[layer] /= double(out.pairs);
		out.mask_sides[layer] /= double(out.pairs);
	}
	return out;
}

void check_terrain_decals(const char *p_level, const char *p_mode, const DecalCounts &p_data) {
	test::Mounted *m = test::shared_mount_or_skip();
	if (m == nullptr) {
		return;
	}
	Result<std::unique_ptr<level::LoadedLevel>> loaded = level::load_level(m->install, p_level, p_mode);
	REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
	const level::LoadedLevel &lvl = **loaded;
	Result<std::unique_ptr<level::World>> world = level::World::traverse(lvl);
	REQUIRE(world.ok());
	Result<level::MeshCensus> census = level::mesh_census(lvl, **world);
	REQUIRE(census.ok());
	Result<render::LevelScenePlan> plan = render::plan_level_scene(lvl, **world, *census);
	REQUIRE_MESSAGE(plan.ok(), (plan.ok() ? std::string() : plan.error().message));
	REQUIRE(plan->terrains.size() == 1);
	Result<render::PreparedTerrain> terrain = render::prepare_scene_terrain(lvl.mount(), *plan, 0);
	REQUIRE_MESSAGE(terrain.ok(), (terrain.ok() ? std::string() : terrain.error().message));
	REQUIRE(terrain->look.has_value());

	DrawnDecals drawn, water, surface;
	size_t decal_surfaces = 0, water_surfaces = 0, surface_surfaces = 0;
	for (const render::PreparedSurface &s : terrain->mesh.surfaces) {
		const bool is_surface =
				std::any_of(plan->terrains[0].surface_decals.begin(), plan->terrains[0].surface_decals.end(),
						[&](const render::SceneWaterShader &w) { return w.shader == s.material_name; });
		if (is_surface) {
			surface.add(s);
			REQUIRE(s.material_index < terrain->look->materials.size());
			const std::string &code = terrain->look->materials[s.material_index].code;
			CHECK(code.find("ALBEDO = decal_colour * decal_colour;") != std::string::npos);
			CHECK(code.find("NORMAL = normalize((VIEW_MATRIX * vec4(o0.xyz") != std::string::npos);
			CHECK(code.find("skip_vertex_transform") != std::string::npos);
			CHECK(code.find("VERTEX = (MODELVIEW_MATRIX * vec4(VERTEX, 1.0)).xyz * " +
						  std::to_string(1.0f - render::kTerrainDecalViewPull)) != std::string::npos);
			++surface_surfaces;
			continue;
		}
		const bool is_water = std::any_of(plan->terrains[0].water.begin(), plan->terrains[0].water.end(),
				[&](const render::SceneWaterShader &w) { return w.shader == s.material_name; });
		if (is_water) {
			water.add(s);
			++water_surfaces;
			REQUIRE(s.material_index < terrain->look->materials.size());
			const std::string &code = terrain->look->materials[s.material_index].code;
			CHECK(code.find("blend_premul_alpha") != std::string::npos);
			CHECK(code.find("uniform samplerCube t") != std::string::npos);
		} else if (s.material_name != "terrain") {
			drawn.add(s);
			++decal_surfaces;
			REQUIRE(s.material_index < terrain->look->materials.size());
			CHECK(terrain->look->materials[s.material_index].render_priority < 0);
		}
	}
	const EbxPartition *partition = lvl.db().partition_by_name(render::terrain_decals_name(plan->terrains[0].asset));
	REQUIRE(partition != nullptr);
	size_t shapes = 0, shapes_drawn = 0, shapes_without_length = 0, shapes_refused = 0, points = 0, points_drawn = 0;
	size_t surface_shapes = 0, surface_shapes_drawn = 0, without_shader = 0, without_shader_recorded = 0;
	size_t without_shader_under_water = 0;
	for (const char *type : { "RoadData", "TerrainQuadDecalData" }) {
		for (const EbxInstance &shape : partition->instances_of(type)) {
			const std::optional<EbxInstance> shader2d = lvl.db().follow(shape.data(), "Shader2d");
			if (!shader2d) {
				const bool has_surface = lvl.db().follow(shape.data(), "Shader3d").has_value();
				const std::optional<EbxValue> list = shape.get("Points");
				const OptionalEbxArray array = list ? list->as_array() : OptionalEbxArray();
				REQUIRE(bool(array));
				std::vector<Vec3> at;
				for (const EbxValue &v : *array) {
					const std::optional<EbxStruct> st = v.as_struct();
					const std::optional<Vec3> p = st ? st->vec3() : std::nullopt;
					REQUIRE(p.has_value());
					at.push_back(*p);
				}
				if (has_surface) {
					++surface_shapes;
					size_t covered = 0;
					for (const Vec3 &p : at) {
						covered += surface.covers(p.x, p.z) ? 1 : 0;
					}
					surface_shapes_drawn += covered == at.size() ? 1 : 0;
				} else {
					++without_shader;
					size_t under_decal = 0, under_water = 0;
					float lowest = INFINITY, highest = -INFINITY;
					for (const Vec3 &p : at) {
						under_decal += drawn.covers(p.x, p.z) || surface.covers(p.x, p.z) ? 1 : 0;
						under_water += water.covers(p.x, p.z) ? 1 : 0;
						lowest = std::min(lowest, p.y);
						highest = std::max(highest, p.y);
					}
					without_shader_recorded += under_decal;
					without_shader_under_water += under_water;
					std::printf("%s %s: %s %s has neither Shader2d nor Shader3d; of its %zu points at y %.2f..%.2f, "
								"%zu lie "
								"under a drawn decal and %zu under the drawn water\n",
							p_level, p_mode, type, shape.guid().to_string().c_str(), at.size(), double(lowest),
							double(highest), under_decal, under_water);
				}
				continue;
			}
			const std::optional<std::string_view> shader_name = lvl.db().name_of(shader2d->partition().guid());
			REQUIRE(shader_name.has_value());
			const std::optional<EbxValue> list = shape.get("Points");
			const OptionalEbxArray array = list ? list->as_array() : OptionalEbxArray();
			REQUIRE(bool(array));
			std::vector<Vec3> at;
			for (const EbxValue &v : *array) {
				const std::optional<EbxStruct> s = v.as_struct();
				const std::optional<Vec3> p = s ? s->vec3() : std::nullopt;
				REQUIRE(p.has_value());
				at.push_back(*p);
			}
			double length = 0.0;
			for (size_t i = 1; i < at.size(); ++i) {
				length += std::hypot(double(at[i].x - at[i - 1].x), double(at[i].z - at[i - 1].z));
			}
			++shapes;
			if (!(length > 0.0)) {
				++shapes_without_length;
				continue;
			}
			if (std::find(p_data.refused.begin(), p_data.refused.end(), to_lower_ascii(*shader_name)) !=
					p_data.refused.end()) {
				++shapes_refused;
				continue;
			}
			if (std::string_view(type) == "TerrainQuadDecalData") {
				Vec3 centre{};
				for (const Vec3 &p : at) {
					centre = { centre.x + p.x / float(at.size()), 0.0f, centre.z + p.z / float(at.size()) };
				}
				at = { centre };
			} else if (at.size() > 2) {
				at.erase(at.begin());
				at.pop_back();
			}
			size_t covered = 0;
			for (const Vec3 &p : at) {
				covered += drawn.covers(p.x, p.z) ? 1 : 0;
			}
			points += at.size();
			points_drawn += covered;
			shapes_drawn += covered * 2 >= at.size() && covered > 0 ? 1 : 0;
		}
	}
	if (!p_data.asphalt.empty()) {
		const RasterContrast contrast = road_raster_contrast(lvl, plan->terrains[0], p_data.asphalt);
		std::printf(
				"%s %s: %zu %s cross sections; colour raster luma, road centre against the mean a road width beyond "
				"both edges %.4f, one side against the other %.4f; mask coverage on the road and beyond it, by layer:",
				p_level, p_mode, contrast.pairs, p_data.asphalt.c_str(), contrast.colour_across, contrast.colour_sides);
		std::vector<uint8_t> under;
		for (size_t layer = 0; layer < contrast.mask_road.size(); ++layer) {
			std::printf(" %zu %.3f/%.3f", layer, contrast.mask_road[layer], contrast.mask_sides[layer]);
			if (contrast.mask_road[layer] - contrast.mask_sides[layer] > kRoadLayerMargin) {
				under.push_back(uint8_t(layer));
			}
		}
		CHECK(contrast.colour_across < contrast.colour_sides);
		CHECK(under == p_data.layers_under_asphalt);
		std::printf("\n");
	}
	size_t water_shapes = 0, water_shapes_drawn = 0;
	for (const char *type : { "RiverData", "LakeData" }) {
		for (const EbxInstance &shape : partition->instances_of(type)) {
			const std::optional<EbxValue> list = shape.get("Points");
			const OptionalEbxArray array = list ? list->as_array() : OptionalEbxArray();
			REQUIRE(bool(array));
			std::vector<Vec3> at;
			for (const EbxValue &v : *array) {
				const std::optional<EbxStruct> st = v.as_struct();
				const std::optional<Vec3> p = st ? st->vec3() : std::nullopt;
				REQUIRE(p.has_value());
				at.push_back(*p);
			}
			if (std::string_view(type) == "RiverData" && at.size() > 2) {
				at.erase(at.begin());
				at.pop_back();
			} else if (std::string_view(type) == "LakeData" && at.size() > 2) {
				std::vector<Vec3> outline = at;
				at.clear();
				float lo_x = INFINITY, lo_z = INFINITY, hi_x = -INFINITY, hi_z = -INFINITY;
				for (const Vec3 &p : outline) {
					lo_x = std::min(lo_x, p.x), hi_x = std::max(hi_x, p.x);
					lo_z = std::min(lo_z, p.z), hi_z = std::max(hi_z, p.z);
				}
				constexpr int kLakeSamples = 32;
				for (int iz = 0; iz < kLakeSamples; ++iz) {
					for (int ix = 0; ix < kLakeSamples; ++ix) {
						const float x = lo_x + (hi_x - lo_x) * (float(ix) + 0.5f) / float(kLakeSamples);
						const float z = lo_z + (hi_z - lo_z) * (float(iz) + 0.5f) / float(kLakeSamples);
						bool inside = false;
						for (size_t i = 0, j = outline.size() - 1; i < outline.size(); j = i++) {
							const Vec3 &a = outline[i], &b = outline[j];
							if ((a.z > z) != (b.z > z) && x < (b.x - a.x) * (z - a.z) / (b.z - a.z) + a.x) {
								inside = !inside;
							}
						}
						if (inside) {
							at.push_back(Vec3{ x, 0.0f, z });
						}
					}
				}
			}
			size_t covered = 0;
			for (const Vec3 &p : at) {
				covered += water.covers(p.x, p.z) ? 1 : 0;
			}
			std::printf("%s %s: %s with %zu points (a lake's are a grid inside its outline), %zu under a drawn water "
						"triangle\n",
					p_level, p_mode, type, at.size(), covered);
			++water_shapes;
			water_shapes_drawn += covered * 2 >= at.size() && covered > 0 ? 1 : 0;
		}
	}
	const render::TerrainWaterStats &ws = terrain->water;
	std::printf("%s %s: water records %u of %u, shaders %u of %u, %zu surfaces, %llu triangles; %zu of %zu river and "
				"lake shapes under drawn water\n",
			p_level, p_mode, ws.drawn_records, ws.records, ws.drawn_shaders, ws.shaders, water_surfaces,
			(unsigned long long)ws.triangles, water_shapes_drawn, water_shapes);
	for (const auto &[shader, why] : ws.undrawn) {
		std::printf("  water not drawn: %s: %s\n", shader.c_str(), why.c_str());
	}
	CHECK(ws.records == p_data.water_records);
	CHECK(ws.drawn_records == ws.records);
	CHECK(ws.drawn_shaders == ws.shaders);
	CHECK(ws.undrawn.empty());
	CHECK(water_surfaces == ws.surfaces);
	CHECK(water_shapes == p_data.water_shapes);
	CHECK(water_shapes_drawn == water_shapes);
	const render::TerrainDecalStats &stats = terrain->decals;
	std::printf("%s %s: %zu of %zu road and decal shapes drawn, %zu more have no length (%zu of %zu control points "
				"under a drawn triangle); records "
				"%u of %u, shaders %u of %u, %zu surfaces, %llu triangles from %llu, area %.0f of %.0f m2\n",
			p_level, p_mode, shapes_drawn, shapes - shapes_without_length, shapes_without_length, points_drawn, points,
			stats.drawn_records, stats.records, stats.drawn_shaders, stats.shaders, decal_surfaces,
			(unsigned long long)stats.drawn_triangles, (unsigned long long)stats.data_triangles, stats.drawn_area,
			stats.data_area);
	for (const auto &[shader, why] : stats.undrawn) {
		std::printf("  not drawn: %s: %s\n", shader.c_str(), why.c_str());
	}
	CHECK(shapes == p_data.shapes);
	CHECK(shapes_without_length == p_data.shapes_without_length);
	CHECK((shapes_refused > 0) == !p_data.refused.empty());
	CHECK(shapes_drawn == shapes - shapes_without_length - shapes_refused);
	CHECK(stats.drawn_records + stats.refused_records == stats.records);
	CHECK(stats.drawn_area + stats.refused_area == doctest::Approx(stats.data_area).epsilon(0.001));
	CHECK(stats.records == p_data.records);
	CHECK(stats.shaders == p_data.shaders);
	CHECK(stats.drawn_shaders + p_data.refused.size() == stats.shaders);
	std::vector<std::string> undrawn;
	for (const auto &[shader, why] : stats.undrawn) {
		undrawn.push_back(shader);
	}
	CHECK(undrawn == p_data.refused);
	if (!p_data.asphalt.empty()) {
		const auto asphalt = stats.shader_records.find(p_data.asphalt);
		REQUIRE(asphalt != stats.shader_records.end());
		std::printf("%s %s: %s records %u of %u drawn\n", p_level, p_mode, p_data.asphalt.c_str(), asphalt->second[1],
				asphalt->second[0]);
		CHECK(asphalt->second[0] == p_data.asphalt_records);
		CHECK(asphalt->second[1] == p_data.asphalt_records);
		for (const render::PreparedSurface &s : terrain->mesh.surfaces) {
			if (s.material_name == p_data.asphalt) {
				const std::string &code = terrain->look->materials[s.material_index].code;
				const auto hints = [&](const char *p_texture) {
					const size_t at = code.find(std::string("uniform sampler2D ") + p_texture + " : ");
					return at == std::string::npos ? std::string() : code.substr(at, code.find('\n', at) - at);
				};
				CHECK(hints("t1").ends_with("repeat_disable;"));
				CHECK(hints("t2").ends_with("repeat_disable;"));
				CHECK(hints("t3").ends_with("repeat_enable;"));
				CHECK(code.find("? textureGrad(t2, vec2(") != std::string::npos);
				CHECK(code.find(" : vec4(0.0, 0.0, 0.0, 0.0))") != std::string::npos);
				CHECK(code.find("textureGrad(t1, vec2((1.0 - abs(mod(") != std::string::npos);
				CHECK(code.find("texture(t3, ") != std::string::npos);
			}
		}
	}
	CHECK(decal_surfaces == stats.surfaces);
	const render::TerrainSurfaceDecalStats &ss = terrain->surface_decals;
	std::printf("%s %s: surface (Shader3d) decals of the shapes without a Shader2d: %zu shapes, %zu with every corner "
				"under a drawn triangle; records %u of %u, shaders %u of %u, %zu surfaces, %llu triangles\n",
			p_level, p_mode, surface_shapes, surface_shapes_drawn, ss.drawn_records, ss.records, ss.drawn_shaders,
			ss.shaders, surface_surfaces, (unsigned long long)ss.triangles);
	for (const auto &[shader, why] : ss.undrawn) {
		std::printf("  surface decal not drawn: %s: %s\n", shader.c_str(), why.c_str());
	}
	size_t surface_codes = 0, surface_lit = 0;
	for (const render::TerrainMaterial &m : terrain->look->materials) {
		if (m.code.find("vec3 decal_colour = o1.xyz / decal_cover;") == std::string::npos) {
			continue;
		}
		++surface_codes;
		surface_lit +=
				render::uses_light_pass(m.code) && m.code.find("bf3_specular = max(0.0, 0.0);") != std::string::npos;
	}
	CHECK(surface_lit == surface_codes);
	CHECK(surface_shapes == p_data.surface_shapes);
	CHECK(surface_shapes_drawn == surface_shapes);
	CHECK(ss.records == p_data.surface_records);
	CHECK(ss.drawn_records == ss.records);
	CHECK(ss.drawn_shaders == ss.shaders);
	CHECK(ss.undrawn.empty());
	CHECK(surface_surfaces == ss.surfaces);
	CHECK(without_shader == p_data.without_shader);
	CHECK(without_shader_recorded == p_data.shaderless_under_decal);
	CHECK(without_shader_under_water == p_data.shaderless_under_water);
}

}

TEST_SUITE("terrain-decals-draw-unit") {
	TEST_CASE("terrain decals: a triangle is cut along the terrain's own triangles and keeps its area") {
		core::res::HeightGrid grid;
		grid.origin_xz = { 0.0f, 0.0f };
		grid.sample_spacing = { 4.0f, 4.0f };
		grid.width = 4;
		grid.height = 4;
		grid.heights.assign(16, 0.0f);
		grid.holes.assign(16, 0);
		for (size_t z = 0; z < 4; ++z) {
			for (size_t x = 0; x < 4; ++x) {
				grid.heights[x + z * 4] = float(x) * 2.0f + float(z);
			}
		}
		std::array<render::DrapeVertex, 3> triangle;
		triangle[0] = { 1.0f, 1.0f, { 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } };
		triangle[1] = { 9.0f, 1.0f, { 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 1.0f } };
		triangle[2] = { 1.0f, 9.0f, { 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } };
		std::vector<render::DrapedVertex> out;
		const double area = render::drape_triangle(grid, triangle, out);
		CHECK(area == doctest::Approx(32.0));
		REQUIRE(out.size() % 3 == 0);
		CHECK(out.size() >= 9);
		double summed = 0.0;
		for (size_t i = 0; i < out.size(); i += 3) {
			const double twice = double(out[i + 1].x - out[i].x) * double(out[i + 2].z - out[i].z) -
					double(out[i + 1].z - out[i].z) * double(out[i + 2].x - out[i].x);
			CHECK(twice > 0.0);
			summed += twice * 0.5;
		}
		CHECK(summed == doctest::Approx(32.0));
		for (const render::DrapedVertex &v : out) {
			CHECK(v.y == doctest::Approx(v.x * 0.5f + v.z * 0.25f));
			CHECK(v.uv[0] == doctest::Approx((v.x - 1.0f) / 8.0f));
			CHECK(v.uv[1] == doctest::Approx((v.z - 1.0f) / 8.0f));
			CHECK(v.mask[0] + v.mask[1] + v.mask[2] == doctest::Approx(1.0f));
			CHECK(v.mask[3] == doctest::Approx(1.0f));
		}
		std::vector<render::DrapedVertex> cropped;
		const render::DrapeCells cells = render::drape_cells(grid, std::array<float, 4>{ 0.0f, 0.0f, 4.0f, 12.0f });
		CHECK(cells.x1 == 1);
		CHECK(render::drape_triangle(grid, triangle, cropped, cells) == doctest::Approx(32.0 - 12.5));
		grid.holes[1 + 1 * 4] = 1;
		std::vector<render::DrapedVertex> holed;
		CHECK(render::drape_triangle(grid, triangle, holed) == doctest::Approx(1.0));
	}

	TEST_CASE("terrain decals: a sampler's address modes reach the sample coordinate and the texture's repeat") {
		using namespace dxbct;
		const std::vector<std::vector<uint32_t>> code = {
			ins(69, { dst(kTemp, 0, 0xF), src(kInput, 3, swz(0, 1, 0, 0)), src(kResource, 1), sampler(0) }),
			ins(69, { dst(kTemp, 1, 0xF), src(kInput, 3, swz(0, 1, 0, 0)), src(kResource, 2), sampler(1) }),
			ins(69, { dst(kTemp, 2, 0xF), src(kInput, 3, swz(0, 1, 0, 0)), src(kResource, 3), sampler(2) }),
			ins(0, { dst(kOutput, 0, 0xF), src(kTemp, 0), src(kTemp, 1) }),
		};
		Result<render::dxbc::Program> program = render::dxbc::parse(container({ { "SHDR", code_chunk(0, 3, code) } }));
		REQUIRE(program.ok());
		std::vector<render::TextureSampler> samplers(3);
		samplers[0] = { 0, { 2, 1, 1 }, {} };
		samplers[1] = { 1, { 4, 1, 1 }, { 0.5f, 0.0f, 0.0f, 1.0f } };
		samplers[2] = { 2, { 1, 1, 1 }, {} };
		Result<render::PixelTranslation> plain = render::translate_pixel_program(*program);
		Result<render::PixelTranslation> t = render::translate_pixel_program(*program, samplers);
		REQUIRE(plain.ok());
		REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
		INFO(t->body);
		CHECK(plain->clamped.empty());
		CHECK(t->clamped == std::vector<uint32_t>{ 1, 2 });
		const std::string c = "(v3.xyxx).xy";
		CHECK(t->body.find("textureGrad(t1, vec2((1.0 - abs(mod(" + c + ".x, 2.0) - 1.0)), fract(" + c + ".y)), dFdx(" +
					  c + "), dFdy(" + c + "))") != std::string::npos);
		CHECK(t->body.find("((" + c + ".x >= 0.0 && " + c + ".x <= 1.0) ? textureGrad(t2, vec2(" + c + ".x, fract(" +
					  c + ".y)), dFdx(" + c + "), dFdy(" + c + ")) : vec4(0.5, 0.0, 0.0, 1.0))") != std::string::npos);
		CHECK(t->body.find("texture(t3, " + c + ")") != std::string::npos);
		CHECK(plain->body.find("texture(t1, " + c + ")") != std::string::npos);
		render::TerrainMaterial material;
		for (uint32_t slot = 1; slot <= 3; ++slot) {
			render::TerrainTextureBinding b;
			b.slot = slot;
			b.texture = int32_t(slot);
			material.textures.push_back(b);
		}
		Result<std::string> head = render::terrain_shader_head(*t, material, "test", "blend_mix");
		REQUIRE(head.ok());
		CHECK(head->find("uniform sampler2D t1 : filter_linear_mipmap_anisotropic, repeat_disable;") !=
				std::string::npos);
		CHECK(head->find("uniform sampler2D t2 : filter_linear_mipmap_anisotropic, repeat_disable;") !=
				std::string::npos);
		CHECK(head->find("uniform sampler2D t3 : filter_linear_mipmap_anisotropic, repeat_enable;") !=
				std::string::npos);
		samplers.pop_back();
		CHECK_FALSE(render::translate_pixel_program(*program, samplers).ok());
		samplers.push_back({ 2, { 1, 6, 1 }, {} });
		CHECK_FALSE(render::translate_pixel_program(*program, samplers).ok());
	}

	TEST_CASE("terrain decals: the shader takes the mask, the world position and the decal coordinates") {
		render::PixelTranslation translation;
		translation.outputs = 3;
		translation.inputs = { 1, 2, 3 };
		translation.body = "o0 = v1 + v2 + v3;\n";
		render::TerrainMaterial material;
		material.surface.smoothness.atlas = 1;
		material.surface.smoothness.lane = 2;
		material.surface.specular.atlas = 1;
		Result<std::string> code = render::terrain_decal_shader_code(translation, material);
		REQUIRE(code.ok());
		CHECK(render::uses_light_pass(*code));
		CHECK(code->find("bf3_smoothness = clamp((abs(o2.y) / decal_cover), 0.0, 1.0);") != std::string::npos);
		CHECK(code->find("bf3_specular = max(" + render::terrain_page_camera() +
					  " ? 0.0 : (abs(o1.y) / decal_cover) * (abs(o1.y) / decal_cover), 0.0);") != std::string::npos);
		CHECK(code->find("depth_draw_never") != std::string::npos);
		CHECK(code->find("vec4 v1 = COLOR;") != std::string::npos);
		CHECK(code->find("ALPHA = clamp(o0.w, 0.0, 1.0);") != std::string::npos);
		translation.inputs = { 4 };
		CHECK_FALSE(render::terrain_decal_shader_code(translation, material).ok());
		translation.inputs = { 1 };
		translation.outputs = 1;
		CHECK_FALSE(render::terrain_decal_shader_code(translation, material).ok());
	}
}

TEST_SUITE("terrain-decals-draw") {
	TEST_CASE("terrain decals: MP_007 ConquestLarge0 draws its roads and quad decals") {
		check_terrain_decals("MP_007", "ConquestLarge0",
				DecalCounts{ 156, 1, 281, 5, {}, "levels/mp_007/shaders/mp_007_roadasphalt2d", {}, 72, 42, 4, 0, 0, 1,
						1, 53 });
	}

	TEST_CASE("terrain decals: MP_Subway ConquestLarge0 draws its roads and quad decals") {
		check_terrain_decals("MP_Subway", "ConquestLarge0", DecalCounts{ 75, 0, 44, 3, {}, {}, {}, 0, 25, 2, 4, 2, 0 });
	}

	TEST_CASE("terrain decals: MP_018 ConquestLarge0 draws its roads and quad decals") {
		check_terrain_decals("MP_018", "ConquestLarge0",
				DecalCounts{
						260, 0, 503, 7, {}, "levels/mp_018/terrain/shaders/mp018_road_01_2d", { 7 }, 121, 556, 1 });
	}
}
