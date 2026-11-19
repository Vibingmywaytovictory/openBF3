#include "bf3/core/level/census.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "bf3/core/res/terrain_mask_field.h"
#include "bf3/core/util/text.h"
#include "bf3/render/level_scene.h"
#include "bf3/render/mesh_cache.h"
#include "bf3/render/terrain_detail.h"
#include "bf3/render/terrain_scatter.h"
#include "test_support.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <numbers>
#include <string>
#include <tuple>
#include <vector>

using namespace bf3;
using namespace bf3::core;

namespace {

struct Camera {
	const char *name;
	float x;
	float z;
};

constexpr Camera kCaspianCameras[] = { { "field", -236.9f, -87.7f }, { "forest", -28.3f, 98.9f },
	{ "hill", -383.1f, 245.3f } };

struct Caspian {
	std::unique_ptr<level::LoadedLevel> level;
	render::LevelScenePlan plan;
	render::PreparedTerrain terrain;
};

const Caspian *caspian() {
	static std::unique_ptr<Caspian> loaded;
	static bool tried = false;
	if (tried) {
		return loaded.get();
	}
	tried = true;
	test::Mounted *m = test::shared_mount_or_skip();
	if (m == nullptr) {
		return nullptr;
	}
	auto out = std::make_unique<Caspian>();
	Result<std::unique_ptr<level::LoadedLevel>> lvl = level::load_level(m->install, "MP_007", "ConquestLarge0");
	REQUIRE_MESSAGE(lvl.ok(), (lvl.ok() ? std::string() : lvl.error().message));
	out->level = std::move(*lvl);
	Result<std::unique_ptr<level::World>> world = level::World::traverse(*out->level);
	REQUIRE(world.ok());
	Result<level::MeshCensus> census = level::mesh_census(*out->level, **world);
	REQUIRE(census.ok());
	Result<render::LevelScenePlan> plan = render::plan_level_scene(*out->level, **world, *census);
	REQUIRE_MESSAGE(plan.ok(), (plan.ok() ? std::string() : plan.error().message));
	REQUIRE(plan->terrains.size() == 1);
	out->plan = std::move(*plan);
	Result<render::PreparedTerrain> terrain = render::prepare_scene_terrain(out->level->mount(), out->plan, 0);
	REQUIRE_MESSAGE(terrain.ok(), (terrain.ok() ? std::string() : terrain.error().message));
	out->terrain = std::move(*terrain);
	loaded = std::move(out);
	return loaded.get();
}

std::array<float, 2> terrain_local(const render::LevelScenePlan &p_plan, float p_x, float p_z) {
	const level::LevelTransform &t = p_plan.terrains[0].transform;
	return { (p_x - t.trans.x) / t.scale, (p_z - t.trans.z) / t.scale };
}

constexpr int kSides = 8;

struct RockRest {
	float terrain = -INFINITY;
	float plane = -INFINITY;
	float flat = -INFINITY;
	float bend = 0.0f;
};

RockRest rock_rest(const render::ScatterInstance &p_instance, const std::vector<float> &p_positions,
		const render::ScatterHeights &p_heights) {
	const std::array<float, 12> rows = render::scatter_instance_rows(p_instance);
	std::array<float, kSides> on_terrain, on_plane, on_flat;
	on_terrain.fill(INFINITY);
	on_plane.fill(INFINITY);
	on_flat.fill(INFINITY);
	RockRest out;
	for (size_t v = 0; v + 2 < p_positions.size(); v += 3) {
		const float lx = p_positions[v], ly = p_positions[v + 1], lz = p_positions[v + 2];
		const float x = rows[0] * lx + rows[1] * ly + rows[2] * lz + rows[3];
		const float y = rows[4] * lx + rows[5] * ly + rows[6] * lz + rows[7];
		const float z = rows[8] * lx + rows[9] * ly + rows[10] * lz + rows[11];
		const std::optional<float> ground = p_heights.surface(x, z);
		if (!ground) {
			continue;
		}
		const std::optional<std::array<float, 3>> n = p_heights.normal(p_instance.position[0], p_instance.position[2]);
		const std::array<float, 3> tangent = n ? *n : std::array<float, 3>{ 0.0f, 1.0f, 0.0f };
		const float plane = p_instance.position[1] -
				(tangent[0] * (x - p_instance.position[0]) + tangent[2] * (z - p_instance.position[2])) / tangent[1];
		out.bend = std::fmax(out.bend, (plane - *ground) * tangent[1]);
		const double angle = std::atan2(double(lz), double(lx));
		const size_t side =
				size_t(std::clamp(int((angle + std::numbers::pi) / (2.0 * std::numbers::pi) * kSides), 0, kSides - 1));
		on_terrain[side] = std::fmin(on_terrain[side], (y - *ground) * tangent[1]);
		on_plane[side] = std::fmin(on_plane[side], (y - plane) * tangent[1]);
		on_flat[side] = std::fmin(on_flat[side], ly * p_instance.scale[1]);
	}
	for (size_t side = 0; side < kSides; ++side) {
		if (std::isfinite(on_terrain[side])) {
			out.terrain = std::fmax(out.terrain, on_terrain[side]);
			out.plane = std::fmax(out.plane, on_plane[side]);
			out.flat = std::fmax(out.flat, on_flat[side]);
		}
	}
	return out;
}
}

