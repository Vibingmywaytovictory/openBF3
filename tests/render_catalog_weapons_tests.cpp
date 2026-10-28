#include <doctest/doctest.h>

#include "bf3/anim/crowd_eval.h"
#include "bf3/anim/skinning.h"
#include "bf3/core/ant/ant_library.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/thread_pool.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/core/util/text.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/loadout.h"
#include "bf3/render/crowd_weapons.h"
#include "bf3/render/soldier_assets.h"
#include "bf3/render/viewmodel_set.h"
#include "game_crowd_items.h"
#include "test_support.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace bf3;

namespace {

using bf3::core::seconds_since;
using Role = render::ViewmodelClipRole;
using Source = render::ViewmodelClipSource;

constexpr const char *kLevel = "Win32/Levels/MP_Subway/MP_Subway";

constexpr const char *kCurveFire = "Fire Anim is a CurveAnimationAsset, which no reader decodes (the kick stand-in)";

const std::map<std::string, std::string> kCatalogViewmodelGaps{
	{ "weapons/m40a5/u_m40a5", kCurveFire },
	{ "weapons/model98b/u_m98b", kCurveFire },
	{ "weapons/sv98/u_sv98", kCurveFire },
	{ "weapons/xp1_l96/u_l96", kCurveFire },
	{ "weapons/xp2_jng90/u_jng90", kCurveFire },
};

struct CatalogEntry {
	uint8_t team = 1;
	host::LoadoutWeapon weapon;
	std::string error;
	std::string gap;
	std::string facts;
};

std::string role_facts(const render::ViewmodelItem &p_item) {
	std::string out;
	for (const Role r : { Role::Deploy, Role::Idle, Role::Fire, Role::FireSingle, Role::Reload, Role::ZoomIn,
				 Role::Throw, Role::Knife }) {
		const render::ViewmodelClip &c = p_item.clips[size_t(r)];
		out += std::string(" ") + render::viewmodel_clip_role_name(r) + "=" +
				render::viewmodel_clip_source_name(c.source);
	}
	return out;
}

bool package_has_controller(const render::ViewmodelSet &p_set, const render::ViewmodelItem &p_item,
		std::initializer_list<std::string_view> p_controllers) {
	for (const std::unique_ptr<core::ant::AntPackage> &pkg : p_item.packages) {
		for (const uint32_t index : core::ant::AntLibrary::clips(*pkg)) {
			core::ant::ClipBinding b;
			if (!p_set.library->bind(*pkg, pkg->object(index), b).ok()) {
				continue;
			}
			for (const std::string_view c : p_controllers) {
				if (core::iequals_ascii(b.controller_name, c)) {
					return true;
				}
			}
		}
	}
	return false;
}

struct ItemVerdict {
	std::string error;
	std::string gap;
};

ItemVerdict check_item(const render::ViewmodelSet &p_set, const render::ViewmodelItem &p_item) {
	if (p_item.mesh.empty() || p_item.weapon.prepared.vertices == 0 || !p_item.weapon.prepared.skinned) {
		return { "no skinned 1p mesh", {} };
	}
	if (p_item.binds.size() == 0 || p_item.binds.size() > anim::kMaxPaletteBinds) {
		return { "skin binds " + std::to_string(p_item.binds.size()), {} };
	}
	if (!render::viewmodel_coverage(p_item).covered) {
		return { "deploy, fire or reload has no clip or stand-in", {} };
	}
	if (p_item.clips[size_t(Role::Idle)].source != Source::Clip) {
		return { "no idle clip", {} };
	}
	if (p_item.kind != render::ViewmodelItemKind::Gun) {
		return {};
	}
	if (p_item.ads.controller.empty() || !(p_item.ads.zoom_fov > 0.0f) || !(p_item.ads.hip_fov > 0.0f)) {
		return { "no zoom data", {} };
	}
	if (p_item.ads.zoom_levels >= 2 && p_item.clips[size_t(Role::ZoomIn)].source == Source::Missing) {
		return { "zooms with no zoom clip or blend", {} };
	}
	ItemVerdict v;
	const bool fire_clip = p_item.clips[size_t(Role::Fire)].source == Source::Clip ||
			p_item.clips[size_t(Role::FireSingle)].source == Source::Clip;
	if (!fire_clip && package_has_controller(p_set, p_item, { "Fire", "FireSingle" })) {
		if (p_item.clips[size_t(Role::Fire)].codec != "Curve") {
			return { "a Fire clip in the package is not resolved", {} };
		}
		v.gap = kCurveFire;
	}
	if (p_item.clips[size_t(Role::Reload)].source != Source::Clip) {
		if (package_has_controller(p_set, p_item, { "ReloadClipFull", "ReloadFull", "Reload" })) {
			return { "a Reload clip in the package is not resolved", {} };
		}
		v.gap += std::string(v.gap.empty() ? "" : "; ") +
				"its Ant package has no Reload clip (the lower-and-raise stand-in)";
	}
	return v;
}

struct CrowdEntry {
	uint8_t team = 0;
	host::LoadoutWeapon weapon;
	std::unique_ptr<render::CrowdWeaponSet> set;
	std::string error;
	std::string facts;
};

std::string crowd_part_error(const render::SoldierAssets &p_assets, const render::CrowdWeaponSet &p_set, uint8_t p_team,
		const host::LoadoutWeapon &p_weapon) {
	if (p_set.teams[p_team].size() != 1) {
		return std::to_string(p_set.teams[p_team].size()) + " parts";
	}
	const render::CrowdWeaponPart &p = p_set.teams[p_team][0];
	const size_t rig = p_assets.clips.rig.size();
	if (!p.prepared.skinned || p.prepared.vertices == 0) {
		return "no skinned 3p mesh";
	}
	if (p.binds.size() == 0 || p.binds.size() > anim::kMaxPaletteBinds) {
		return "skin binds " + std::to_string(p.binds.size());
	}
	if (p.weapon_joints.size() != rig || p.weapon_local.size() != rig) {
		return "the weapon joints do not cover the soldier rig";
	}
	if (!std::isfinite(p.reload_rate) || p.reload_rate < 0.5f || p.reload_rate > 2.0f) {
		return "reload rate " + std::to_string(p.reload_rate);
	}
	for (const render::PreparedSurface &s : p.prepared.surfaces) {
		if (s.material_index >= p.materials.slots.size()) {
			return "a surface with no material slot";
		}
		const std::string diffuse = p.materials.slots[s.material_index].texture("Diffuse");
		if (!diffuse.empty() && p_assets.texture(diffuse) == nullptr && p_set.texture(diffuse) == nullptr) {
			return "diffuse texture " + diffuse + " not loaded";
		}
	}
	for (const std::string &n : p_set.notes) {
		if (n.find("did not resolve") != std::string::npos || n.find("texture skipped") != std::string::npos) {
			return n;
		}
	}
	const bool gun = p_weapon.slot == size_t(core::gameplay::WeaponSlot::Primary) ||
			p_weapon.slot == size_t(core::gameplay::WeaponSlot::Sidearm);
	if (gun && p.muzzle_joint < 0) {
		return "a primary or secondary with no muzzle";
	}
	return {};
}

}

