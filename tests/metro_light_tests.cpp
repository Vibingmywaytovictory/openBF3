#include <doctest/doctest.h>

#include "bf3/core/level/census.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "bf3/render/level_scene.h"
#include "bf3/render/look_environment.h"
#include "test_support.h"

#include <array>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

using namespace bf3::core;
using namespace bf3::render;

namespace {

constexpr std::string_view kShadowMesh = "levels/mp_subway/objects/subway/tunnel/shadowmesh_mesh";
constexpr std::string_view kPixelOccluder = "objects/pixeloccluder_01/pixeloccluder_01_mesh";
constexpr std::string_view kBazaarOccluderWall = "levels/mp_001/props/mp001_occluderwall/mp001_occluderwall_01_mesh";
constexpr std::string_view kQuakeOccluderWall = "levels/xp4_earthquake/objects/occluderwall_01_mesh";

struct LevelCase {
	std::unique_ptr<level::LoadedLevel> level;
	std::unique_ptr<level::World> world;
	level::MeshCensus census;
};

void load_case(bf3::test::Mounted &r_m, const char *p_level, const char *p_mode, LevelCase &r_out) {
	Result<std::unique_ptr<level::LoadedLevel>> loaded = level::load_level(r_m.install, p_level, p_mode);
	REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
	r_out.level = std::move(*loaded);
	Result<std::unique_ptr<level::World>> world = level::World::traverse(*r_out.level);
	REQUIRE_MESSAGE(world.ok(), (world.ok() ? std::string() : world.error().message));
	r_out.world = std::move(*world);
	Result<level::MeshCensus> census = level::mesh_census(*r_out.level, *r_out.world);
	REQUIRE_MESSAGE(census.ok(), (census.ok() ? std::string() : census.error().message));
	r_out.census = std::move(*census);
}

LevelScenePlan plan_case(const LevelCase &p_case, SceneLook p_look) {
	ScenePlanOptions options;
	options.look = p_look;
	Result<LevelScenePlan> plan = plan_level_scene(*p_case.level, *p_case.world, p_case.census, options);
	REQUIRE_MESSAGE(plan.ok(), (plan.ok() ? std::string() : plan.error().message));
	return std::move(*plan);
}

size_t placed(const level::MeshCensus &p_census, std::string_view p_name) {
	size_t n = 0;
	for (const level::SceneMesh &m : p_census.meshes) {
		n += m.name == p_name ? m.placements : 0;
	}
	return n;
}

size_t kept(const level::MeshCensus &p_census, std::string_view p_name) {
	size_t n = 0;
	for (const level::SceneMesh &m : p_census.meshes) {
		n += m.name == p_name && m.kept ? m.placements : 0;
	}
	return n;
}

size_t drawn_instances(const LevelScenePlan &p_plan, const level::MeshCensus &p_census, std::string_view p_name) {
	size_t n = 0;
	for (const SceneInstancePlan &inst : p_plan.instances) {
		if (inst.mesh >= p_plan.meshes.size()) {
			continue;
		}
		const SceneMeshPlan &mesh = p_plan.meshes[inst.mesh];
		bool drawn = false;
		for (const uint32_t material : mesh.surface_materials) {
			drawn = drawn || material != kSceneUndrawnMaterial;
		}
		n += drawn && p_census.meshes[mesh.census_mesh].name == p_name ? 1 : 0;
	}
	return n;
}

const LookMaterial *first_look(
		const LevelScenePlan &p_plan, const level::MeshCensus &p_census, std::string_view p_name) {
	for (const SceneMeshPlan &mesh : p_plan.meshes) {
		if (p_census.meshes[mesh.census_mesh].name != p_name) {
			continue;
		}
		for (const uint32_t material : mesh.surface_materials) {
			if (material != kSceneUndrawnMaterial && p_plan.materials[material].look) {
				return &*p_plan.materials[material].look;
			}
		}
	}
	return nullptr;
}

void check_install_unchanged(const bf3::test::Mounted &p_m) {
	const SnapshotDiff diff = diff_snapshots(p_m.before, p_m.reader.snapshot());
	std::printf("install.unchanged %s\n", diff.clean() ? "PASS" : diff.summary().c_str());
	CHECK_MESSAGE(diff.clean(), diff.summary());
}

}