TEST_SUITE("render-terrain-detail-unit") {
	TEST_CASE(
			"terrain detail: a mask sample goes through the window function and a mask read any other way is refused") {
		const std::vector<uint32_t> masks{ 3 }, colours{ 5 };
		Result<render::TerrainDetailCode> code = render::terrain_detail_code(
				masks, colours, "r0 = texture(t3, (v1.xyxx).xy).xyzw; r1 = texture(t5, (r0.xyxx).xy).xyzw;");
		REQUIRE(code.ok());
		CHECK(code->body ==
				"r0 = terrain_detail_mask(t3, (v1.xyxx).xy).xyzw; r1 = terrain_detail_colour(t5, "
				"(r0.xyxx).xy).xyzw;");
		CHECK(code->functions.find("vec4 terrain_detail_mask(sampler2D p_coarse, vec2 p_uv)") != std::string::npos);
		CHECK(code->functions.find("vec4 terrain_detail_colour(sampler2D p_coarse, vec2 p_uv)") != std::string::npos);
		CHECK_FALSE(render::terrain_detail_code(masks, {}, "r0 = textureLod(t3, (v1.xyxx).xy, 0.0);").ok());
		CHECK_FALSE(render::terrain_detail_code(masks, {}, "r0 = texture(t4, (v1.xyxx).xy);").ok());
	}

	TEST_CASE("terrain detail: a window covers its texels and nothing past them") {
		core::res::TerrainRegion r;
		r.origin = { 10.0f, -20.0f };
		r.texel = 2.0f;
		r.width = 4;
		r.height = 3;
		CHECK(render::terrain_detail_covers(r, 10.0f, -20.0f));
		CHECK(render::terrain_detail_covers(r, 18.0f, -14.0f));
		CHECK_FALSE(render::terrain_detail_covers(r, 18.5f, -14.0f));
		CHECK_FALSE(render::terrain_detail_covers(r, 9.5f, -15.0f));
		r.width = 0;
		CHECK_FALSE(render::terrain_detail_covers(r, 12.0f, -15.0f));
	}
}

