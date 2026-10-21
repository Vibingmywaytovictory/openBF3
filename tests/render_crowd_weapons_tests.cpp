#include <doctest/doctest.h>

#include "bf3/anim/crowd_eval.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/host/gunplay.h"
#include "bf3/render/crowd_weapons.h"
#include "bf3/render/soldier_assets.h"
#include "game_crowd_items.h"
#include "test_support.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace bf3;

namespace {

using bf3::core::seconds_since;

anim::Xform xform_of(const anim::Mat34 &p_m) {
	return anim::Xform{ anim::quat_from_mat(p_m), p_m.origin() };
}

anim::Vec3 rotate(const anim::Quat &p_q, const anim::Vec3 &p_v) {
	return anim::xform_mul(anim::Xform{ p_q, anim::Vec3{} }, anim::Xform{ anim::Quat{}, p_v }).t;
}

}

TEST_SUITE("render-crowd-weapons") {
	TEST_CASE("crowd weapons: kit items, binds, reload, muzzle, strip, views, evaluator") {
		core::InstallReader reader;
		if (!reader.locate({})) {
			test::skip_install_case(reader.describe().c_str());
			return;
		}
		const core::InstallSnapshot before = reader.snapshot();
		render::SoldierSpec spec;
		auto t0 = std::chrono::steady_clock::now();
		core::Result<std::unique_ptr<render::SoldierAssets>> assets = render::load_soldier_assets(reader, spec, nullptr, nullptr);
		REQUIRE_MESSAGE(assets.ok(), (assets.ok() ? "" : assets.error().message));
		const double s_assets = seconds_since(t0);
		t0 = std::chrono::steady_clock::now();
		core::Result<std::shared_ptr<core::Install>> install = core::Install::open(reader);
		REQUIRE(install.ok());
		auto gunplay = host::load_gunplay_level(*install, "MP_Subway", "TeamDeathMatch0");
		REQUIRE_MESSAGE(gunplay.ok(), (gunplay.ok() ? "" : gunplay.error().message));
		const double s_gunplay = seconds_since(t0);
		const godot_side::CrowdItemLists lists = godot_side::crowd_item_specs(gunplay->get(), spec);
		for (const std::string &n : lists.notes) {
			MESSAGE("items: " << n);
		}
		for (size_t t = 0; t < 2; ++t) {
			REQUIRE(!lists.teams[t].empty());
			CHECK(lists.teams[t][0].weapon_def == spec.teams[t].weapon_def);
			CHECK(lists.teams[t][0].def != sim::weapons::kNoWeapon);
			CHECK(lists.teams[t].size() <= anim::kMaxCrowdWeapons);
			std::string line;
			for (const render::CrowdWeaponSpec &s : lists.teams[t]) {
				line += " " + std::to_string(s.def) + ":" + s.weapon_def;
			}
			std::printf("render.crowd_weapons team %zu items (%zu):%s\n", t, lists.teams[t].size(), line.c_str());
		}
		t0 = std::chrono::steady_clock::now();
		core::Result<std::unique_ptr<render::CrowdWeaponSet>> set =
				render::load_crowd_weapons(reader, spec, **assets, { std::span<const render::CrowdWeaponSpec>(lists.teams[0]), std::span<const render::CrowdWeaponSpec>(lists.teams[1]) });
		REQUIRE_MESSAGE(set.ok(), (set.ok() ? "" : set.error().message));
		const double s_weapons = seconds_since(t0);
		for (const std::string &n : (*set)->notes) {
			std::printf("render.crowd_weapons note: %s\n", n.c_str());
		}
		const render::SoldierAssets &a = **assets;
		const anim::Skeleton &rig = a.clips.rig;
		const int32_t wep_root = rig.find("Wep_Root");
		REQUIRE(wep_root >= 0);
		uint32_t muzzles = 0, firearms = 0;
		for (size_t t = 0; t < 2; ++t) {
			const std::vector<render::CrowdWeaponPart> &items = (*set)->teams[t];
			REQUIRE(!items.empty());
			CHECK(items[0].spec.def == lists.teams[t][0].def);
			CHECK(items.size() >= 4);
			for (const render::CrowdWeaponPart &p : items) {
				CHECK(p.prepared.skinned);
				CHECK(p.prepared.vertices > 0);
				CHECK(p.binds.size() > 0);
				CHECK(p.binds.size() <= anim::kMaxPaletteBinds);
				CHECK(p.weapon_joints.size() == rig.size());
				CHECK(p.weapon_local.size() == rig.size());
				CHECK(std::isfinite(p.reload_rate));
				CHECK(p.reload_rate >= 0.5f);
				CHECK(p.reload_rate <= 2.0f);
				const bool firearm = p.spec.weapon_def.find("knife") == std::string::npos && p.spec.weapon_def.find("m67") == std::string::npos;
				firearms += firearm ? 1 : 0;
				if (p.muzzle_joint < 0) {
					std::printf("render.crowd_weapons team %zu %s: no muzzle\n", t, p.spec.weapon_def.c_str());
					CHECK(!firearm);
					continue;
				}
				++muzzles;
				CHECK(p.muzzle_joint == wep_root);

				const int32_t w_root = a.weapon_skeleton.find("Wep_Root");
				REQUIRE(w_root >= 0);
				const anim::Xform in_mesh = anim::xform_mul(xform_of(a.weapon_skeleton.model_pose[size_t(w_root)]), p.muzzle_offset);
				const anim::Vec3 dir = rotate(in_mesh.q, anim::Vec3{ 0, 1, 0 });
				double past = -1e9, tip_lateral = 0;
				for (const render::PreparedSurface &s : p.prepared.surfaces) {
					for (size_t v = 0; v < s.vertex_count(); ++v) {
						const anim::Vec3 d{ s.positions[3 * v] - in_mesh.t.x, s.positions[3 * v + 1] - in_mesh.t.y, s.positions[3 * v + 2] - in_mesh.t.z };
						const double along = double(d.x * dir.x + d.y * dir.y + d.z * dir.z);
						if (along > past) {
							past = along;
							const double len2 = double(d.x * d.x + d.y * d.y + d.z * d.z);
							tip_lateral = std::sqrt(std::max(0.0, len2 - along * along));
						}
					}
				}
				std::printf("render.crowd_weapons team %zu %s: muzzle at (%.3f, %.3f, %.3f) in the mesh, barrel (%.2f, %.2f, %.2f); the farthest vertex along it %.1f cm "
							"past, %.1f cm off axis; %zu binds, %llu vertices, reload %s rate %.3f\n",
						t, p.spec.weapon_def.c_str(), double(in_mesh.t.x), double(in_mesh.t.y), double(in_mesh.t.z), double(dir.x), double(dir.y), double(dir.z), past * 100.0,
						tip_lateral * 100.0, p.binds.size(), (unsigned long long)p.vertices, p.have_reload ? "yes" : "no", double(p.reload_rate));
				CHECK(dir.z > 0.99f);

				if (firearm && in_mesh.t.z > 0.3f) {
					CHECK(past < 0.15);
					CHECK(past > -0.05);
					CHECK(tip_lateral < 0.05);
				}
			}

			uint32_t matched = 0;
			for (const anim::SkinBind &b : items[0].binds.binds()) {
				for (const anim::SkinBind &tb : a.teams[t].binds.binds()) {
					if (tb.bone == b.bone && anim::mat_max_diff(tb.inverse_bind, b.inverse_bind) < 1e-5f) {
						++matched;
						break;
					}
				}
			}
			CHECK(matched == items[0].binds.size());
			std::printf("render.crowd_weapons team %zu item 0: %u of %zu binds are S3's team weapon binds\n", t, matched, items[0].binds.size());
		}
		CHECK(muzzles >= firearms);

		std::array<uint64_t, 2> before_vertices{ a.teams[0].vertices, a.teams[1].vertices };
		CHECK(render::strip_team_weapon_parts(**assets) == 2);
		for (size_t t = 0; t < 2; ++t) {
			for (const render::SoldierPart &p : a.teams[t].parts) {
				CHECK(!p.weapon);
			}
			CHECK(a.teams[t].vertices < before_vertices[t]);
			CHECK(a.teams[t].vertices + (*set)->teams[t][0].vertices == before_vertices[t]);
		}
		std::array<std::vector<anim::CrowdWeapon>, 2> views{ render::crowd_weapons_of(**set, 0), render::crowd_weapons_of(**set, 1) };
		std::vector<anim::CrowdTeam> teams(2);
		for (size_t t = 0; t < 2; ++t) {
			CHECK(views[t].size() == (*set)->teams[t].size());
			for (size_t i = 0; i < (*set)->teams[t].size(); ++i) {
				CHECK(render::crowd_weapon_index(**set, uint32_t(t), (*set)->teams[t][i].spec.def) == int32_t(i));
			}
			teams[t].binds = &a.teams[t].binds;
			teams[t].reload = &a.teams[t].reload;
			teams[t].weapon_joints = &a.teams[t].weapon_joints;
			teams[t].weapon_local = a.teams[t].weapon_local;
			teams[t].weapons = views[t];
		}
		CHECK(render::crowd_weapon_index(**set, 0, 0xFFFE) == -1);

		const uint32_t n = uint32_t(views[0].size() + views[1].size());
		anim::CrowdConfig config;
		config.temporal_lod = false;
		auto ev = anim::CrowdEvaluator::make(a.clips, teams, n, config);
		REQUIRE_MESSAGE(ev.ok(), (ev.ok() ? "" : ev.error().message));
		std::vector<anim::CrowdSoldierInput> in(n);
		uint32_t k = 0;
		for (uint8_t t = 0; t < 2; ++t) {
			for (size_t i = 0; i < views[t].size(); ++i, ++k) {
				in[k].team = t;
				in[k].weapon = uint8_t(i);
				in[k].flags = anim::crowd_flags::kReloadStart;
			}
		}
		for (int f = 0; f < 30; ++f) {
			(*ev)->set_inputs(in, 1.0 / 60.0);
			(*ev)->evaluate_all_here();
			for (anim::CrowdSoldierInput &s : in) {
				s.flags = 0;
			}
		}
		k = 0;
		for (uint8_t t = 0; t < 2; ++t) {
			for (size_t i = 0; i < views[t].size(); ++i, ++k) {
				CHECK((*ev)->weapon_of(k) == i);
				CHECK((*ev)->weapon_palette(k).size() == (*set)->teams[t][i].binds.size());
				CHECK((*ev)->palette(k).size() == a.teams[t].binds.size());
				anim::Xform mz;
				CHECK((*ev)->muzzle_model(k, mz) == ((*set)->teams[t][i].muzzle_joint >= 0));
			}
		}
		const anim::CrowdCombatStats ms = (*ev)->combat_stats();
		std::printf("render.crowd_weapons evaluator: %u soldiers, %u weapon palettes, %u muzzles, %u reloading; load: soldier assets %.2f s, gunplay %.2f s, "
					"weapons %.2f s (branch, unlocked, shared machine: indicative only)\n",
				n, ms.weapon_palettes, ms.muzzles, (*ev)->stats().reloading, s_assets, s_gunplay, s_weapons);
		CHECK(ms.weapon_palettes == n);
		const core::SnapshotDiff diff = core::diff_snapshots(before, reader.snapshot());
		std::printf("render.crowd_weapons install.unchanged %s\n", diff.clean() ? "yes" : diff.summary().c_str());
		CHECK(diff.clean());
	}
}
