#include "bf3/core/level/level.h"
#include "bf3/core/level/placed_effects.h"
#include "bf3/core/level/world.h"
#include "bf3/fx/effect_compiler.h"
#include "bf3/fx/placed_effects.h"
#include "bf3/fx/render_prep.h"
#include "bf3/fx/simulator.h"
#include "fx_test_support.h"
#include "test_support.h"

#include <doctest/doctest.h>

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace bf3;
using namespace bf3::core;

namespace {

fx::Transform at(float p_x, float p_y, float p_z) {
	fx::Transform t;
	t.trans = { p_x, p_y, p_z };
	return t;
}

fx::FxView view_at(float p_x, float p_y, float p_z) {
	fx::FxView v;
	v.camera_position = { p_x, p_y, p_z };
	return v;
}

std::shared_ptr<const fx::EffectDef> looping_effect(float p_cull_distance) {
	fx::EmitterDef e = fxt::emitter_def("loop", 8, 4.0f, 1.0f);
	e.tmpl.flags |= fx::kEmitterRepeatParticleSpawning;
	e.tmpl.lifetime = 1.0f;
	std::shared_ptr<const fx::EffectDef> shared = fxt::effect_def("placed/loop", { e });
	fx::EffectDef def = *shared;
	def.cull_distance = p_cull_distance;
	return std::make_shared<const fx::EffectDef>(std::move(def));
}

struct PlacedCounts {
	size_t placed = 0;
	size_t auto_start = 0;
	size_t blueprints = 0;
};

void check_placed_effects(const char *p_level, const char *p_mode, const PlacedCounts &p_data) {
	test::Mounted *m = test::shared_mount_or_skip();
	if (m == nullptr) {
		return;
	}
	Result<std::unique_ptr<level::LoadedLevel>> loaded = level::load_level(m->install, p_level, p_mode);
	REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
	const level::LoadedLevel &lvl = **loaded;
	Result<std::unique_ptr<level::World>> world = level::World::traverse(lvl);
	REQUIRE_MESSAGE(world.ok(), (world.ok() ? std::string() : world.error().message));
	Result<std::vector<level::PlacedEffect>> placed = level::read_placed_effects(**world);
	REQUIRE_MESSAGE(placed.ok(), (placed.ok() ? std::string() : placed.error().message));

	size_t effect_objects = 0;
	for (const level::PlacedObject &o : (*world)->objects) {
		effect_objects += o.kind == level::ObjectKind::Effect ? 1 : 0;
	}
	std::map<std::string, std::shared_ptr<const fx::EffectDef>> defs;
	fx::PlacedEffects effects;
	size_t auto_start = 0, mesh_emitters = 0;
	for (const level::PlacedEffect &e : *placed) {
		auto found = defs.find(e.blueprint);
		if (found == defs.end()) {
			fx::CompileContext ctx;
			ctx.db = &lvl.db();
			Result<fx::EffectDef> def = fx::compile_effect(e.blueprint, ctx);
			REQUIRE_MESSAGE(def.ok(), e.blueprint << ": " << (def.ok() ? std::string() : def.error().message));
			found = defs.emplace(e.blueprint, std::make_shared<const fx::EffectDef>(std::move(*def))).first;
		}
		mesh_emitters += fx::mesh_emitter_count(*found->second);
		auto_start += e.auto_start ? 1 : 0;
		fx::Transform t;
		t.right = e.transform.right;
		t.up = e.transform.up;
		t.forward = e.transform.forward;
		t.trans = e.transform.trans;
		effects.add(found->second, t, e.auto_start);
	}
	std::printf(
			"%s %s: %zu effect objects, %zu placed effects of %zu blueprints, %zu start at load, %zu mesh emitters\n",
			p_level, p_mode, effect_objects, placed->size(), defs.size(), auto_start, mesh_emitters);
	CHECK(placed->size() == effect_objects);
	CHECK(placed->size() == p_data.placed);
	CHECK(auto_start == p_data.auto_start);
	CHECK(defs.size() == p_data.blueprints);

	fx::FxWorld fx_world;
	for (size_t i = 0; i < placed->size(); ++i) {
		const level::PlacedEffect &e = (*placed)[i];
		if (!e.auto_start) {
			continue;
		}
		fx::FxView view;
		view.camera_position = e.transform.trans;
		effects.step(fx_world, view, fx::kTierHigh, 7);
		const fx::PlacedEffectStats &s = effects.stats();
		CHECK(s.running + s.ended_in_range == s.in_range);
		CHECK(s.start_failures == 0);
		fx_world.step(1.0f / 60.0f, view);
	}
	CHECK(effects.stats().without_def == 0);
	CHECK(effects.stats().started_placements == p_data.auto_start);
}

}