TEST_SUITE("render-terrain-detail") {
	TEST_CASE("terrain detail: on MP_007 the ground under each camera draws its masks and colour at the data's texel") {
		const Caspian *c = caspian();
		if (c == nullptr) {
			return;
		}
		REQUIRE(c->terrain.look.has_value());
		REQUIRE(c->terrain.detail != nullptr);
		const render::TerrainDetailLayout &layout = c->terrain.look->detail;
		REQUIRE(layout.colour.has_value());
		std::printf(
				"render.terrain_detail MP_007: whole look masks %.2f m (data %.2f m), colour %.2f m (data %.2f m) over "
				"%.0f x %.0f m; %.1f MiB of chunks kept\n",
				double(layout.mask.texel), double(layout.data_mask_texel), double(layout.colour->texel),
				double(layout.data_colour_texel), double(layout.region[2] - layout.region[0]),
				double(layout.region[3] - layout.region[1]), double(c->terrain.detail->bytes()) / 1048576.0);
		CHECK(layout.data_mask_texel == doctest::Approx(1.0f));
		CHECK(layout.data_colour_texel == doctest::Approx(2.0f));
		core::res::TerrainMaskField field;
		REQUIRE(core::res::TerrainMaskField::build(c->terrain.detail->tree, c->terrain.detail->fetch(), field).ok());
		for (const Camera &camera : kCaspianCameras) {
			const auto [x, z] = terrain_local(c->plan, camera.x, camera.z);
			Result<render::TerrainDetail> detail = render::build_terrain_detail(*c->terrain.detail, layout, x, z);
			REQUIRE_MESSAGE(detail.ok(), (detail.ok() ? std::string() : detail.error().message));
			const float mask = render::terrain_mask_texel_at(layout, &*detail, x, z);
			const float colour = render::terrain_colour_texel_at(layout, &*detail, x, z);
			std::printf(
					"render.terrain_detail MP_007 %s (%.1f, %.1f): masks %.2f m, colour %.2f m at the camera, window "
					"%u x %u masks from (%.1f, %.1f), %u x %u colour\n",
					camera.name, double(x), double(z), double(mask), double(colour), detail->mask.width,
					detail->mask.height, double(detail->mask.origin[0]), double(detail->mask.origin[1]),
					detail->colour.width, detail->colour.height);
			CHECK(mask == layout.data_mask_texel);
			CHECK(colour == layout.data_colour_texel);
			REQUIRE(detail->mask_atlas_width == layout.mask_columns * (detail->mask.width + 2));
			uint64_t compared = 0, differ = 0;
			for (uint32_t tile = 0; tile < layout.mask_tiles; ++tile) {
				const uint32_t ox = (tile % layout.mask_columns) * (detail->mask.width + 2) + 1;
				const uint32_t oy = (tile / layout.mask_columns) * (detail->mask.height + 2) + 1;
				for (uint32_t j = 0; j < detail->mask.height; j += 7) {
					for (uint32_t i = 0; i < detail->mask.width; i += 7) {
						const float wx = detail->mask.origin[0] + (float(i) + 0.5f) * detail->mask.texel;
						const float wz = detail->mask.origin[1] + (float(j) + 0.5f) * detail->mask.texel;
						const float data = field.coverage(uint8_t(tile), wx, wz);
						const uint8_t drawn = detail->mask_atlas[size_t(oy + j) * detail->mask_atlas_width + ox + i];
						++compared;
						differ += std::fabs(float(drawn) - data) > 1.0f ? 1 : 0;
					}
				}
			}
			std::printf("render.terrain_detail MP_007 %s: %llu of %llu window mask texels differ from the data's own\n",
					camera.name, static_cast<unsigned long long>(differ), static_cast<unsigned long long>(compared));
			CHECK(compared > 0);
			CHECK(differ == 0);
		}
	}
}