TEST_SUITE("render-metro-light") {
	TEST_CASE("render.metro_light: Metro keeps and draws its tunnel shadow mesh and its 56 pixel occluders") {
		bf3::test::Mounted *m = bf3::test::shared_mount_or_skip();
		if (m == nullptr) {
			return;
		}
		LevelCase metro;
		load_case(*m, "MP_Subway", "ConquestLarge0", metro);
		const LevelScenePlan plan = plan_case(metro, SceneLook::L1);
		const LevelScenePlan plain = plan_case(metro, SceneLook::M1);
		std::printf("render.metro_light blockers: shadow mesh placed %zu kept %zu drawn %zu (plain look %zu); pixel "
					"occluders "
					"placed %zu kept %zu drawn %zu (plain look %zu)\n",
				placed(metro.census, kShadowMesh), kept(metro.census, kShadowMesh),
				drawn_instances(plan, metro.census, kShadowMesh), drawn_instances(plain, metro.census, kShadowMesh),
				placed(metro.census, kPixelOccluder), kept(metro.census, kPixelOccluder),
				drawn_instances(plan, metro.census, kPixelOccluder),
				drawn_instances(plain, metro.census, kPixelOccluder));
		CHECK(placed(metro.census, kShadowMesh) == 1);
		CHECK(kept(metro.census, kShadowMesh) == 1);
		CHECK(drawn_instances(plan, metro.census, kShadowMesh) == 1);
		CHECK(drawn_instances(plain, metro.census, kShadowMesh) == 1);
		CHECK(placed(metro.census, kPixelOccluder) == 56);
		CHECK(kept(metro.census, kPixelOccluder) == 56);
		CHECK(drawn_instances(plan, metro.census, kPixelOccluder) == 56);
		CHECK(drawn_instances(plain, metro.census, kPixelOccluder) == 56);
		CHECK(metro.census.failures.empty());
		const LookMaterial *shadow = first_look(plan, metro.census, kShadowMesh);
		const LookMaterial *occluder = first_look(plan, metro.census, kPixelOccluder);
		REQUIRE(shadow != nullptr);
		REQUIRE(occluder != nullptr);
		REQUIRE(shadow->flat_colour.has_value());
		REQUIRE(occluder->flat_colour.has_value());
		std::printf(
				"render.metro_light blocker looks: shadow mesh flat (%.3f %.3f %.3f) unlit %d double-sided %d; pixel "
				"occluder flat (%.3f %.3f %.3f) unlit %d double-sided %d\n",
				double((*shadow->flat_colour)[0]), double((*shadow->flat_colour)[1]), double((*shadow->flat_colour)[2]),
				int(shadow->flat_unlit), int(shadow->double_sided), double((*occluder->flat_colour)[0]),
				double((*occluder->flat_colour)[1]), double((*occluder->flat_colour)[2]), int(occluder->flat_unlit),
				int(occluder->double_sided));
		CHECK(*shadow->flat_colour == std::array<float, 3>{ 0.0f, 0.0f, 0.0f });
		CHECK(!shadow->flat_unlit);
		CHECK(shadow->double_sided);
		CHECK(!shadow->color);
		CHECK(*occluder->flat_colour == std::array<float, 3>{ 0.014f, 0.007f, 0.003f });
		CHECK(occluder->flat_unlit);
		CHECK(!occluder->color);
		size_t flat_lit = 0, flat_unlit = 0;
		for (const SceneMaterialPlan &material : plan.materials) {
			if (material.look && material.look->flat_colour) {
				++(material.look->flat_unlit ? flat_unlit : flat_lit);
				std::printf("render.metro_light flat material %s: (%.3f %.3f %.3f) %s\n", material.slot.shader.c_str(),
						double((*material.look->flat_colour)[0]), double((*material.look->flat_colour)[1]),
						double((*material.look->flat_colour)[2]), material.look->flat_unlit ? "unlit" : "lit");
			}
		}
		std::printf("render.metro_light flat materials: %zu lit, %zu unlit of %zu\n", flat_lit, flat_unlit,
				plan.materials.size());
		CHECK(flat_lit == 2);
		CHECK(flat_unlit == 1);
		check_install_unchanged(*m);
	}

	TEST_CASE("render.metro_light: the occluder walls of Grand Bazaar and Epicenter are drawn") {
		bf3::test::Mounted *m = bf3::test::shared_mount_or_skip();
		if (m == nullptr) {
			return;
		}
		LevelCase bazaar, quake;
		load_case(*m, "MP_001", "ConquestLarge0", bazaar);
		load_case(*m, "XP4_Quake", "ConquestLarge0", quake);
		const LevelScenePlan bazaar_plan = plan_case(bazaar, SceneLook::L1);
		const LevelScenePlan quake_plan = plan_case(quake, SceneLook::L1);
		std::printf("render.metro_light occluder walls: MP_001 placed %zu kept %zu drawn %zu; XP4_Quake placed %zu "
					"kept %zu "
					"drawn %zu\n",
				placed(bazaar.census, kBazaarOccluderWall), kept(bazaar.census, kBazaarOccluderWall),
				drawn_instances(bazaar_plan, bazaar.census, kBazaarOccluderWall),
				placed(quake.census, kQuakeOccluderWall), kept(quake.census, kQuakeOccluderWall),
				drawn_instances(quake_plan, quake.census, kQuakeOccluderWall));
		CHECK(placed(bazaar.census, kBazaarOccluderWall) == 7);
		CHECK(kept(bazaar.census, kBazaarOccluderWall) == 7);
		CHECK(drawn_instances(bazaar_plan, bazaar.census, kBazaarOccluderWall) == 7);
		CHECK(placed(quake.census, kQuakeOccluderWall) == 12);
		CHECK(kept(quake.census, kQuakeOccluderWall) == 12);
		CHECK(drawn_instances(quake_plan, quake.census, kQuakeOccluderWall) == 12);
		const LookMaterial *bazaar_wall = first_look(bazaar_plan, bazaar.census, kBazaarOccluderWall);
		const LookMaterial *quake_wall = first_look(quake_plan, quake.census, kQuakeOccluderWall);
		REQUIRE(bazaar_wall != nullptr);
		REQUIRE(quake_wall != nullptr);
		CHECK(bazaar_wall->color.has_value());
		CHECK(!bazaar_wall->flat_colour);
		CHECK(quake_wall->color.has_value());
		CHECK(!quake_wall->flat_colour);
		check_install_unchanged(*m);
	}

	TEST_CASE(
			"render.metro_light: a visual environment whose data says Enabled false is still the level's environment") {
		bf3::test::Mounted *m = bf3::test::shared_mount_or_skip();
		if (m == nullptr) {
			return;
		}
		struct Tally {
			size_t environments = 0, disabled = 0, switches = 0;
		};
		const auto tally = [](const level::World &p_world) {
			Tally out;
			const int32_t enabled_field = level::field_id("Enabled");
			for (const level::PlacedObject &object : p_world.objects) {
				if (object.kind != level::ObjectKind::VisualEnvironment ||
						!object.entity.is_a("VisualEnvironmentEntityData")) {
					continue;
				}
				++out.environments;
				const std::optional<EbxValue> enabled = object.entity.get("Enabled");
				if (!enabled || enabled->as_bool() != false) {
					continue;
				}
				++out.disabled;
				const level::Connections &connections = p_world.scopes.connections(object.scope);
				const auto targets = [&](const level::Connection &p_c) {
					return p_c.target == object.entity || (object.reference && p_c.target == *object.reference);
				};
				for (const level::Connection &c : connections.property) {
					out.switches += targets(c) && c.target_field == enabled_field ? 1 : 0;
				}
				for (const level::Connection &c : connections.event) {
					out.switches += targets(c) ? 1 : 0;
				}
			}
			return out;
		};
		LevelCase canals, metro;
		load_case(*m, "MP_017", "ConquestLarge0", canals);
		load_case(*m, "MP_Subway", "ConquestLarge0", metro);
		const Tally canals_tally = tally(*canals.world), metro_tally = tally(*metro.world);
		const LevelScenePlan plan = plan_case(canals, SceneLook::L1);
		const LevelScenePlan metro_plan = plan_case(metro, SceneLook::L1);
		size_t city_areas = 0;
		for (const LookArea &area : metro_plan.look_environment.areas) {
			city_areas += area.name == "levels/mp_subway/lighting/ve_mp_subway_city_01" ? 1 : 0;
		}
		std::printf("render.metro_light Enabled false: MP_017 %zu of %zu visual environments, %zu Enabled or event "
					"connections "
					"into them, look base %s, plain environment from data %d; MP_Subway %zu of %zu, %zu connections, "
					"city_01 an "
					"area %zu\n",
				canals_tally.disabled, canals_tally.environments, canals_tally.switches,
				plan.look_environment.base.source.substr(0, 64).c_str(), int(plan.environment.from_data),
				metro_tally.disabled, metro_tally.environments, metro_tally.switches, city_areas);
		CHECK(canals_tally.environments == 2);
		CHECK(canals_tally.disabled == 1);
		CHECK(canals_tally.switches == 0);
		CHECK(plan.look_environment.from_data);
		CHECK(plan.look_environment.base.source.find("OutdoorLightComponentData<levels/mp_017/lighting/ve_mp_017_02") !=
				std::string::npos);
		CHECK(plan.environment.from_data);
		CHECK(metro_tally.environments == 5);
		CHECK(metro_tally.disabled == 1);
		CHECK(metro_tally.switches == 0);
		CHECK(city_areas == 1);
		check_install_unchanged(*m);
	}
}