TEST_SUITE("placed-effects-unit") {
	TEST_CASE("placed effects: every placement keeps its own instance of one blueprint") {
		const std::shared_ptr<const fx::EffectDef> def = looping_effect(0.0f);
		REQUIRE(def->max_instance_count == 1);
		fx::PlacedEffects placed;
		placed.add(def, at(0, 0, 0), true);
		placed.add(def, at(50, 0, 0), true);
		placed.add(def, at(100, 0, 0), false);
		placed.add(nullptr, at(150, 0, 0), true);
		fx::FxWorld world;
		const fx::FxView view = view_at(0, 0, 0);
		for (int i = 0; i < 30; ++i) {
			placed.step(world, view, fx::kTierHigh, 1);
			world.step(1.0f / 30.0f, view);
		}
		const fx::PlacedEffectStats &s = placed.stats();
		CHECK(s.placed == 4);
		CHECK(s.auto_start == 3);
		CHECK(s.without_def == 1);
		CHECK(s.in_range == 2);
		CHECK(s.running == 2);
		CHECK(s.starts == 2);
		CHECK(s.start_failures == 0);
		CHECK(world.stats().live_effects == 2);
		CHECK(world.stats().stolen_instances == 0);
	}

	TEST_CASE("placed effects: an effect runs while the camera is inside its cull distance") {
		fx::PlacedEffects placed;
		placed.add(looping_effect(75.0f), at(0, 0, 0), true);
		fx::FxWorld world;
		placed.step(world, view_at(200, 0, 0), fx::kTierHigh, 1);
		CHECK(placed.stats().in_range == 0);
		CHECK(placed.stats().running == 0);
		placed.step(world, view_at(74, 0, 0), fx::kTierHigh, 1);
		CHECK(placed.stats().in_range == 1);
		CHECK(placed.stats().running == 1);
		world.step(0.1f, view_at(74, 0, 0));
		placed.step(world, view_at(76, 0, 0), fx::kTierHigh, 1);
		CHECK(placed.stats().running == 0);
		CHECK(placed.stats().stops == 1);
		placed.step(world, view_at(10, 0, 0), fx::kTierHigh, 1);
		CHECK(placed.stats().running == 1);
		CHECK(placed.stats().starts == 2);
	}

	TEST_CASE("placed effects: a logic Start runs a waiting placement and a Stop ends it") {
		const std::shared_ptr<const fx::EffectDef> def = looping_effect(0.0f);
		fx::PlacedEffects placed;
		placed.add(def, at(0, 0, 0), false);
		placed.add(def, at(10, 0, 0), true);
		placed.add(def, at(20, 0, 0), false);
		const uint32_t nodes[] = { 7, 8, fx::kNoPlacedLogicNode };
		placed.bind_logic(nodes);
		CHECK(placed.stats().logic_bound == 2);
		fx::FxWorld world;
		const fx::FxView view = view_at(0, 0, 0);
		const auto run = [&](int p_frames) {
			for (int i = 0; i < p_frames; ++i) {
				placed.step(world, view, fx::kTierHigh, 1);
				world.step(1.0f / 30.0f, view);
			}
		};
		run(5);
		CHECK(placed.stats().running == 1);
		placed.logic_event(7, true);
		run(5);
		CHECK(placed.stats().running == 2);
		CHECK(placed.stats().logic_starts == 1);
		CHECK(placed.stats().logic_started_placements == 1);
		placed.logic_event(8, false);
		run(60);
		CHECK(placed.stats().running == 1);
		CHECK(placed.stats().logic_stops == 1);
		CHECK(world.stats().live_effects == 1);
		placed.logic_reset();
		run(5);
		CHECK(placed.stats().running == 1);
		CHECK(placed.stats().started_placements == 2);
		placed.logic_event(fx::kNoPlacedLogicNode, true);
		run(5);
		CHECK(placed.stats().running == 1);
	}

	TEST_CASE("placed effects: mesh particle emitters are counted and their meshes named once") {
		fx::EmitterDef quad = fxt::emitter_def("quad", 4, 2.0f, 1.0f);
		fx::EmitterDef mesh = fxt::emitter_def("mesh", 4, 2.0f, 1.0f);
		mesh.tmpl.emittable = fx::EmittableType::ParticleMesh;
		mesh.tmpl.mesh = "fx/meshparticles/a_mesh";
		const std::shared_ptr<const fx::EffectDef> def =
				fxt::effect_def("placed/mixed", { quad, mesh, mesh }, { 0, 1, 2 });
		CHECK(fx::mesh_emitter_count(*def) == 2);
		std::vector<std::string> names;
		fx::collect_mesh_names(*def, names);
		fx::collect_mesh_names(*def, names);
		REQUIRE(names.size() == 1);
		CHECK(names[0] == "fx/meshparticles/a_mesh");
		CHECK(fx::mesh_emitter_count(*fxt::effect_def("placed/quad", { quad })) == 0);
	}
}

