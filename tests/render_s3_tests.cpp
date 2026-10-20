#include <doctest/doctest.h>

#include "bf3/anim/crowd_eval.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/render/soldier_assets.h"
#include "test_support.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fs = std::filesystem;
using namespace bf3;

namespace {

struct Scratch {
	fs::path root;
	Scratch() {
		wchar_t tmp[MAX_PATH];
		GetTempPathW(MAX_PATH, tmp);
		root = fs::path(tmp) / (L"openbf3-render-s3-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
	}
	~Scratch() {
		std::error_code ec;
		fs::remove_all(root, ec);
	}
};

uint32_t identity_count(const std::vector<anim::Mat34> &p_skin, float p_eps = 1e-3f) {
	uint32_t n = 0;
	const anim::Mat34 id;
	for (const anim::Mat34 &m : p_skin) {
		n += anim::mat_max_diff(m, id) < p_eps ? 1 : 0;
	}
	return n;
}

struct Expected {
	const char *mesh;
	bool weapon;
};

}

TEST_SUITE("render-s3") {
	TEST_CASE("s3 soldiers: census, binds, weapon layout, materials, cache cold then warm, pinch at rest") {
		core::InstallReader reader;
		if (!reader.locate({})) {
			test::skip_install_case(reader.describe().c_str());
			return;
		}
		const core::InstallSnapshot before = reader.snapshot();
		Scratch scratch;
		render::SoldierSpec spec;
		std::unique_ptr<render::SoldierAssets> assets;
		for (int pass = 0; pass < 2; ++pass) {
			core::Cache cache;
			core::CacheOptions options;
			options.root = scratch.root;
			options.guard = reader.write_guard();
			REQUIRE(cache.open(options) == core::CacheStatus::Ok);
			const auto t0 = std::chrono::steady_clock::now();
			auto r = render::load_soldier_assets(reader, spec, nullptr, &cache);
			const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
			MESSAGE((pass == 0 ? "cold" : "warm") << " load_soldier_assets " << seconds << " s (ant " << (*r)->seconds_ant << ", ebx " << (*r)->seconds_ebx
												  << ", meshes " << (*r)->seconds_meshes << ", textures " << (*r)->seconds_textures << ")");
			for (const render::SoldierTeam &team : (*r)->teams) {
				for (const render::SoldierPart &part : team.parts) {
					CHECK_MESSAGE(part.cache.outcome == (pass == 0 ? core::CacheOutcome::Converted : core::CacheOutcome::Hit), part.mesh);
				}
			}
			if (pass == 1) {
				assets = std::move(*r);
			} else {
				for (const std::string &n : (*r)->notes) {
					MESSAGE(n);
				}
			}
		}
		const render::SoldierAssets &a = *assets;
		const Expected us[] = { { "characters/headgear/us_helmet09/us_helmet09_mesh", false }, { "characters/heads/sp_cole/head_cole_mp_mesh", false },
			{ "characters/upperbody/us_upperbody04/us_upperbody04_mesh", false }, { "characters/lowerbody/us_lowerbody03/us_lb03_mesh", false },
			{ "weapons/m16a4/m16a4_3p_mesh", true } };
		const Expected ru[] = { { "characters/headgear/ru_helmet05/ru_helmet05_mesh", false }, { "characters/lowerbody/us_lowerbody03/us_lb03_mesh", false },
			{ "characters/upperbody/ru_upperbody01/ru_upperbody01_mesh", false }, { "weapons/ak74m/ak74_3p_mesh", true } };
		REQUIRE(a.teams[0].parts.size() == std::size(us));
		REQUIRE(a.teams[1].parts.size() == std::size(ru));
		for (size_t t = 0; t < 2; ++t) {
			const render::SoldierTeam &team = a.teams[t];
			const Expected *e = t == 0 ? us : ru;
			for (size_t k = 0; k < team.parts.size(); ++k) {
				CHECK(team.parts[k].mesh == e[k].mesh);
				CHECK(team.parts[k].weapon == e[k].weapon);
				CHECK(team.parts[k].prepared.skinned);
				for (const render::PreparedSurface &s : team.parts[k].prepared.surfaces) {
					CHECK_MESSAGE(s.material_index < team.parts[k].materials.slots.size(), team.parts[k].mesh);
				}
			}
			MESSAGE("team " << t << ": " << team.binds.size() << " binds, " << team.vertices << " vertices, " << team.triangles << " triangles, reload index "
							<< team.reload_index << " (" << team.reload.tracks.end_frame << " frames), reload time " << team.reload_time << " s");
		}
		CHECK(a.teams[0].parts[1].variation.empty());
		CHECK(a.teams[1].parts[1].variation == "characters/lowerbody/us_lowerbody03/ru_lb03_desert");

		CHECK(a.teams[0].vertices == 17724);
		CHECK(a.teams[0].triangles == 20836);
		CHECK(a.teams[1].vertices == 18517);
		CHECK(a.teams[1].triangles == 21881);
		CHECK(a.teams[0].binds.size() == 113);
		CHECK(a.teams[1].binds.size() == 107);
		CHECK(a.teams[0].reload_index == 41);
		CHECK(a.teams[1].reload_index == 33);
		CHECK(a.teams[0].reload.tracks.end_frame == 95);
		CHECK(a.teams[1].reload.tracks.end_frame == 108);
		CHECK(!a.textures.empty());
		CHECK(a.lod.tiers[0].distance == 2.0f);
		CHECK(a.lod.far_distance == 208.0f);
		CHECK(a.body_skeleton.size() == 213);

		const anim::Skeleton &rig = a.clips.rig;
		uint32_t layout_joints = 0;
		for (float w : a.teams[0].weapon_joints.weight) {
			layout_joints += w > 0.0f ? 1 : 0;
		}
		MESSAGE("weapon layout joints: " << layout_joints);
		CHECK(layout_joints == 20);
		const uint32_t expect_plain[2] = { 109, 103 };
		for (size_t t = 0; t < 2; ++t) {
			const render::SoldierTeam &team = a.teams[t];
			std::vector<anim::Xform> model;
			std::vector<anim::Mat34> skin;
			anim::model_pose(rig, anim::rest_pose(rig), model);
			anim::skin_matrices(model, team.binds.binds(), skin);
			const uint32_t plain = identity_count(skin);
			anim::LocalPose laid = anim::rest_pose(rig);
			for (size_t j = 0; j < rig.size(); ++j) {
				if (team.weapon_joints.weight[j] > 0.0f) {
					laid.joints[j] = team.weapon_local[j];
				}
			}
			anim::model_pose(rig, laid, model);
			anim::skin_matrices(model, team.binds.binds(), skin);
			const uint32_t with_layout = identity_count(skin);
			MESSAGE("team " << t << " rest identity: " << plain << " / " << team.binds.size() << " plain, " << with_layout << " with the weapon layout");
			CHECK(plain == expect_plain[t]);
			CHECK(with_layout == team.binds.size());

			const render::PinchReport p = render::measure_pinch(team, rig, model, skin);
			MESSAGE("team " << t << " pinch at rest: left " << p.min_ratio_left << " (" << p.vertices_left << " vertices), right " << p.min_ratio_right << " ("
							<< p.vertices_right << "), hand-IK " << p.hand_ik_m << " m");
			CHECK(p.vertices_left > 20);
			CHECK(p.vertices_right > 20);
			CHECK(p.min_ratio_left == doctest::Approx(1.0f).epsilon(1e-4));
			CHECK(p.min_ratio_right == doctest::Approx(1.0f).epsilon(1e-4));
			CHECK(p.below_left + p.below_right == 0);
		}

		std::vector<anim::CrowdTeam> teams(2);
		for (size_t t = 0; t < 2; ++t) {
			teams[t].binds = &a.teams[t].binds;
			teams[t].reload = &a.teams[t].reload;
			teams[t].weapon_joints = &a.teams[t].weapon_joints;
			teams[t].weapon_local = a.teams[t].weapon_local;
		}
		auto e = anim::CrowdEvaluator::make(a.clips, teams, 2, anim::CrowdConfig{});
		REQUIRE_MESSAGE(e.ok(), (e.ok() ? "" : e.error().message));
		std::vector<anim::CrowdSoldierInput> in(2);
		in[0].team = 0;
		in[1].team = 1;
		in[0].flags = in[1].flags = anim::crowd_flags::kReloadStart;
		(*e)->set_inputs(in, 0.0);
		(*e)->evaluate_all_here();
		in[0].flags = in[1].flags = 0;
		(*e)->set_inputs(in, 1.3);
		(*e)->evaluate_all_here();
		CHECK((*e)->stats().reloading == 2);
		for (uint32_t t = 0; t < 2; ++t) {
			const std::span<const anim::Mat34> pal = (*e)->palette(t);
			const render::PinchReport p = render::measure_pinch(a.teams[t], rig, (*e)->model(t), pal);
			MESSAGE("team " << t << " pinch 1.3 s into the reload: left " << p.min_ratio_left << " (" << p.below_left << " < 0.7), right " << p.min_ratio_right << " ("
							<< p.below_right << " < 0.7), hand-IK " << p.hand_ik_m << " m");
			CHECK(pal.size() == a.teams[t].binds.size());
		}

		{
			std::vector<anim::CrowdSoldierInput> idle(2);
			idle[0].team = 0;
			idle[1].team = 1;
			idle[1].pitch = 0.6f;
			auto ei = anim::CrowdEvaluator::make(a.clips, teams, 2, anim::CrowdConfig{});
			REQUIRE(ei.ok());
			(*ei)->set_inputs(idle, 0.0);
			(*ei)->evaluate_all_here();
			for (uint32_t t = 0; t < 2; ++t) {
				const std::vector<anim::SkinBind> &binds = a.teams[t].binds.binds();
				const std::span<const anim::Mat34> pal = (*ei)->palette(t);
				int32_t root = -1;
				for (size_t b = 0; b < binds.size(); ++b) {
					root = binds[b].name == "Wep_Root" ? int32_t(b) : root;
				}
				REQUIRE(root >= 0);
				std::string loose;
				for (size_t b = 0; b < binds.size(); ++b) {
					if (binds[b].name.rfind("Wep_", 0) != 0) {
						continue;
					}
					const float diff = anim::mat_max_diff(pal[b], pal[size_t(root)]);
					if (diff > 1e-3f) {
						loose += " " + binds[b].name + "=" + std::to_string(diff);
					}
				}
				MESSAGE("team " << t << std::string(t == 0 ? " idle" : " idle + aim") << ": weapon binds off Wep_Root:" << (loose.empty() ? " none" : loose));
				CHECK_MESSAGE(loose.empty(), loose);
			}
		}
		const bf3::core::SnapshotDiff d = bf3::core::diff_snapshots(before, reader.snapshot());
		MESSAGE("install unchanged: " << (d.clean() ? "yes" : d.summary()));
		CHECK(d.clean());
	}
}
