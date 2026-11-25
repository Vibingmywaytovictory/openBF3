#include <doctest/doctest.h>

#include "bf3/core/level/census.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "bf3/core/util/text.h"
#include "bf3/render/level_scene.h"
#include "bf3/render/look_environment.h"
#include "bf3/render/tree_wind.h"
#include "test_support.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace bf3;
using core::Vec3;
namespace vec = core::vec;
using render::TreeRig;

namespace {

std::shared_ptr<const TreeRig> chain_rig() {
	TreeRig r;
	r.stem_bones = 3;
	const std::vector<int32_t> parents = { -1, 0, 1, 2, 2 };
	const std::vector<Vec3> offsets = { { 0, 0, 0 }, { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 }, { 1, 0.5f, 0 } };
	for (size_t i = 0; i < parents.size(); ++i) {
		core::Transform local = core::Transform::from_translation(offsets[i]);
		r.parents.push_back(parents[i]);
		r.rest.push_back(i == 0 ? local : r.rest[size_t(parents[i])].then_inner(local));
		r.length.push_back(i == 0 ? 0.0f : vec::length(offsets[i]));
		r.height.push_back(r.rest.back().trans.y);
		r.stem.push_back(i < 3 ? 1 : 0);
		r.stiffness.push_back(i < 3 ? 4.0f * (1.0f - 0.8f * float(i + 1) / 3.0f) : 2.0f);
		r.wind.push_back(i < 3 ? 3.0f : 8.0f);
	}
	return std::make_shared<const TreeRig>(std::move(r));
}

float motion(render::TreeSim &r_sim, const Vec3 &p_force, int p_steps) {
	std::vector<Vec3> before = r_sim.positions();
	float most = 0.0f;
	for (int s = 0; s < p_steps; ++s) {
		r_sim.step(p_force);
		for (size_t i = 0; i < before.size(); ++i) {
			most = std::fmax(most, vec::length(vec::sub(r_sim.positions()[i], before[i])));
		}
		before = r_sim.positions();
	}
	return most;
}

struct Sway {
	float degrees = 0.0f;
	bool finite = true;
};

Sway sway(const std::shared_ptr<const TreeRig> &p_rig, const render::TreeWind &p_wind, int p_steps) {
	render::TreeSim sim(p_rig);
	Sway out;
	const TreeRig &r = sim.rig();
	for (int s = 0; s < p_steps; ++s) {
		sim.step(render::tree_gust(p_wind, Vec3{}, float(s) * render::kTreeStep));
		for (size_t i = 1; i < r.bones(); ++i) {
			const Vec3 &x = sim.positions()[i];
			out.finite = out.finite && std::isfinite(x.x) && std::isfinite(x.y) && std::isfinite(x.z);
			const Vec3 bone = vec::sub(x, sim.positions()[size_t(r.parents[i])]);
			const Vec3 rest = vec::sub(r.rest[i].trans, r.rest[size_t(r.parents[i])].trans);
			if (s >= int(render::kTreePreSimulation / render::kTreeStep) && vec::length(bone) > 1e-6f &&
					vec::length(rest) > 1e-6f) {
				const float c = std::clamp(vec::dot(vec::normalize(bone), vec::normalize(rest)), -1.0f, 1.0f);
				out.degrees = std::fmax(out.degrees, std::acos(c) / core::kDegToRadF);
			}
		}
	}
	return out;
}
}