TEST_SUITE("placed-effects") {
	TEST_CASE("placed effects: MP_007 ConquestLarge0 starts what the level places") {
		check_placed_effects("MP_007", "ConquestLarge0", PlacedCounts{ 338, 239, 30 });
	}

	TEST_CASE("placed effects: MP_Subway ConquestLarge0 starts what the level places") {
		check_placed_effects("MP_Subway", "ConquestLarge0", PlacedCounts{ 292, 255, 48 });
	}

	TEST_CASE("placed effects: MP_018 ConquestLarge0 starts what the level places") {
		check_placed_effects("MP_018", "ConquestLarge0", PlacedCounts{ 271, 204, 39 });
	}

	TEST_CASE("emitter exclusion volumes: MP_001's 34 rain occluders are read as boxes their centres are inside and a "
			  "point past each face is outside; Caspian and Metro place none") {
		test::Mounted *m = test::shared_mount_or_skip();
		if (m == nullptr) {
			return;
		}
		for (const auto &[name, expected] : { std::pair{ "MP_001", size_t(34) }, std::pair{ "MP_007", size_t(0) },
					 std::pair{ "MP_Subway", size_t(0) } }) {
			CAPTURE(name);
			Result<std::unique_ptr<level::LoadedLevel>> loaded = level::load_level(m->install, name, "ConquestLarge0");
			REQUIRE(loaded.ok());
			Result<std::unique_ptr<level::World>> world = level::World::traverse(**loaded);
			REQUIRE(world.ok());
			Result<std::vector<level::EmitterExclusionVolume>> volumes = level::read_emitter_exclusion_volumes(**world);
			REQUIRE_MESSAGE(volumes.ok(), (volumes.ok() ? std::string() : volumes.error().message));
			size_t inside = 0, outside = 0;
			for (const level::EmitterExclusionVolume &v : *volumes) {
				CHECK(v.half_extents.x > 0.0f);
				CHECK(v.half_extents.y > 0.0f);
				CHECK(v.half_extents.z > 0.0f);
				fx::FxExclusionVolume f;
				f.transform.right = v.transform.right;
				f.transform.up = v.transform.up;
				f.transform.forward = v.transform.forward;
				f.transform.trans = v.transform.trans;
				f.half_extents = v.half_extents;
				inside += fx::fx_inside_exclusion_volume(f, f.transform.trans) ? 1 : 0;
				const float k = 1.01f * v.half_extents.y;
				const fx::Vec3 above{ f.transform.trans.x + f.transform.up.x * k,
					f.transform.trans.y + f.transform.up.y * k, f.transform.trans.z + f.transform.up.z * k };
				outside += fx::fx_inside_exclusion_volume(f, above) ? 0 : 1;
			}
			std::printf("%s: %zu emitter exclusion volumes\n", name, volumes->size());
			CHECK(volumes->size() == expected);
			CHECK(inside == volumes->size());
			CHECK(outside == volumes->size());
		}
	}
}