TEST_SUITE("render-catalog-weapons") {
	TEST_CASE("catalog viewmodels: every weapon of Metro TDM's loadout catalog, per team, resolves its 1p mesh, skin, "
			  "clips and zoom "
			  "data; a fire clip where its Ant package has one, else the kick stand-in, as the automatic defaults") {
		core::InstallReader reader;
		if (!reader.locate({})) {
			test::skip_install_case(reader.describe().c_str());
			return;
		}
		const core::InstallSnapshot before = reader.snapshot();
		auto t0 = std::chrono::steady_clock::now();
		core::Result<std::shared_ptr<core::Install>> install = core::Install::open(reader);
		REQUIRE(install.ok());
		auto gunplay = host::load_gunplay_level(
				*install, "MP_Subway", "TeamDeathMatch0", host::GunplayLoadOptions{ true, true });
		REQUIRE_MESSAGE(gunplay.ok(), (gunplay.ok() ? "" : gunplay.error().message));
		const host::GunplayLevel &level = **gunplay;
		std::vector<CatalogEntry> entries;
		for (uint8_t team = 1; team <= 2; ++team) {
			for (const host::LoadoutWeapon &w : host::loadout_weapons(level.loadouts, team)) {
				entries.push_back(CatalogEntry{ team, w, {}, {}, {} });
			}
		}
		const double s_catalog = seconds_since(t0);
		REQUIRE(entries.size() > 2 * 8);
		t0 = std::chrono::steady_clock::now();
		core::Result<std::unique_ptr<render::ViewmodelSources>> sources =
				render::open_viewmodel_sources(reader, kLevel, nullptr);
		REQUIRE_MESSAGE(sources.ok(), (sources.ok() ? "" : sources.error().message));
		const double s_sources = seconds_since(t0);
		t0 = std::chrono::steady_clock::now();
		core::parallel_for(entries.size(), [&](size_t i) {
			CatalogEntry &e = entries[i];
			render::ViewmodelSetSpec spec =
					render::team_viewmodel_items(e.team, std::span<const std::string>(&e.weapon.unlock, 1));
			spec.superbundle = kLevel;
			core::Result<std::unique_ptr<render::ViewmodelSet>> set =
					render::load_viewmodel_set(**sources, spec, nullptr);
			if (!set) {
				e.error = set.error().message;
				return;
			}
			if ((*set)->items.size() != 1) {
				e.error = std::to_string((*set)->items.size()) + " items";
				return;
			}
			const render::ViewmodelItem &item = (*set)->items[0];
			const ItemVerdict verdict = check_item(**set, item);
			e.error = verdict.error;
			e.gap = verdict.gap;
			e.facts = std::string(render::viewmodel_item_kind_name(item.kind)) + ", mesh " + item.mesh + ", " +
					std::to_string(item.weapon.prepared.vertices) + " vertices, " + std::to_string(item.binds.size()) +
					" binds, zoom " + std::to_string(int(item.ads.hip_fov)) + " -> " +
					std::to_string(int(item.ads.zoom_fov)) + " (" + std::to_string(item.ads.zoom_levels) + " levels)" +
					role_facts(item);
		});
		const double s_load = seconds_since(t0);
		std::vector<std::string> failed;
		std::map<std::string, std::string> gaps;
		for (const CatalogEntry &e : entries) {
			const std::string verdict =
					!e.error.empty() ? "FAIL " + e.error : (!e.gap.empty() ? "data gap: " + e.gap : "ok");
			std::printf("render.catalog_weapons team %u def %u %s: %s; %s\n", unsigned(e.team), unsigned(e.weapon.def),
					e.weapon.unlock.c_str(), verdict.c_str(), e.facts.c_str());
			if (!e.error.empty()) {
				failed.push_back(std::to_string(e.team) + " " + e.weapon.unlock + ": " + e.error);
			}
			if (!e.gap.empty()) {
				gaps[e.weapon.unlock] = e.gap;
			}
		}
		MESSAGE("catalog ", entries.size(), " (team, weapon) pairs: catalog ", s_catalog, " s, sources ", s_sources,
				" s, viewmodels ", s_load, " s");
		CHECK_MESSAGE(
				failed.empty(), failed.size(), " fail, the first: ", failed.empty() ? std::string() : failed.front());
		for (const auto &[unlock, gap] : gaps) {
			MESSAGE("data gap: ", unlock, ": ", gap);
		}
		CHECK(gaps == kCatalogViewmodelGaps);
		const core::SnapshotDiff diff = core::diff_snapshots(before, reader.snapshot());
		std::printf("render.catalog_weapons install.unchanged %s\n", diff.clean() ? "yes" : diff.summary().c_str());
		CHECK(diff.clean());
	}
}