TEST_SUITE("render-trees-unit") {
	TEST_CASE("render.trees.unit: the level wind's gust, its clamp and the wind without a visual environment") {
		const render::VegetationSystemSettings settings;
		const render::TreeWind wind = render::tree_wind(settings, render::LevelWind{ 91.485f, 2.0f });
		CHECK(wind.direction.x == doctest::Approx(-0.0259f).epsilon(0.002));
		CHECK(wind.direction.y == 0.0f);
		CHECK(wind.direction.z == doctest::Approx(0.99966f).epsilon(1e-4));
		CHECK(wind.rate == doctest::Approx(0.1f));
		CHECK(wind.variation == doctest::Approx(1.1f));
		const Vec3 f = render::tree_gust(wind, Vec3{}, 0.0f);
		CHECK(f.x == doctest::Approx(-0.00044f).epsilon(0.02));
		CHECK(f.y == 0.0f);
		CHECK(f.z == doctest::Approx(0.01679f).epsilon(0.001));
		CHECK(vec::length(f) == doctest::Approx(0.0168f).epsilon(0.001));

		render::TreeWind unit_rate = wind;
		unit_rate.rate = 1.0f;
		float least = 1.0f, most = 0.0f;
		for (int k = 0; k < 200000; ++k) {
			const float g = vec::length(render::tree_gust(unit_rate, Vec3{}, float(k) * 0.001f));
			least = std::fmin(least, g);
			most = std::fmax(most, g);
		}
		CHECK(least == doctest::Approx(0.01f * 0.004f * 2.0f).epsilon(1e-3));
		CHECK(most <= 6.5f * 0.004f * 2.0f + 1e-6f);
		CHECK(most > 0.04f);

		const render::TreeWind none = render::tree_wind(settings, std::nullopt);
		CHECK(none.direction == render::kTreeDefaultWindDirection);
		CHECK(none.strength == 2.0f);
		CHECK(none.rate == doctest::Approx(0.1f));

		core::level::LevelTransform turned;
		turned.right = { 0, 0, -1 };
		turned.forward = { 1, 0, 0 };
		const Vec3 local = render::tree_local_force(turned, Vec3{ 1, 0, 0 });
		CHECK(local.x == doctest::Approx(0.0f));
		CHECK(local.z == doctest::Approx(1.0f));
	}

	TEST_CASE("render.trees.unit: the fixed 0.015 s step, the 0.03 s frame cap and the 2 s pre-simulation") {
		const render::TreeWind wind = render::tree_wind({}, render::LevelWind{ 91.485f, 2.0f });
		render::TreeForest forest({ chain_rig() }, { render::TreePlacement{ 0, {} } });
		forest.frame(0.1f, Vec3{}, wind);
		CHECK(forest.steps() == 135);
		CHECK(forest.clock() == doctest::Approx(0.03f));
		forest.frame(0.1f, Vec3{ 1, 0, 0 }, wind);
		CHECK(forest.steps() == 137);
		forest.frame(0.1f, Vec3{ 300, 0, 0 }, wind);
		CHECK(forest.steps() == 139);
		CHECK(forest.active() == 1);
		for (int k = 0; k < 10; ++k) {
			forest.frame(0.1f, Vec3{ 300, 0, 0 }, wind);
		}
		CHECK(forest.active() == 0);
		CHECK_FALSE(forest.stepped(0));

		render::TreeForest far({ chain_rig() }, { render::TreePlacement{ 0, {} } });
		far.frame(0.1f, Vec3{ 251, 0, 0 }, wind);
		CHECK(far.sim(0) == nullptr);
		far.frame(0.1f, Vec3{ 249, 0, 0 }, wind);
		REQUIRE(far.sim(0) != nullptr);
		CHECK(far.steps() == 2);
		far.frame(0.016f, Vec3{ 248, 0, 0 }, wind);
		CHECK(far.steps() == 3);
		far.frame(0.01f, Vec3{ 248, 0, 0 }, wind);
		CHECK(far.steps() == 3);
	}

	TEST_CASE("render.trees.unit: a tree settles still without wind and keeps moving downwind with it") {
		render::TreeSim calm(chain_rig());
		motion(calm, Vec3{}, 4000);
		CHECK(motion(calm, Vec3{}, 200) < 1e-5f);
		for (size_t i = 1; i < calm.positions().size(); ++i) {
			const Vec3 bone = vec::sub(calm.positions()[i], calm.positions()[size_t(calm.rig().parents[i])]);
			CHECK(vec::length(bone) == doctest::Approx(calm.rig().length[i]).epsilon(1e-4));
		}

		const render::TreeWind wind = render::tree_wind({}, render::LevelWind{ 0.0f, 2.0f });
		render::TreeSim windy(chain_rig());
		float most = 0.0f, downwind = 0.0f;
		for (int s = 0; s < 4000; ++s) {
			const Vec3 f = render::tree_gust(wind, Vec3{}, float(s) * render::kTreeStep);
			most = std::fmax(most, motion(windy, f, 1));
			downwind += vec::sub(windy.positions()[2], calm.positions()[2]).x;
		}
		CHECK(most > 1e-4f);
		CHECK(downwind > 0.0f);

		const std::vector<core::Transform> offsets = windy.bone_offsets();
		REQUIRE(offsets.size() == windy.positions().size());
		for (size_t i = 0; i < offsets.size(); ++i) {
			const Vec3 at = offsets[i].transform_point(windy.rig().rest[i].trans);
			CHECK(vec::length(vec::sub(at, windy.positions()[i])) < 1e-4f);
			CHECK(offsets[i].determinant() == doctest::Approx(1.0f).epsilon(1e-4));
		}
	}
}

