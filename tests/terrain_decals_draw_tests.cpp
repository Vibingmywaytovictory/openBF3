#include "bf3/core/level/census.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "bf3/core/res/terrain_decals.h"
#include "bf3/core/res/terrain_mask_field.h"
#include "bf3/core/util/text.h"
#include "bf3/render/level_scene.h"
#include "bf3/render/roads.h"
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

	DrawnDecals drawn;
	size_t decal_surfaces = 0;
	for (const render::PreparedSurface &s : terrain->mesh.surfaces) {
		if (s.material_name != "terrain") {
			drawn.add(s);
			++decal_surfaces;
			REQUIRE(s.material_index < terrain->look->materials.size());
			CHECK(terrain->look->materials[s.material_index].render_priority < 0);
		}
	}
	const EbxPartition *partition = lvl.db().partition_by_name(render::terrain_decals_name(plan->terrains[0].asset));
	REQUIRE(partition != nullptr);
	size_t shapes = 0, shapes_drawn = 0, shapes_without_length = 0, shapes_refused = 0, points = 0, points_drawn = 0;
	for (const char *type : { "RoadData", "TerrainQuadDecalData" }) {
		for (const EbxInstance &shape : partition->instances_of(type)) {
			const std::optional<EbxInstance> shader2d = lvl.db().follow(shape.data(), "Shader2d");
			if (!shader2d) {
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
		CHECK(why.find("address modes are not decoded") != std::string::npos);
	}
	CHECK(undrawn == p_data.refused);
	CHECK(decal_surfaces == stats.surfaces);
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

	TEST_CASE("terrain decals: the shader takes the mask, the world position and the decal coordinates") {
		render::PixelTranslation translation;
		translation.outputs = 3;
		translation.inputs = { 1, 2, 3 };
		translation.body = "o0 = v1 + v2 + v3;\n";
		render::TerrainMaterial material;
		Result<std::string> code = render::terrain_decal_shader_code(translation, material);
		REQUIRE(code.ok());
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
				DecalCounts{ 156, 1, 281, 5, { "levels/mp_007/shaders/mp_007_roadasphalt2d" },
						"levels/mp_007/shaders/mp_007_roadasphalt2d" });
	}

	TEST_CASE("terrain decals: MP_Subway ConquestLarge0 draws its roads and quad decals") {
		check_terrain_decals("MP_Subway", "ConquestLarge0", DecalCounts{ 75, 0, 44, 3 });
	}

	TEST_CASE("terrain decals: MP_018 ConquestLarge0 draws its roads and quad decals") {
		check_terrain_decals("MP_018", "ConquestLarge0",
				DecalCounts{ 260, 0, 503, 7, { "levels/mp_018/terrain/shaders/mp018_road_01_2d" },
						"levels/mp_018/terrain/shaders/mp018_road_01_2d", { 7 } });
	}
}