TEST_SUITE("render-scatter-ground") {
	TEST_CASE("scatter: every rock MP_007 scatters near the cameras rests on the drawn terrain as on flat ground") {
		const Caspian *c = caspian();
		if (c == nullptr) {
			return;
		}
		REQUIRE(c->terrain.scatter != nullptr);
		const render::TerrainScatter &scatter = *c->terrain.scatter;
		const LevelMount &mount = c->level->mount();
		std::map<int32_t, std::vector<float>> positions;
		uint64_t instances = 0, floating = 0;
		float worst = -INFINITY;
		for (uint32_t t = 0; t < scatter.types().size(); ++t) {
			const render::ScatterType &type = scatter.types()[t];
			if (type.mesh < 0) {
				continue;
			}
			const render::SceneMeshPlan &mesh_plan = c->plan.meshes[size_t(type.mesh)];
			const std::string name = to_lower_ascii(c->plan.requests[mesh_plan.request].res);
			if (name.find("rock") == std::string::npos) {
				continue;
			}
			std::vector<float> &local = positions[type.mesh];
			if (local.empty()) {
				Result<render::CachedMesh> blob =
						render::load_scene_request(nullptr, mount, c->plan, mesh_plan.request);
				REQUIRE_MESSAGE(blob.ok(), (blob.ok() ? std::string() : blob.error().message));
				Result<render::PreparedSceneMesh> mesh =
						render::prepare_scene_mesh(nullptr, mount, c->plan, uint32_t(type.mesh), blob->mesh);
				REQUIRE_MESSAGE(mesh.ok(), (mesh.ok() ? std::string() : mesh.error().message));
				for (const render::PreparedSurface &s : mesh->lod0.surfaces) {
					local.insert(local.end(), s.positions.begin(), s.positions.end());
				}
			}
			REQUIRE(!local.empty());
			std::vector<render::ScatterFootprint> footprints(size_t(type.mesh) + 1);
			std::array<float, 4> box{ INFINITY, INFINITY, -INFINITY, -INFINITY };
			for (size_t v = 0; v + 2 < local.size(); v += 3) {
				box = { std::fmin(box[0], local[v]), std::fmin(box[1], local[v + 2]), std::fmax(box[2], local[v]),
					std::fmax(box[3], local[v + 2]) };
			}
			footprints[size_t(type.mesh)] = box;
			std::map<std::tuple<int32_t, int32_t>, bool> seen;
			uint64_t type_instances = 0, type_floating = 0;
			float type_worst = -INFINITY, slope_at_worst = 0.0f, flat_at_worst = 0.0f, type_bend = 0.0f,
				  steepest = 0.0f;
			std::array<float, 3> steepest_at{};
			for (const Camera &camera : kCaspianCameras) {
				const auto [x, z] = terrain_local(c->plan, camera.x, camera.z);
				for (const render::ScatterCellId &cell : scatter.cells_within(type.tree_level, x, z, 64.0f)) {
					if (!seen.emplace(std::make_tuple(cell.x, cell.z), true).second) {
						continue;
					}
					for (const render::ScatterBatch &batch : scatter.generate(cell, nullptr, footprints)) {
						if (batch.type != t) {
							continue;
						}
						for (const render::ScatterInstance &inst : batch.instances) {
							const RockRest rest = rock_rest(inst, local, scatter.heights());
							const float gap = rest.terrain;
							const float flat = rest.flat;
							++type_instances;
							type_bend = std::fmax(type_bend, rest.bend);
							const std::optional<std::array<float, 3>> slope =
									scatter.heights().normal(inst.position[0], inst.position[2]);
							if (slope && (*slope)[1] < 1.0f - steepest) {
								steepest = 1.0f - (*slope)[1];
								steepest_at = inst.position;
							}
							if (gap > flat + 1e-3f) {
								++type_floating;
							}
							if (gap - flat > type_worst) {
								type_worst = gap - flat;
								flat_at_worst = flat;
								const std::optional<std::array<float, 3>> n =
										scatter.heights().normal(inst.position[0], inst.position[2]);
								slope_at_worst =
										n ? float(std::acos(double((*n)[1])) * 180.0 / std::numbers::pi) : 0.0f;
							}
						}
					}
				}
			}
			std::printf(
					"render.scatter_ground MP_007 %s: %llu instances near the cameras, %llu float on a side over the "
					"drawn terrain, worst %.3f m over the mesh's own on flat ground (%.3f m) on a %.1f degree slope; "
					"the "
					"drawn terrain bends away under a rock by at most %.3f m; the steepest rock stands at (%.1f, %.1f, "
					"%.1f)\n",
					name.c_str(), static_cast<unsigned long long>(type_instances),
					static_cast<unsigned long long>(type_floating), double(type_worst), double(flat_at_worst),
					double(slope_at_worst), double(type_bend), double(steepest_at[0]), double(steepest_at[1]),
					double(steepest_at[2]));
			instances += type_instances;
			floating += type_floating;
			worst = std::fmax(worst, type_worst);
		}
		std::printf("render.scatter_ground MP_007: %llu rock instances, %llu float on a side, worst %.3f m\n",
				static_cast<unsigned long long>(instances), static_cast<unsigned long long>(floating), double(worst));
		CHECK(instances > 0);
		CHECK(floating == 0);
	}
}

TEST_SUITE("render-scatter-cut") {
	TEST_CASE("scatter: every scattered mesh of MP_007 cuts its alpha where its own program discards") {
		const Caspian *c = caspian();
		if (c == nullptr) {
			return;
		}
		REQUIRE(c->terrain.scatter != nullptr);
		std::map<uint32_t, bool> seen;
		uint32_t cut = 0, grass = 0;
		for (const render::ScatterType &type : c->terrain.scatter->types()) {
			if (type.mesh < 0) {
				continue;
			}
			const render::SceneMeshPlan &mesh_plan = c->plan.meshes[size_t(type.mesh)];
			const std::string name = to_lower_ascii(c->plan.requests[mesh_plan.request].res);
			for (uint32_t m : mesh_plan.surface_materials) {
				REQUIRE(m < c->plan.materials.size());
				const render::SceneMaterialPlan &mp = c->plan.materials[m];
				if (!seen.emplace(m, true).second) {
					continue;
				}
				REQUIRE(mp.look.has_value());
				std::printf("render.scatter_cut MP_007 %s: alpha cut %.3f\n", name.c_str(),
						mp.look->alpha_cut ? double(*mp.look->alpha_cut) : -1.0);
				CHECK(mp.alpha_test);
				CHECK(mp.look->alpha_cut.has_value());
				cut += mp.look->alpha_cut ? 1 : 0;
				if (name.find("moorlandgrass01") != std::string::npos) {
					++grass;
					CHECK(mp.look->alpha_cut.value_or(0.0f) == doctest::Approx(0.253f));
				}
			}
		}
		CHECK(cut == seen.size());
		CHECK(grass == 1);
	}
}
