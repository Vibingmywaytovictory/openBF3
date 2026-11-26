#include <doctest/doctest.h>

#include "bf3/core/level/census.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "bf3/core/util/text.h"
#include "bf3/render/level_scene.h"
#include "bf3/render/look_environment.h"
#include "bf3/render/mesh_cache.h"
#include "bf3/render/tree_wind.h"
#include "test_support.h"

#include <algorithm>
#include <array>
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
	render::TreeRigData d;
	d.parents = { -1, 0, 1, 2, 2 };
	for (const Vec3 &offset :
			{ Vec3{ 0, 0, 0 }, Vec3{ 0, 1, 0 }, Vec3{ 0, 1, 0 }, Vec3{ 0, 1, 0 }, Vec3{ 1, 0.5f, 0 } }) {
		d.locals.push_back(core::Transform::from_translation(offset));
		d.translations.push_back(offset);
	}
	d.stem_bones = 3;
	d.stem_stiffness = 4.0f;
	d.stem_stiffness_spread = 0.2f;
	d.branch_stiffness = 2.0f;
	d.branch_stiffness_l2 = 2.0f;
	d.wind_stem = 3.0f;
	d.wind_branch = 8.0f;
	d.wind_branch_l2 = 8.0f;
	d.joint_threshold = 0.45f;
	core::Result<TreeRig> rig = render::build_tree_rig(d);
	REQUIRE(rig.ok());
	return std::make_shared<const TreeRig>(std::move(*rig));
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

double turn_degrees(const core::Transform &p_t) {
	const double c = (double(p_t.right.x) + double(p_t.up.y) + double(p_t.forward.z) - 1.0) * 0.5;
	const double sx = double(p_t.up.z) - double(p_t.forward.y);
	const double sy = double(p_t.forward.x) - double(p_t.right.z);
	const double sz = double(p_t.right.y) - double(p_t.up.x);
	return std::atan2(0.5 * std::sqrt(sx * sx + sy * sy + sz * sz), c) * 180.0 / 3.14159265358979323846;
}

std::vector<double> bone_turns(const render::TreeSim &p_sim) {
	std::vector<double> out;
	for (const core::Transform &t : p_sim.bone_offsets()) {
		out.push_back(turn_degrees(t));
	}
	return out;
}

struct Sway {
	double degrees = 0.0;
	bool finite = true;
};

Sway sway(const std::shared_ptr<const TreeRig> &p_rig, const render::TreeWind &p_wind, int p_steps) {
	render::TreeSim sim(p_rig);
	Sway out;
	for (int s = 0; s < p_steps; ++s) {
		sim.step(render::tree_gust(p_wind, Vec3{}, float(s) * render::kTreeStep));
		for (const Vec3 &x : sim.positions()) {
			out.finite = out.finite && std::isfinite(x.x) && std::isfinite(x.y) && std::isfinite(x.z);
		}
		if (s >= int(render::kTreePreSimulation / render::kTreeStep)) {
			for (const double t : bone_turns(sim)) {
				out.degrees = std::max(out.degrees, t);
			}
		}
	}
	return out;
}

struct BeechLevel {
	std::unique_ptr<core::level::LoadedLevel> loaded;
	std::unique_ptr<core::level::World> world;
	render::LevelTrees trees;
	std::shared_ptr<const TreeRig> beech;
};

BeechLevel load_mp007_trees(test::Mounted &r_m) {
	BeechLevel out;
	core::Result<std::unique_ptr<core::level::LoadedLevel>> loaded =
			core::level::load_level(r_m.install, "MP_007", "ConquestLarge0");
	REQUIRE(loaded.ok());
	out.loaded = std::move(*loaded);
	core::Result<std::unique_ptr<core::level::World>> world = core::level::World::traverse(*out.loaded);
	REQUIRE(world.ok());
	out.world = std::move(*world);
	out.trees = render::level_trees(*out.world);
	for (size_t t = 0; t < out.trees.trees.size() && out.beech == nullptr; ++t) {
		const core::level::PlacedObject &o = out.world->objects[out.trees.objects[t]];
		const std::optional<std::string_view> name = out.loaded->db().name_of(o.entity.partition().guid());
		if (name && core::to_lower_ascii(*name).find("treebeech_m_02_cluster") != std::string::npos) {
			out.beech = out.trees.rigs[out.trees.trees[t].rig];
		}
	}
	return out;
}

double length_error(const render::TreeSim &p_sim) {
	const TreeRig &r = p_sim.rig();
	double most = 0.0;
	for (size_t c = 1; c < r.bones(); ++c) {
		if (r.children[c].size() == 1) {
			const double reach = vec::length(vec::sub(p_sim.positions()[r.tips[c]], p_sim.positions()[c]));
			most = std::max(most, std::fabs(reach - double(r.length[c])));
		}
	}
	return most;
}

