#include "bf3/core/level/census.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "bf3/core/res/terrain_mask_field.h"
#include "bf3/render/level_scene.h"
#include "bf3/render/terrain_detail.h"
#include "test_support.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
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