TEST_SUITE("render-catalog-weapons-3p") {
	TEST_CASE(
			"catalog 3p weapons: every weapon of Metro TDM's loadout catalog, per team, loads its 3p part alone as the "
			"crowd loads a carried weapon, and the crowd evaluator adds and poses every one past the preloaded items") {
		core::InstallReader reader;
		if (!reader.locate({})) {
			test::skip_install_case(reader.describe().c_str());
			return;
		}
		const core::InstallSnapshot before = reader.snapshot();
		auto t0 = std::chrono::steady_clock::now();
		core::Result<std::shared_ptr<core::Install>> install = core::Install::open(reader);
		REQUIRE(install.ok());
		auto gunplay = host::load_gunplay_level(
				*install, "MP_Subway", "TeamDeathMatch0", host::GunplayLoadOptions{ true, true });
		REQUIRE_MESSAGE(gunplay.ok(), (gunplay.ok() ? "" : gunplay.error().message));
		const host::GunplayLevel &level = **gunplay;
		render::SoldierSpec spec;
		core::Result<std::unique_ptr<render::SoldierAssets>> assets =
				render::load_soldier_assets(reader, spec, nullptr, nullptr);
		REQUIRE_MESSAGE(assets.ok(), (assets.ok() ? "" : assets.error().message));
		core::Result<std::unique_ptr<core::EbxLibrary>> ebx = core::EbxLibrary::open(*install, { spec.superbundle });
		REQUIRE_MESSAGE(ebx.ok(), (ebx.ok() ? "" : ebx.error().message));
		const godot_side::CrowdItemLists lists = godot_side::crowd_item_specs(&level, spec);
		core::Result<std::unique_ptr<render::CrowdWeaponSet>> preload =
				render::load_crowd_weapons(**ebx, spec, **assets,
						{ std::span<const render::CrowdWeaponSpec>(lists.teams[0]),
								std::span<const render::CrowdWeaponSpec>(lists.teams[1]) });
		REQUIRE_MESSAGE(preload.ok(), (preload.ok() ? "" : preload.error().message));
		const double s_setup = seconds_since(t0);
		std::vector<CrowdEntry> entries;
		for (uint8_t wire = 1; wire <= 2; ++wire) {
			for (const host::LoadoutWeapon &w : host::loadout_weapons(level.loadouts, wire)) {
				entries.push_back(CrowdEntry{ anim::crowd_team_of_wire(wire), w, nullptr, {}, {} });
			}
		}
		REQUIRE(entries.size() > 2 * godot_side::kCrowdPreloadWeapons);
		t0 = std::chrono::steady_clock::now();
		core::parallel_for(entries.size(), [&](size_t i) {
			CrowdEntry &e = entries[i];
			const render::CrowdWeaponSpec item = godot_side::catalog_crowd_spec(level, e.weapon.def);
			std::array<std::span<const render::CrowdWeaponSpec>, 2> items{};
			items[e.team] = std::span<const render::CrowdWeaponSpec>(&item, 1);
			core::Result<std::unique_ptr<render::CrowdWeaponSet>> set =
					render::load_crowd_weapons(**ebx, spec, **assets, items, nullptr);
			if (!set) {
				e.error = set.error().message;
				return;
			}
			e.error = crowd_part_error(**assets, **set, e.team, e.weapon);
			if ((*set)->teams[e.team].size() == 1) {
				const render::CrowdWeaponPart &p = (*set)->teams[e.team][0];
				e.facts = p.spec.mesh + ", " + std::to_string(p.prepared.vertices) + " vertices, " +
						std::to_string(p.binds.size()) + " binds, reload " + (p.have_reload ? "clip" : "none") +
						", muzzle " + (p.muzzle_joint >= 0 ? "yes" : "no") + ", throw " + (p.have_throw ? "yes" : "no");
			}
			e.set = std::move(*set);
		});
		const double s_load = seconds_since(t0);

		render::strip_team_weapon_parts(**assets);
		const render::SoldierAssets &a = **assets;
		std::array<std::vector<anim::CrowdWeapon>, 2> views{ render::crowd_weapons_of(**preload, 0),
			render::crowd_weapons_of(**preload, 1) };
		std::array<std::map<uint16_t, uint8_t>, 2> index;
		std::vector<std::pair<uint8_t, uint8_t>> carried;
		std::vector<size_t> carried_binds;
		std::vector<anim::CrowdTeam> teams(2);
		for (uint8_t t = 0; t < 2; ++t) {
			for (size_t i = 0; i < (*preload)->teams[t].size(); ++i) {
				index[t].emplace((*preload)->teams[t][i].spec.def, uint8_t(i));
				carried.emplace_back(t, uint8_t(i));
				carried_binds.push_back((*preload)->teams[t][i].binds.size());
			}
			teams[t].binds = &a.teams[t].binds;
			teams[t].reload = &a.teams[t].reload;
			teams[t].weapon_joints = &a.teams[t].weapon_joints;
			teams[t].weapon_local = a.teams[t].weapon_local;
			teams[t].weapons = views[t];
		}
		uint32_t to_add = 0;
		for (const CrowdEntry &e : entries) {
			to_add += e.error.empty() && index[e.team].count(e.weapon.def) == 0 ? 1u : 0u;
		}
		const uint32_t n = uint32_t(carried.size()) + to_add;
		REQUIRE(n <= anim::kMaxCrowdSoldiers);
		anim::CrowdConfig config;
		config.temporal_lod = false;
		auto ev = anim::CrowdEvaluator::make(a.clips, teams, n, config);
		REQUIRE_MESSAGE(ev.ok(), (ev.ok() ? "" : ev.error().message));
		std::array<uint32_t, 2> next{ uint32_t(views[0].size()), uint32_t(views[1].size()) };
		std::vector<std::string> failed;
		uint32_t preloaded = 0;
		for (CrowdEntry &e : entries) {
			const std::string what = std::to_string(e.team) + " " + e.weapon.unlock;
			if (!e.error.empty()) {
				failed.push_back(what + ": " + e.error);
				continue;
			}
			if (const auto it = index[e.team].find(e.weapon.def); it != index[e.team].end()) {
				e.facts += ", preloaded crowd item " + std::to_string(it->second);
				++preloaded;
				continue;
			}
			const std::vector<anim::CrowdWeapon> view = render::crowd_weapons_of(*e.set, e.team);
			core::Result<uint8_t> added = (*ev)->add_weapon(e.team, view[0]);
			if (!added || *added != next[e.team]) {
				failed.push_back(what + ": add_weapon " +
						(added ? "gave index " + std::to_string(*added) + ", not " + std::to_string(next[e.team])
							   : added.error().message));
				continue;
			}
			++next[e.team];
			index[e.team].emplace(e.weapon.def, *added);
			carried.emplace_back(e.team, *added);
			carried_binds.push_back(e.set->teams[e.team][0].binds.size());
			e.facts += ", added as crowd item " + std::to_string(*added);
		}
		std::vector<anim::CrowdSoldierInput> in(n);
		for (size_t k = 0; k < carried.size(); ++k) {
			in[k].team = carried[k].first;
			in[k].weapon = carried[k].second;
		}
		for (int f = 0; f < 2; ++f) {
			(*ev)->set_inputs(in, 1.0 / 60.0);
			(*ev)->evaluate_all_here();
		}
		for (size_t k = 0; k < carried.size(); ++k) {
			const uint32_t s = uint32_t(k);
			if ((*ev)->weapon_of(s) != carried[k].second || (*ev)->weapon_palette(s).size() != carried_binds[k]) {
				failed.push_back("soldier " + std::to_string(k) + " team " + std::to_string(carried[k].first) +
						" carries crowd item " + std::to_string(unsigned((*ev)->weapon_of(s))) + " with " +
						std::to_string((*ev)->weapon_palette(s).size()) + " palette binds, not item " +
						std::to_string(carried[k].second) + " with " + std::to_string(carried_binds[k]));
			}
		}
		for (const CrowdEntry &e : entries) {
			std::printf("render.catalog_weapons_3p team %u def %u %s: %s; %s\n", unsigned(e.team),
					unsigned(e.weapon.def), e.weapon.unlock.c_str(),
					e.error.empty() ? "ok" : ("FAIL " + e.error).c_str(), e.facts.c_str());
		}
		const anim::CrowdCombatStats ms = (*ev)->combat_stats();
		MESSAGE("catalog ", entries.size(), " (team, weapon) pairs: ", preloaded, " preloaded, ",
				next[0] - views[0].size(), " + ", next[1] - views[1].size(), " added; ", ms.weapon_palettes,
				" weapon palettes for ", carried.size(), " soldiers; setup ", s_setup, " s, 3p parts ", s_load, " s");
		CHECK_MESSAGE(
				failed.empty(), failed.size(), " fail, the first: ", failed.empty() ? std::string() : failed.front());
		CHECK(carried.size() == n);
		CHECK(ms.weapon_palettes == n);
		const core::SnapshotDiff diff = core::diff_snapshots(before, reader.snapshot());
		std::printf("render.catalog_weapons_3p install.unchanged %s\n", diff.clean() ? "yes" : diff.summary().c_str());
		CHECK(diff.clean());
	}
}