TEST_SUITE("render-trees") {
	TEST_CASE("render.trees: MP_007's, MP_Subway's and MP_001's VegetationTree bones move with their level's wind and "
			  "stay finite over three minutes") {
		test::Mounted m;
		if (!test::open_or_skip(m)) {
			return;
		}
		struct Case {
			const char *level;
			float direction;
			float strength;
			bool branches_below_stem_top;
		};
		for (const Case &c : { Case{ "MP_007", 91.485f, 2.0f, true }, Case{ "MP_Subway", 113.548f, 2.0f, true },
					 Case{ "MP_001", 37.593f, 10.0f, false } }) {
			core::Result<std::unique_ptr<core::level::LoadedLevel>> loaded =
					core::level::load_level(m.install, c.level, "TeamDeathMatchC0");
			REQUIRE(loaded.ok());
			core::Result<std::unique_ptr<core::level::World>> world = core::level::World::traverse(**loaded);
			REQUIRE(world.ok());
			core::Result<render::LookEnvironment> look = render::read_look_environment((*loaded)->db(), **world);
			REQUIRE(look.ok());
			REQUIRE(look->base.wind);
			CHECK(look->base.wind_direction == doctest::Approx(c.direction));
			CHECK(look->base.wind_strength == doctest::Approx(c.strength));

			const render::LevelTrees trees = render::level_trees(**world);
			for (const std::string &f : trees.failures) {
				std::printf("render.trees %s rig failure: %s\n", c.level, f.c_str());
			}
			CHECK(trees.failures.empty());
			REQUIRE(!trees.trees.empty());
			size_t chained = 0, decisive = 0;
			for (const std::shared_ptr<const TreeRig> &rig : trees.rigs) {
				REQUIRE(rig != nullptr);
				REQUIRE(rig->stem_bones <= rig->bones());
				if (rig->stem_bones == rig->bones()) {
					continue;
				}
				bool chain = true;
				for (size_t i = 1; i < rig->stem_bones; ++i) {
					chain = chain && rig->parents[i] == int32_t(i) - 1;
				}
				CHECK_MESSAGE(chain, "EBX bones 0..StemBoneCount-1 are not one chain");
				chained += chain ? 1 : 0;
				decisive += rig->parents[rig->stem_bones] != int32_t(rig->stem_bones) - 1 ? 1 : 0;
			}
			const render::TreeWind wind =
					render::tree_wind({}, render::LevelWind{ look->base.wind_direction, look->base.wind_strength });
			const render::TreeWind calm_wind =
					render::tree_wind({}, render::LevelWind{ look->base.wind_direction, 0.0f });
			const Vec3 camera = trees.trees.front().transform.trans;
			render::TreeForest forest(trees.rigs, trees.trees);
			render::TreeForest calm(trees.rigs, trees.trees);
			double downwind = 0.0, most = 0.0;
			size_t measured = 0;
			for (int frame = 0; frame < 600; ++frame) {
				forest.frame(1.0f / 60.0f, camera, wind);
				calm.frame(1.0f / 60.0f, camera, calm_wind);
				if (frame < 300) {
					continue;
				}
				for (size_t t = 0; t < forest.trees(); ++t) {
					const render::TreeSim *s = forest.sim(t);
					const render::TreeSim *q = calm.sim(t);
					if (s == nullptr || q == nullptr) {
						continue;
					}
					const core::level::LevelTransform &x = forest.placement(t).transform;
					for (size_t i = 1; i < s->positions().size(); ++i) {
						const Vec3 d = vec::sub(s->positions()[i], q->positions()[i]);
						const Vec3 world_d = vec::add(vec::add(vec::scale(vec::normalize(x.right), d.x),
															  vec::scale(vec::normalize(x.up), d.y)),
								vec::scale(vec::normalize(x.forward), d.z));
						downwind += vec::dot(world_d, wind.direction);
						most = std::fmax(most, double(vec::length(d)));
						++measured;
					}
				}
			}
			const double mean = measured > 0 ? downwind / double(measured) : 0.0;
			std::printf("render.trees %s: %zu trees of %zu rigs (%zu with a stem chain EBX 0..StemBoneCount-1, %zu "
						"whose next bone "
						"branches lower), %zu "
						"active at %.1f %.1f %.1f, %llu steps; wind %.3f deg at %.2f; mean downwind bone offset "
						"%.4f m against the calm run, largest %.4f m\n",
					c.level, trees.trees.size(), trees.rigs.size(), chained, decisive, forest.active(), camera.x,
					camera.y, camera.z, (unsigned long long)forest.steps(), double(look->base.wind_direction),
					double(look->base.wind_strength), mean, most);
			CHECK((decisive > 0 || !c.branches_below_stem_top));
			for (const std::shared_ptr<const TreeRig> &rig : trees.rigs) {
				const Sway s = sway(rig, wind, 12000);
				const Sway still = sway(rig, calm_wind, 12000);
				std::printf("render.trees %s rig of %zu bones: largest bone turn from its base pose after its 2 s "
							"pre-simulation, over 180 s, %.1f "
							"degrees, %.1f without wind\n",
						c.level, rig->bones(), double(s.degrees), double(still.degrees));
				CHECK(s.finite);
				CHECK(s.degrees > 0.0f);
			}
			CHECK(forest.active() > 0);
			CHECK(forest.steps() > 0);
			CHECK(measured > 0);
			CHECK(mean > 1e-3);
			CHECK(most > 1e-2);
		}
		const core::SnapshotDiff diff = core::diff_snapshots(m.before, m.reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}

	TEST_CASE("render.trees: treelinden_l_01's stiffness and wind weights per bone follow the stem taper and the "
			  "branch levels") {
		test::Mounted m;
		if (!test::open_or_skip(m)) {
			return;
		}
		core::Result<std::unique_ptr<core::level::LoadedLevel>> loaded =
				core::level::load_level(m.install, "MP_007", "TeamDeathMatchC0");
		REQUIRE(loaded.ok());
		core::Result<std::unique_ptr<core::level::World>> world = core::level::World::traverse(**loaded);
		REQUIRE(world.ok());
		const render::LevelTrees trees = render::level_trees(**world);
		std::shared_ptr<const TreeRig> linden;
		for (size_t t = 0; t < trees.trees.size() && linden == nullptr; ++t) {
			const core::level::PlacedObject &o = (*world)->objects[trees.objects[t]];
			const std::optional<std::string_view> name = (*loaded)->db().name_of(o.entity.partition().guid());
			if (name && core::to_lower_ascii(*name) == "objects/vegetation/treelinden_l_01/treelinden_l_01") {
				linden = trees.rigs[trees.trees[t].rig];
			}
		}
		REQUIRE(linden != nullptr);
		REQUIRE(linden->bones() == 34);
		CHECK(linden->stem_bones == 18);
		CHECK(linden->stiffness[0] == doctest::Approx(3.822f).epsilon(1e-3));
		CHECK(linden->stiffness[8] == doctest::Approx(2.4f));
		CHECK(linden->stiffness[17] == doctest::Approx(0.8f));
		CHECK(linden->wind[17] == 3.0f);
		CHECK(linden->parents[18] == 13);
		CHECK(linden->stiffness[18] == 2.0f);
		CHECK(linden->wind[18] == 8.0f);
		CHECK(linden->parents[19] == 18);
		CHECK(linden->stiffness[19] == 1.5f);
		CHECK(linden->wind[19] == 12.0f);
		CHECK(linden->length[1] == doctest::Approx(0.38586882f));
		CHECK(linden->rest[1].trans.y == doctest::Approx(-0.38586882f));
		const core::SnapshotDiff diff = core::diff_snapshots(m.before, m.reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}

	TEST_CASE("render.trees: MP_007's scene plan skins every VegetationTree instance's mesh to its own tree bones") {
		test::Mounted m;
		if (!test::open_or_skip(m)) {
			return;
		}
		core::Result<std::unique_ptr<core::level::LoadedLevel>> loaded =
				core::level::load_level(m.install, "MP_007", "TeamDeathMatchC0");
		REQUIRE(loaded.ok());
		core::Result<std::unique_ptr<core::level::World>> world = core::level::World::traverse(**loaded);
		REQUIRE(world.ok());
		core::Result<core::level::MeshCensus> census = core::level::mesh_census(**loaded, **world);
		REQUIRE(census.ok());
		core::Result<render::LevelScenePlan> plan = render::plan_level_scene(**loaded, **world, *census);
		REQUIRE(plan.ok());
		REQUIRE(!plan->trees.trees.empty());
		std::vector<uint32_t> requests;
		std::vector<uint32_t> rig_of_request;
		size_t tree_instances = 0;
		for (const render::SceneInstancePlan &inst : plan->instances) {
			if (inst.tree == render::kNoSceneTree) {
				continue;
			}
			++tree_instances;
			REQUIRE(inst.tree < plan->trees.trees.size());
			REQUIRE(inst.mesh < plan->meshes.size());
			const uint32_t r = plan->meshes[inst.mesh].request;
			CHECK(plan->requests[r].request.own_skeleton);
			if (std::find(requests.begin(), requests.end(), r) == requests.end()) {
				requests.push_back(r);
				rig_of_request.push_back(plan->trees.trees[inst.tree].rig);
			}
		}
		CHECK(tree_instances == plan->trees.trees.size());
		size_t skinned_vertices = 0;
		for (size_t k = 0; k < requests.size(); ++k) {
			core::Result<render::CachedMesh> mesh =
					render::load_scene_request(nullptr, (*loaded)->mount(), *plan, requests[k]);
			REQUIRE_MESSAGE(mesh.ok(), (mesh.ok() ? std::string() : mesh.error().message));
			REQUIRE(!mesh->mesh.meshes.empty());
			const render::PreparedMesh &lod0 = mesh->mesh.meshes.front();
			CHECK(lod0.skinned);
			const size_t bones = plan->trees.rigs[rig_of_request[k]]->bones();
			for (const render::PreparedSurface &s : lod0.surfaces) {
				REQUIRE(s.bones.size() == s.vertex_count() * 4);
				for (size_t v = 0; v < s.vertex_count(); ++v) {
					float sum = 0.0f;
					for (size_t l = 0; l < 4; ++l) {
						CHECK(size_t(s.bones[v * 4 + l]) < bones);
						sum += s.weights[v * 4 + l];
					}
					CHECK(sum == doctest::Approx(1.0f).epsilon(1e-4));
				}
				skinned_vertices += s.vertex_count();
			}
		}
		std::printf("render.trees MP_007 plan: %zu tree instances of %zu rigs through %zu meshes, %zu skinned lod 0 "
					"vertices\n",
				tree_instances, plan->trees.rigs.size(), requests.size(), skinned_vertices);
		CHECK(skinned_vertices > 0);
		const core::SnapshotDiff diff = core::diff_snapshots(m.before, m.reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}
}