double relative_turn(const render::TreeSim &p_sim, size_t p_bone) {
	const std::vector<core::Transform> offsets = p_sim.bone_offsets();
	const size_t parent = size_t(p_sim.rig().parents[p_bone]);
	return turn_degrees(offsets[parent].inverse().then_inner(offsets[p_bone]));
}

void check_turns(const char *p_case, const std::vector<double> &p_turns, const std::array<double, 21> &p_expected) {
	REQUIRE(p_turns.size() == p_expected.size());
	for (size_t b = 1; b < p_turns.size(); ++b) {
		CHECK_MESSAGE(std::fabs(p_turns[b] - p_expected[b]) <= 0.015,
				std::string(p_case) << " bone " << b << ": " << p_turns[b] << " degrees, R6-VG2-vectors V3 "
									<< p_expected[b]);
	}
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
		turned.right = { 0, 0, -0.6f };
		turned.up = { 0, 0.6f, 0 };
		turned.forward = { 0.6f, 0, 0 };
		const Vec3 local = render::tree_local_force(turned, Vec3{ 1, 0, 0 });
		CHECK(local.x == doctest::Approx(1.0f / 0.6f));
		CHECK(local.y == 0.0f);
		CHECK(local.z == 0.0f);
		const core::Transform root = render::tree_root_frame(turned);
		CHECK(root.right == turned.right);
		CHECK(root.forward == turned.forward);
		CHECK(root.trans == Vec3{});
		const core::Transform back = root.inverse().then_inner(root);
		CHECK(vec::length(vec::sub(back.right, Vec3{ 1, 0, 0 })) < 1e-6f);
		CHECK(vec::length(vec::sub(back.up, Vec3{ 0, 1, 0 })) < 1e-6f);
		CHECK(vec::length(vec::sub(back.forward, Vec3{ 0, 0, 1 })) < 1e-6f);
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

		CHECK(forest.posed(0));

		render::TreeForest far({ chain_rig() }, { render::TreePlacement{ 0, {} } });
		far.frame(0.1f, Vec3{ 251, 0, 0 }, wind);
		CHECK(far.sim(0) == nullptr);
		CHECK_FALSE(far.posed(0));
		far.frame(0.1f, Vec3{ 249, 0, 0 }, wind);
		REQUIRE(far.sim(0) != nullptr);
		CHECK(far.posed(0));
		CHECK(far.steps() == 135);
		far.frame(0.016f, Vec3{ 248, 0, 0 }, wind);
		CHECK(far.steps() == 136);
		far.frame(0.005f, Vec3{ 248, 0, 0 }, wind);
		CHECK(far.steps() == 136);

		render::VegetationSystemSettings many_jobs;
		many_jobs.job_count = 10;
		const std::vector<render::TreePlacement> crowd(40, render::TreePlacement{ 0, {} });
		render::TreeForest busy({ chain_rig() }, crowd, many_jobs);
		busy.frame(0.015f, Vec3{ 251, 0, 0 }, wind);
		busy.frame(0.015f, Vec3{ 249, 0, 0 }, wind);
		size_t posed = 0, active = 0;
		for (size_t t = 0; t < busy.trees(); ++t) {
			posed += busy.posed(t) ? 1 : 0;
			active += busy.sim(t) != nullptr ? 1 : 0;
		}
		CHECK(active == 40);
		CHECK(posed == 32);
		CHECK(busy.steps() == 32 * 134 + 8);
		busy.frame(0.015f, Vec3{ 249, 0, 0 }, wind);
		posed = 0;
		for (size_t t = 0; t < busy.trees(); ++t) {
			posed += busy.posed(t) ? 1 : 0;
		}
		CHECK(posed == 40);
	}

	TEST_CASE("render.trees.unit: a tree settles still without wind and keeps moving downwind with it") {
		render::TreeSim calm(chain_rig());
		const TreeRig &chain = calm.rig();
		CHECK(chain.rigid == std::vector<uint8_t>{ 0, 0, 0, 1, 1 });
		CHECK(chain.tips == std::vector<uint32_t>{ 0, 2, 2, 3, 4 });
		CHECK(chain.driven == std::vector<uint32_t>{ 2, 3, 4 });
		motion(calm, Vec3{}, 4000);
		CHECK(motion(calm, Vec3{}, 200) < 1e-5f);
		for (size_t c = 1; c < calm.positions().size(); ++c) {
			if (chain.children[c].size() == 1) {
				const Vec3 bone = vec::sub(calm.positions()[chain.tips[c]], calm.positions()[c]);
				CHECK(vec::length(bone) == doctest::Approx(chain.length[c]).epsilon(1e-4));
			}
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
					const float scale = forest.placement(t).transform.axis_scales().x;
					for (size_t i = 1; i < s->positions().size(); ++i) {
						const Vec3 d = vec::sub(s->positions()[i], q->positions()[i]);
						const Vec3 world_d = vec::scale(d, scale);
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
		CHECK(linden->stiffness[8] == doctest::Approx(2.578f).epsilon(1e-3));
		CHECK(linden->stiffness[17] == doctest::Approx(0.978f).epsilon(1e-3));
		CHECK(linden->wind[17] == 3.0f);
		CHECK(linden->parents[18] == 13);
		CHECK(linden->stem[18] == 1);
		CHECK(linden->stiffness[18] == doctest::Approx(0.8f));
		CHECK(linden->wind[18] == 3.0f);
		CHECK(linden->parents[19] == 18);
		CHECK(linden->stem[19] == 0);
		CHECK(linden->stiffness[19] == 2.0f);
		CHECK(linden->wind[19] == 8.0f);
		CHECK(linden->rest[1].trans.y == doctest::Approx(-0.38586882f));
		const core::SnapshotDiff diff = core::diff_snapshots(m.before, m.reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}

	TEST_CASE("render.trees: treebeech_m_02_cluster's bones follow R6-VG2-vectors with and without MP_007's wind") {
		test::Mounted m;
		if (!test::open_or_skip(m)) {
			return;
		}
		const BeechLevel level = load_mp007_trees(m);
		REQUIRE(level.beech != nullptr);
		const std::shared_ptr<const TreeRig> &beech = level.beech;
		REQUIRE(beech->bones() == 21);
		const std::array<int32_t, 21> parents = { -1, 0, 1, 2, 3, 4, 5, 6, 7, 5, 9, 5, 11, 1, 13, 14, 15, 16, 17, 16,
			19 };
		const std::array<float, 21> stiffness = { 2.0f, 1.825f, 1.65f, 1.475f, 1.3f, 1.125f, 0.95f, 0.775f, 0.6f, 1, 1,
			1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
		const std::array<float, 21> wind = { 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 4, 3, 4, 3, 4, 4, 4, 4, 4, 4, 4 };
		for (size_t i = 0; i < 21; ++i) {
			CHECK(beech->parents[i] == parents[i]);
			if (i > 0) {
				CHECK_MESSAGE(beech->stiffness[i] == doctest::Approx(stiffness[i]).epsilon(1e-4), "node " << i);
				CHECK_MESSAGE(beech->wind[i] == wind[i], "node " << i);
			}
		}

		const std::array<Vec3, 21> rest = { Vec3{ 0, 0, 0 }, { 0.0109f, -0.6484f, -0.0012f },
			{ -0.1021f, 0.1768f, -0.3658f }, { -0.1021f, 0.3495f, -0.3786f }, { -0.1021f, 0.5287f, -0.3658f },
			{ -0.2234f, 1.6226f, -0.5129f }, { -0.4504f, 2.6657f, -0.7321f }, { -0.6937f, 3.7790f, -0.9404f },
			{ -1.2448f, 5.0685f, -1.2447f }, { -1.3975f, 3.0302f, -0.0270f }, { -2.4305f, 3.7898f, 0.5714f },
			{ 0.6594f, 2.9075f, -1.5185f }, { 1.2765f, 4.0879f, -2.1517f }, { 0.0413f, 0.2066f, 0.3712f },
			{ 0.0413f, 0.3751f, 0.3712f }, { 0.0413f, 0.5760f, 0.3777f }, { 0.0046f, 2.1906f, 0.5008f },
			{ -0.0716f, 3.6362f, 1.1510f }, { -0.1374f, 4.7225f, 1.9960f }, { 0.9370f, 3.4600f, 0.9166f },
			{ 1.8316f, 4.2641f, 1.4568f } };
		const std::array<float, 21> length = { 0, 0.6485f, 0.1732f, 0.1796f, 1.1104f, 1.1104f, 1.1584f, 1.4350f,
			1.4350f, 1.4151f, 1.4151f, 1.4748f, 1.4748f, 0.1685f, 0.2010f, 1.6197f, 1.6197f, 1.3779f, 1.3779f, 1.3186f,
			1.3186f };
		const std::array<float, 21> height = { 0, 0, 0, 0, 0.1777f, 1.2584f, 1.2584f, 0, 1.4097f, 0, 0, 0, 0, 0, 0,
			0.2009f, 1.8186f, 1.8186f, 2.9672f, 2.9672f, 2.6410f };
		const std::array<uint8_t, 21> free = { 0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 0, 1 };
		CHECK(beech->stem_bones == 8);
		CHECK(beech->ground == 0.0f);
		for (size_t i = 1; i < 21; ++i) {
			CHECK_MESSAGE(vec::length(vec::sub(beech->rest[i].trans, rest[i])) < 1e-4f, "V1 rest x node " << i);
			CHECK_MESSAGE(beech->length[i] == doctest::Approx(length[i]).epsilon(1e-3), "V1 m node " << i);
			CHECK_MESSAGE(std::fabs(beech->height[i] - height[i]) < 1e-4f, "V1 h node " << i);
			CHECK_MESSAGE(beech->stem[i] == (i <= 8 ? 1 : 0), "V1 stem node " << i);
			if (i >= 2) {
				CHECK_MESSAGE(beech->rigid[i] == 1 - free[i], "V1 free node " << i);
			}
		}
		CHECK(beech->rigid[1] == 0);
		CHECK(beech->driven.size() == 19);
		const core::Transform &bone4 = beech->rest_frames[4];
		CHECK(vec::length(vec::sub(bone4.right, Vec3{ 0.99377f, 0.11119f, 0.00794f })) < 1e-4f);
		CHECK(vec::length(vec::sub(bone4.up, Vec3{ -0.10917f, 0.98515f, -0.13251f })) < 1e-4f);
		CHECK(vec::length(vec::sub(bone4.forward, Vec3{ -0.02256f, 0.13081f, 0.99115f })) < 1e-4f);
		CHECK(vec::length(vec::sub(beech->offsets[5], Vec3{ 0, 1.11042f, 0 })) < 1e-4f);
		CHECK(vec::length(vec::sub(beech->rest_directions[5], Vec3{ 0, 1, 0 })) < 1e-5f);
		CHECK(vec::length(vec::sub(beech->sides[5], Vec3{ 0, 0, 1 })) < 1e-5f);

		const auto v3 = [](const std::array<double, 15> &p_free) {
			std::array<double, 21> out{};
			const size_t bones[15] = { 4, 5, 6, 7, 8, 9, 10, 11, 12, 15, 16, 17, 18, 19, 20 };
			for (size_t k = 0; k < 15; ++k) {
				out[bones[k]] = p_free[k];
			}
			return out;
		};
		const render::TreeWind mp007 = render::tree_wind({}, render::LevelWind{ 91.485f, 2.0f });

		render::TreeSim calm(beech);
		calm.step(Vec3{});
		struct NodeAt {
			size_t node;
			Vec3 x;
		};
		for (const NodeAt &e :
				{ NodeAt{ 5, { -0.22368f, 1.62251f, -0.51334f } }, NodeAt{ 7, { -0.69425f, 3.77879f, -0.94088f } },
						NodeAt{ 8, { -1.24592f, 5.06806f, -1.24532f } } }) {
			CHECK_MESSAGE(vec::length(vec::sub(calm.positions()[e.node], e.x)) < 2e-5f, "V2 no wind node " << e.node);
			CHECK_MESSAGE(calm.velocities()[e.node].y == doctest::Approx(-0.18605f).epsilon(1e-4),
					"V2 no wind node " << e.node);
		}

		render::TreeSim windy(beech);
		const Vec3 gust = render::tree_gust(mp007, Vec3{}, render::kTreeStep);
		CHECK(vec::length(gust) == doctest::Approx(0.017424f).epsilon(1e-4));
		windy.step(gust);
		struct NodeState {
			size_t node;
			Vec3 v, x;
		};
		for (const NodeState &e :
				{ NodeState{ 5, { -0.00084f, -0.18605f, 0.03253f }, { -0.22370f, 1.62257f, -0.51282f } },
						NodeState{ 8, { -0.00169f, -0.18605f, 0.06507f }, { -1.24602f, 5.06824f, -1.24431f } },
						NodeState{ 16, { -0.00237f, -0.18605f, 0.09133f }, { 0.00451f, 2.19045f, 0.50247f } },
						NodeState{ 18, { -0.00333f, -0.18604f, 0.12853f }, { -0.13749f, 4.72040f, 1.99876f } } }) {
			CHECK_MESSAGE(vec::length(vec::sub(windy.velocities()[e.node], e.v)) < 2e-5f, "V2 wind v node " << e.node);
			CHECK_MESSAGE(vec::length(vec::sub(windy.positions()[e.node], e.x)) < 2e-5f, "V2 wind x node " << e.node);
		}

		render::TreeSim still(beech);
		int steps = 0;
		const auto run_calm = [&](int p_until) {
			for (; steps < p_until; ++steps) {
				still.step(Vec3{});
			}
		};
		run_calm(67);
		check_turns("no wind 1 s", bone_turns(still),
				v3({ 4.64, 4.64, 2.45, 7.97, 7.97, 11.10, 11.10, 8.29, 8.29, 3.50, 3.50, 10.63, 10.63, 11.38, 11.38 }));
		run_calm(133);
		check_turns("no wind 2 s pre-simulation", bone_turns(still),
				v3({ 5.44, 5.44, 2.34, 7.73, 7.73, 10.63, 10.63, 7.83, 7.83, 3.34, 3.34, 10.67, 10.67, 10.83, 10.83 }));
		run_calm(333);
		check_turns("no wind 5 s", bone_turns(still),
				v3({ 5.50, 5.50, 3.26, 8.34, 8.34, 10.55, 10.55, 7.63, 7.63, 3.26, 3.26, 10.44, 10.44, 10.87, 10.87 }));
		run_calm(2000);
		check_turns("no wind 30 s", bone_turns(still),
				v3({ 5.50, 5.50, 3.26, 8.32, 8.32, 10.55, 10.55, 7.63, 7.63, 3.26, 3.26, 10.44, 10.44, 10.87, 10.87 }));

		render::TreeSim gusty(beech);
		steps = 0;
		const auto run_wind = [&](int p_until) {
			for (; steps < p_until; ++steps) {
				gusty.step(render::tree_gust(mp007, Vec3{}, float(steps + 1) * render::kTreeStep));
			}
		};
		run_wind(67);
		check_turns("MP_007 wind 1 s", bone_turns(gusty),
				v3({ 5.26, 5.26, 6.86, 10.38, 10.38, 12.15, 12.15, 6.38, 6.38, 9.57, 9.57, 3.49, 3.49, 12.83, 12.83 }));
		run_wind(333);
		check_turns("MP_007 wind 5 s", bone_turns(gusty),
				v3({ 5.01, 5.01, 4.95, 9.87, 9.87, 13.55, 13.55, 5.45, 5.45, 7.94, 7.94, 0.52, 0.52, 11.62, 11.62 }));
		run_wind(2000);
		check_turns("MP_007 wind 30 s", bone_turns(gusty),
				v3({ 4.07, 4.07, 2.47, 7.32, 7.32, 9.88, 9.88, 8.82, 8.82, 6.83, 6.83, 8.04, 8.04, 11.85, 11.85 }));

		const double calm_length_error = length_error(still), windy_length_error = length_error(gusty);
		std::printf("render.trees beech bone length error at 30 s: %.3e without wind, %.3e with MP_007's\n",
				calm_length_error, windy_length_error);
		CHECK(std::fabs(calm_length_error - 4.6e-4) <= 0.05e-4);
		CHECK(std::fabs(windy_length_error - 4.4e-3) <= 0.05e-3);

		render::TreeSim sway(beech);
		for (int k = 0; k < 133; ++k) {
			sway.step(render::tree_gust(mp007, Vec3{}, render::kTreeStep));
		}
		constexpr int kSwaySteps = 8000;
		std::vector<double> top_z;
		double least = 1e9, most = -1e9, sum = 0.0;
		for (int k = 1; k <= kSwaySteps; ++k) {
			sway.step(render::tree_gust(mp007, Vec3{}, float(k) * render::kTreeStep));
			const double turn = bone_turns(sway)[7];
			least = std::min(least, turn);
			most = std::max(most, turn);
			sum += turn;
			top_z.push_back(double(sway.positions()[8].z));
		}
		double mean_z = 0.0;
		for (const double z : top_z) {
			mean_z += z / double(top_z.size());
		}
		const auto [low_z, high_z] = std::minmax_element(top_z.begin(), top_z.end());
		size_t crossings = 0;
		for (size_t k = 1; k < top_z.size(); ++k) {
			crossings += ((top_z[k - 1] - mean_z) < 0.0) != ((top_z[k] - mean_z) < 0.0) ? 1 : 0;
		}
		const double seconds = double(kSwaySteps) * double(render::kTreeStep);
		double peak_hz = 0.0, peak_power = 0.0;
		for (int bin = 1; bin <= 240; ++bin) {
			const double hz = double(bin) / seconds;
			double re = 0.0, im = 0.0;
			for (size_t k = 0; k < top_z.size(); ++k) {
				const double a = 2.0 * 3.14159265358979323846 * hz * double(k) * double(render::kTreeStep);
				re += (top_z[k] - mean_z) * std::cos(a);
				im += (top_z[k] - mean_z) * std::sin(a);
			}
			if (hz > 0.2 && re * re + im * im > peak_power) {
				peak_power = re * re + im * im;
				peak_hz = hz;
			}
		}
		std::printf("render.trees beech sway over %.0f s of MP_007 wind after its pre-simulation: bone 7 %.2f to %.2f "
					"degrees, mean %.3f; top z range %.3f; %.3f mean crossings a second; strongest above 0.2 Hz at "
					"%.3f Hz\n",
				seconds, least, most, sum / double(kSwaySteps), *high_z - *low_z, double(crossings) / seconds, peak_hz);
		CHECK(std::fabs(least - 3.8) <= 0.05);
		CHECK(std::fabs(most - 13.3) <= 0.05);
		CHECK(std::fabs(sum / double(kSwaySteps) - 8.75) <= 0.005);
		CHECK(std::fabs((*high_z - *low_z) - 0.71) <= 0.005);
		CHECK(std::fabs(double(crossings) / seconds - 0.67) <= 0.005);
		CHECK(std::fabs(peak_hz - 0.50) <= 1.0 / seconds);
		CHECK(peak_hz < 1.0);

		struct Turned {
			float yaw;
			float scale;
		};
		render::TreeSim upright(beech);
		for (int k = 0; k < 800; ++k) {
			upright.step(Vec3{});
		}
		for (const Turned &turned : { Turned{ 90.0f, 1.0f }, Turned{ 180.0f, 1.0f }, Turned{ 0.0f, 0.6f } }) {
			const float a = turned.yaw * core::kDegToRadF;
			core::level::LevelTransform placed;
			placed.right = vec::scale(Vec3{ std::cos(a), 0.0f, -std::sin(a) }, turned.scale);
			placed.up = Vec3{ 0.0f, turned.scale, 0.0f };
			placed.forward = vec::scale(Vec3{ std::sin(a), 0.0f, std::cos(a) }, turned.scale);
			placed.trans = Vec3{ 100.0f, 20.0f, -50.0f };
			render::TreeSim sim(beech, render::tree_root_frame(placed));
			for (int k = 0; k < 800; ++k) {
				sim.step(Vec3{});
			}
			const std::vector<core::Transform> poses = render::tree_bone_poses(sim, placed);
			const std::vector<core::Transform> calm_offsets = upright.bone_offsets();
			REQUIRE(poses.size() == 21);
			if (turned.scale == 1.0f) {
				double worst = 0.0;
				for (size_t b = 1; b < 21; ++b) {
					worst = std::max(worst, turn_degrees(calm_offsets[b].inverse().then_inner(poses[b])));
				}
				const core::Transform root_pose =
						core::Transform{ placed.right, placed.up, placed.forward, placed.trans }.inverse().then_inner(
								core::Transform{ Vec3{ 1, 0, 0 }, Vec3{ 0, 1, 0 }, Vec3{ 0, 0, 1 }, placed.trans });
				CHECK(vec::length(vec::sub(poses[0].right, root_pose.right)) < 1e-5f);
				CHECK(vec::length(vec::sub(poses[0].forward, root_pose.forward)) < 1e-5f);
				CHECK(vec::length(vec::sub(poses[0].trans, root_pose.trans)) < 1e-4f);
				std::printf("render.trees beech at yaw %.0f: largest bone pose difference from the upright tree's "
							"after 12 s %.4f degrees\n",
						double(turned.yaw), worst);
				CHECK(worst <= 0.01);
			} else {
				const std::vector<core::Transform> offsets = sim.bone_offsets();
				std::printf("render.trees beech at scale 0.6: bone 1, 2, 3 and 13 skin determinants %.3f %.3f %.3f "
							"%.3f\n",
						double(offsets[1].determinant()), double(offsets[2].determinant()),
						double(offsets[3].determinant()), double(offsets[13].determinant()));
				CHECK(offsets[1].determinant() == doctest::Approx(0.600f).epsilon(1e-3));
				CHECK(offsets[2].determinant() == doctest::Approx(0.603f).epsilon(1e-3));
				CHECK(offsets[13].determinant() == doctest::Approx(0.600f).epsilon(1e-3));
				CHECK(offsets[3].determinant() == doctest::Approx(0.614f).epsilon(2e-3));
			}
		}
		const core::SnapshotDiff diff = core::diff_snapshots(m.before, m.reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}

	TEST_CASE("render.trees: every MP_007 tree rig settles without wind") {
		test::Mounted m;
		if (!test::open_or_skip(m)) {
			return;
		}
		const BeechLevel level = load_mp007_trees(m);
		REQUIRE(!level.trees.rigs.empty());
		for (size_t r = 0; r < level.trees.rigs.size(); ++r) {
			REQUIRE(level.trees.rigs[r] != nullptr);
			const TreeRig &rig = *level.trees.rigs[r];
			render::TreeSim sim(level.trees.rigs[r]);
			for (int k = 0; k < 2132; ++k) {
				sim.step(Vec3{});
			}
			const std::vector<Vec3> before = sim.positions();
			sim.step(Vec3{});
			double moved = 0.0, held = 0.0;
			for (size_t i = 0; i < before.size(); ++i) {
				moved = std::max(moved, double(vec::length(vec::sub(sim.positions()[i], before[i]))));
			}
			size_t rigid_bones = 0;
			for (size_t c = 1; c < rig.bones(); ++c) {
				if (rig.rigid[rig.tips[c]] != 0 && rig.tips[c] != c) {
					held = std::max(held, relative_turn(sim, c));
					++rigid_bones;
				}
			}
			const std::vector<double> turns = bone_turns(sim);
			const auto worst = std::max_element(turns.begin(), turns.end());
			std::printf("render.trees MP_007 rig %zu of %zu bones settled without wind: largest bone turn %.2f "
						"degrees (bone %zu), %zu short bones held to %.4f degrees in their parent's frame, last step "
						"moved %.2e, bone length error %.2e\n",
					r, rig.bones(), *worst, size_t(worst - turns.begin()), rigid_bones, held, moved, length_error(sim));
			CHECK(held <= 0.01);
			CHECK(moved < 1e-5);
			for (const double t : turns) {
				CHECK(std::isfinite(t));
			}
		}
		const core::SnapshotDiff diff = core::diff_snapshots(m.before, m.reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}

	TEST_CASE("render.trees: a VegetationTree mesh's skin keeps every weighted lane of its vertices") {
		test::Mounted m;
		if (!test::open_or_skip(m)) {
			return;
		}
		const BeechLevel level = load_mp007_trees(m);
		REQUIRE(level.beech != nullptr);
		const std::string name = "objects/vegetation/treebeech_m_02_cluster/treebeech_m_02_cluster_mesh";
		core::Result<render::MeshHead> head = render::read_mesh_head(level.loaded->mount(), name);
		REQUIRE(head.ok());
		core::Result<core::res::LoadedMeshSet> raw = render::load_mesh_lods(level.loaded->mount(), *head, { 0 });
		REQUIRE(raw.ok());
		const core::res::MeshLod &lod = raw->mesh.lods[0];
		std::vector<std::vector<int32_t>> palettes;
		for (const core::res::MeshSubset &subset : lod.subsets) {
			palettes.emplace_back(subset.bone_slots.begin(), subset.bone_slots.end());
		}
		std::vector<core::res::DecodedSubset> decoded;
		REQUIRE(raw->decode(0, core::res::SubsetFilter::Drawn, decoded).ok());
		render::MeshRequest request;
		request.lane_binds = &palettes;
		core::Result<render::PreparedMeshSet> prepared = render::prepare_mesh_set(*raw, request);
		REQUIRE(prepared.ok());
		REQUIRE(prepared->meshes.size() == 1);
		const render::PreparedMesh &mesh = prepared->meshes.front();
		REQUIRE(mesh.surfaces.size() == decoded.size());
		const TreeRig &rig = *level.beech;
		size_t vertices = 0, two_bones = 0, past_bones_per_vertex = 0, near_heaviest = 0, bark = 0;
		for (size_t k = 0; k < decoded.size(); ++k) {
			const core::res::DecodedSubset &d = decoded[k];
			const render::PreparedSurface &s = mesh.surfaces[k];
			REQUIRE(d.bone_indices);
			REQUIRE(d.bone_weights);
			REQUIRE(s.bones.size() == s.vertex_count() * 4);
			const size_t per_vertex = lod.subsets[d.subset].bones_per_vertex;
			for (size_t v = 0; v < s.vertex_count(); ++v) {
				float raw_sum = 0.0f;
				size_t raw_lanes = 0;
				for (size_t l = 0; l < 4; ++l) {
					const float w = (*d.bone_weights)[v][l];
					raw_sum += w;
					raw_lanes += w > 0.0f ? 1 : 0;
					past_bones_per_vertex += w > 0.0f && l >= per_vertex ? 1 : 0;
				}
				REQUIRE(raw_sum > 0.0f);
				size_t kept = 0, heaviest = 0;
				for (size_t l = 0; l < 4; ++l) {
					const float w = (*d.bone_weights)[v][l];
					if (!(w > 0.0f)) {
						continue;
					}
					const int32_t bone = palettes[d.subset][(*d.bone_indices)[v][l]];
					bool found = false;
					for (size_t q = 0; q < 4; ++q) {
						found = found ||
								(s.bones[v * 4 + q] == bone && std::fabs(s.weights[v * 4 + q] - w / raw_sum) < 1e-5f);
					}
					CHECK_MESSAGE(found, "subset " << d.subset << " vertex " << v << " lane " << l);
					kept += found ? 1 : 0;
					heaviest = w > (*d.bone_weights)[v][heaviest] ? l : heaviest;
				}
				two_bones += raw_lanes > 1 ? 1 : 0;
				++vertices;
				if (s.material_name == "Bark") {
					const size_t b = size_t(palettes[d.subset][(*d.bone_indices)[v][heaviest]]);
					REQUIRE(b < rig.bones());
					const Vec3 x{ s.positions[v * 3], s.positions[v * 3 + 1], s.positions[v * 3 + 2] };
					double best = 1e9;
					size_t nearest = 0;
					for (size_t c = 1; c < rig.bones(); ++c) {
						const Vec3 a0 = rig.rest[c].trans, ab = vec::sub(rig.rest[rig.tips[c]].trans, a0);
						const float len2 = vec::dot(ab, ab);
						const float u =
								len2 > 0.0f ? std::clamp(vec::dot(vec::sub(x, a0), ab) / len2, 0.0f, 1.0f) : 0.0f;
						const double dist = vec::length(vec::sub(x, vec::add(a0, vec::scale(ab, u))));
						if (dist < best) {
							best = dist;
							nearest = c;
						}
					}
					near_heaviest += nearest == b ? 1 : 0;
					++bark;
				}
			}
		}
		std::printf(
				"render.trees treebeech_m_02_cluster lod 0: %zu vertices, %zu with two weighted lanes, %zu weighted "
				"lanes past bones_per_vertex; %zu of %zu bark vertices' heaviest bone is the bone segment nearest "
				"them\n",
				vertices, two_bones, past_bones_per_vertex, near_heaviest, bark);
		CHECK(two_bones > 0);
		CHECK(past_bones_per_vertex > 0);
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

	TEST_CASE("render.trees: every MP_007 tree placement settles without wind as its upright rig turned into place") {
		test::Mounted m;
		if (!test::open_or_skip(m)) {
			return;
		}
		const BeechLevel level = load_mp007_trees(m);
		const render::LevelTrees &trees = level.trees;
		REQUIRE(!trees.trees.empty());
		std::vector<std::vector<core::Transform>> upright;
		for (const std::shared_ptr<const TreeRig> &rig : trees.rigs) {
			render::TreeSim sim(rig);
			for (int k = 0; k < 800; ++k) {
				sim.step(Vec3{});
			}
			upright.push_back(sim.bone_offsets());
		}
		std::vector<double> worst(trees.rigs.size(), 0.0), worst_tilt(trees.rigs.size(), 0.0),
				worst_scale(trees.rigs.size(), 0.0);
		double held = 0.0;
		std::vector<double> bent(trees.rigs.size(), 0.0);
		for (const render::TreePlacement &tree : trees.trees) {
			const TreeRig &rig = *trees.rigs[tree.rig];
			render::TreeSim sim(trees.rigs[tree.rig], render::tree_root_frame(tree.transform));
			for (int k = 0; k < 800; ++k) {
				sim.step(Vec3{});
			}
			const std::vector<core::Transform> poses = render::tree_bone_poses(sim, tree.transform);
			for (size_t b = 1; b < rig.bones(); ++b) {
				const double d = turn_degrees(upright[tree.rig][b].inverse().then_inner(poses[b]));
				CHECK(std::isfinite(d));
				if (d > worst[tree.rig]) {
					worst[tree.rig] = d;
					const Vec3 up = vec::normalize(tree.transform.up);
					worst_tilt[tree.rig] =
							std::acos(std::clamp(double(up.y), -1.0, 1.0)) * 180.0 / 3.14159265358979323846;
					worst_scale[tree.rig] = tree.transform.axis_scales().x;
				}
				const core::Transform &parent = sim.frames()[size_t(rig.parents[b])];
				const Vec3 rest = vec::normalize(parent.transform_vector(rig.rest_directions[b]));
				const Vec3 now = vec::normalize(sim.frames()[b].up);
				const double off = std::atan2(double(vec::length(vec::cross(now, rest))), double(vec::dot(now, rest))) *
						180.0 / 3.14159265358979323846;
				if (rig.rigid[rig.tips[b]] != 0 && rig.tips[b] != b) {
					held = std::max(held, off);
				} else {
					bent[tree.rig] = std::max(bent[tree.rig], off);
				}
			}
		}
		for (size_t r = 0; r < trees.rigs.size(); ++r) {
			std::printf(
					"render.trees MP_007 rig %zu placements settled 12 s without wind: largest bone pose difference "
					"from the upright rig %.2f degrees (that tree tilted %.1f degrees, scale %.2f), largest bone "
					"turn from its rest direction in its parent's frame %.2f degrees\n",
					r, worst[r], worst_tilt[r], worst_scale[r], bent[r]);
		}
		std::printf("render.trees MP_007 %zu placements: short bones held to %.4f degrees on their rest direction in "
					"their parent's "
					"frame\n",
				trees.trees.size(), held);
		CHECK(held <= 0.01);
		const core::SnapshotDiff diff = core::diff_snapshots(m.before, m.reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}
}
