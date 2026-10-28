#include <doctest/doctest.h>

#include "bf3/anim/crowd_eval.h"
#include "bf3/anim/skinning.h"
#include "bf3/core/ant/ant_library.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/weapon_resolver.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/thread_pool.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/core/util/text.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/loadout.h"
#include "bf3/render/crowd_weapons.h"
#include "bf3/render/mesh_cache.h"
#include "bf3/render/soldier_assets.h"
#include "bf3/render/viewmodel_set.h"
#include "bf3/render/weapon_sockets.h"
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

constexpr float kSightOffsetMeasured = 0.06f;
constexpr float kSightMeasuredMetres = 0.001f;
constexpr float kSightRatioSpread = 0.3f;

const std::set<std::string> kCurveFireWeapons{ "weapons/m40a5/u_m40a5", "weapons/model98b/u_m98b",
	"weapons/sv98/u_sv98", "weapons/xp1_l96/u_l96", "weapons/xp2_jng90/u_jng90" };

struct CatalogEntry {
	uint8_t team = 1;
	host::LoadoutWeapon weapon;
	std::string error;
	std::string gap;
	std::string facts;
	std::string fire;
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
		return { "a Fire clip in the package is not resolved", {} };
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

uint32_t accessory_ending(const host::LoadoutItem &p_item, size_t p_slot, std::string_view p_suffix) {
	for (size_t i = 0; i < p_item.accessory_unlocks[p_slot].size(); ++i) {
		if (p_item.accessory_unlocks[p_slot][i].ends_with(p_suffix)) {
			return p_item.accessories[p_slot][i];
		}
	}
	return 0;
}

std::array<uint32_t, host::kLoadoutAccessories> m16_acog_foregrip_suppressor(const host::LoadoutItem &p_m16) {
	return { accessory_ending(p_m16, 0, "_acog"), accessory_ending(p_m16, 1, "_foregrip"),
		accessory_ending(p_m16, 2, "_sound_suppressor"), 0 };
}

const host::LoadoutItem *catalog_primary(
		const host::LoadoutCatalog &p_catalog, std::string_view p_unlock, uint8_t &r_team) {
	for (uint8_t team = 1; team <= 2; ++team) {
		for (uint8_t k = 0; k < host::kKitCount; ++k) {
			const host::LoadoutKit *kit = p_catalog.kit(team, k);
			for (size_t i = 0; kit != nullptr && i < kit->slots[0].size(); ++i) {
				if (kit->slots[0][i].unlock == p_unlock) {
					r_team = team;
					return &kit->slots[0][i];
				}
			}
		}
	}
	return nullptr;
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
			for (const std::string &n : (*set)->notes) {
				if (n.find(" 1p Fire:") != std::string::npos) {
					e.facts += "; " + n;
				}
			}
			const render::ViewmodelClip &fire = item.clips[size_t(Role::Fire)];
			e.fire = fire.source == Source::Clip ? fire.codec + " " + fire.controller : std::string();
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
		CHECK(gaps.empty());
		std::set<std::string> curve_fire, chooser_fire;
		for (const CatalogEntry &e : entries) {
			if (e.fire.starts_with("Curve ")) {
				curve_fire.insert(e.weapon.unlock);
			}
			if (e.facts.find("Wep_ShotgunFireChooser") != std::string::npos) {
				chooser_fire.insert(e.weapon.unlock + " " + e.fire);
			}
		}
		CHECK(curve_fire == kCurveFireWeapons);
		CHECK(chooser_fire == std::set<std::string>{ "weapons/remington870/u_870 Dct 1P_MasterShotgunFire" });
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
						", muzzle " + (p.muzzle_joint >= 0 ? "yes" : "no") + ", throw " +
						(p.have_throw ? "yes" : "no") + ", " + std::to_string(p.accessory_meshes.size()) +
						" default accessory meshes";
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

namespace {

struct AccessoryRow {
	uint8_t team = 1;
	std::string weapon;
	size_t slot = 0;
	std::string unlock;
	std::vector<std::string> meshes;
	size_t sockets = 0;
	size_t modifiers = 0;
	std::string error;
	std::string facts;
};

constexpr const char *kAccessoryGap = "an unlock that no SocketData, WeaponModifierData or GunSwayModifierData names";
constexpr const char *kCamouflageGap = " (camouflage)";

std::string accessory_gap(const AccessoryRow &p_row) {
	if (p_row.sockets != 0 || p_row.modifiers != 0 ||
			p_row.unlock.starts_with(core::gameplay::kNoAccessoryUnlockPrefix)) {
		return {};
	}
	return std::string(kAccessoryGap) + (p_row.unlock.find("camo") != std::string::npos ? kCamouflageGap : "");
}

}

TEST_SUITE("render-catalog-accessories") {
	TEST_CASE(
			"catalog accessories: every accessory of every primary of Metro TDM's loadout catalog, per team, resolves "
			"the meshes of its sockets, their attachment bone and its weapon modifiers, or is a named data gap") {
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
		core::Result<std::unique_ptr<core::EbxLibrary>> ebx = core::EbxLibrary::open(*install, { kLevel });
		REQUIRE_MESSAGE(ebx.ok(), (ebx.ok() ? "" : ebx.error().message));
		const core::EbxLibrary &lib = **ebx;
		const render::ViewmodelSetSpec vm_spec;
		const core::EbxPartition *ske = lib.partition_by_name(vm_spec.weapon_skeleton);
		REQUIRE(ske != nullptr);
		core::Result<anim::EbxSkeleton> weapon_skeleton = anim::read_ebx_skeleton(*ske, vm_spec.weapon_skeleton);
		REQUIRE(weapon_skeleton.ok());
		const double s_setup = seconds_since(t0);

		struct Weapon {
			uint8_t team = 1;
			const host::LoadoutItem *item = nullptr;
			std::vector<AccessoryRow> rows;
			std::string error;
		};
		std::vector<Weapon> weapons;
		size_t listed = 0;
		for (uint8_t team = 1; team <= 2; ++team) {
			std::set<uint16_t> seen;
			for (uint8_t k = 0; k < host::kKitCount; ++k) {
				for (const host::LoadoutItem &item :
						level.loadouts.kit(team, k)->slots[host::kLoadoutAccessoryWeapon]) {
					if (seen.insert(item.def).second) {
						weapons.push_back(Weapon{ team, &item, {}, {} });
						for (const std::vector<uint32_t> &a : item.accessories) {
							listed += a.size();
						}
					}
				}
			}
		}
		REQUIRE(weapons.size() == 2 * 61);
		t0 = std::chrono::steady_clock::now();
		core::parallel_for(weapons.size(), [&](size_t i) {
			Weapon &w = weapons[i];
			core::Result<core::gameplay::WeaponDef> base = core::gameplay::resolve_weapon(lib, w.item->unlock);
			const core::EbxPartition *bp = base ? lib.partition_by_name(base->blueprint) : nullptr;
			const std::vector<core::EbxInstance> swd =
					bp != nullptr ? bp->instances_of("SoldierWeaponData") : std::vector<core::EbxInstance>();
			if (swd.empty()) {
				w.error = base ? std::string("no SoldierWeaponData") : base.error().message;
				return;
			}
			const std::vector<render::WeaponSocket> sockets = render::read_weapon_sockets(lib, swd.front());
			for (size_t a = 0; a < host::kLoadoutAccessories; ++a) {
				for (const std::string &unlock : w.item->accessory_unlocks[a]) {
					AccessoryRow row;
					row.team = w.team;
					row.weapon = w.item->unlock;
					row.slot = a;
					row.unlock = unlock;
					for (const render::WeaponSocket &s : sockets) {
						if (s.unlock != unlock) {
							continue;
						}
						++row.sockets;
						for (const render::WeaponSocketObject &o : s.objects) {
							if (!o.skinned && weapon_skeleton->find(s.bone) < 0) {
								row.error = "socket bone `" + s.bone + "` is not a weapon skeleton bone";
							}
							for (const std::string *m : { &o.mesh_1p, &o.mesh_1p_zoom, &o.mesh_3p }) {
								if (!m->empty() &&
										std::find(row.meshes.begin(), row.meshes.end(), *m) == row.meshes.end()) {
									row.meshes.push_back(*m);
								}
							}
						}
					}
					core::gameplay::WeaponResolveOptions opt;
					opt.unlocks = { unlock };
					core::Result<core::gameplay::WeaponDef> d =
							core::gameplay::resolve_weapon(lib, w.item->unlock, opt);
					if (!d) {
						row.error = "resolve with it: " + d.error().message;
					} else {
						for (const std::string &m : d->applied_modifiers) {
							row.modifiers += core::to_lower_ascii(m).starts_with(unlock + ":") ? 1u : 0u;
						}
						for (const auto *list : { &d->sway.applied, &d->sway.conditional }) {
							for (const core::gameplay::SwayGatedModifier &g : *list) {
								row.modifiers += core::to_lower_ascii(g.unlock) == unlock ? 1u : 0u;
							}
						}
						row.facts = "zoom fov " + std::to_string(int(d->zoom_render_fov)) + ", controller " +
								d->aiming_controller + ", sound " + d->sound + (d->silenced ? ", silenced" : "");
					}
					w.rows.push_back(std::move(row));
				}
			}
		});
		std::vector<std::string> meshes;
		for (const Weapon &w : weapons) {
			for (const AccessoryRow &r : w.rows) {
				meshes.insert(meshes.end(), r.meshes.begin(), r.meshes.end());
			}
		}
		std::sort(meshes.begin(), meshes.end());
		meshes.erase(std::unique(meshes.begin(), meshes.end()), meshes.end());
		std::vector<std::string> mesh_errors(meshes.size());
		core::parallel_for(meshes.size(), [&](size_t i) {
			core::Result<render::MeshHead> head = render::read_mesh_head(lib.mount(0), meshes[i]);
			if (!head) {
				mesh_errors[i] = head.error().message;
			} else if (head->mesh.lods.empty()) {
				mesh_errors[i] = "no LODs";
			}
		});
		const double s_rows = seconds_since(t0);
		std::vector<std::string> failed;
		std::map<std::string, size_t> gaps;
		size_t rows = 0, with_mesh = 0, with_modifiers = 0;
		for (const Weapon &w : weapons) {
			if (!w.error.empty()) {
				failed.push_back(w.item->unlock + ": " + w.error);
			}
			for (const AccessoryRow &r : w.rows) {
				++rows;
				std::string error = r.error;
				for (const std::string &m : r.meshes) {
					const size_t i = size_t(std::lower_bound(meshes.begin(), meshes.end(), m) - meshes.begin());
					if (error.empty() && !mesh_errors[i].empty()) {
						error = "mesh " + m + ": " + mesh_errors[i];
					}
				}
				const std::string gap = accessory_gap(r);
				with_mesh += r.meshes.empty() ? 0u : 1u;
				with_modifiers += r.modifiers != 0 ? 1u : 0u;
				const std::string verdict =
						!error.empty() ? "FAIL " + error : (!gap.empty() ? "data gap: " + gap : std::string("ok"));
				std::printf("render.catalog_accessories team %u %s slot %zu %s: %s; %zu sockets, %zu meshes, %zu "
							"modifiers; %s\n",
						unsigned(r.team), r.weapon.c_str(), r.slot + 1, r.unlock.c_str(), verdict.c_str(), r.sockets,
						r.meshes.size(), r.modifiers, r.facts.c_str());
				if (!error.empty()) {
					failed.push_back(r.weapon + " " + r.unlock + ": " + error);
				} else if (!gap.empty()) {
					++gaps[gap];
				}
			}
		}
		MESSAGE(rows, " accessories of ", weapons.size(), " (team, primary) pairs: ", with_mesh,
				" with socket meshes (", meshes.size(), " distinct), ", with_modifiers,
				" with weapon modifiers; setup ", s_setup, " s, rows ", s_rows, " s");
		for (const auto &[gap, n] : gaps) {
			MESSAGE("data gap: ", n, " accessories: ", gap);
		}
		CHECK(rows == listed);
		CHECK(gaps ==
				std::map<std::string, size_t>{
						{ kAccessoryGap, 14 }, { std::string(kAccessoryGap) + kCamouflageGap, 104 } });
		CHECK_MESSAGE(
				failed.empty(), failed.size(), " fail, the first: ", failed.empty() ? std::string() : failed.front());
		const core::SnapshotDiff diff = core::diff_snapshots(before, reader.snapshot());
		std::printf("render.catalog_accessories install.unchanged %s\n", diff.clean() ? "yes" : diff.summary().c_str());
		CHECK(diff.clean());
	}
}

TEST_SUITE("render-catalog-accessories-1p") {
	TEST_CASE("catalog accessories: a viewmodel with picked accessories draws their sockets' meshes in place of the "
			  "no-accessory ones, the skinned ones on the weapon skin, and zooms at the optic's FOV") {
		core::InstallReader reader;
		if (!reader.locate({})) {
			test::skip_install_case(reader.describe().c_str());
			return;
		}
		const core::InstallSnapshot before = reader.snapshot();
		core::Result<std::shared_ptr<core::Install>> install = core::Install::open(reader);
		REQUIRE(install.ok());
		auto gunplay = host::load_gunplay_level(
				*install, "MP_Subway", "TeamDeathMatch0", host::GunplayLoadOptions{ true, true });
		REQUIRE_MESSAGE(gunplay.ok(), (gunplay.ok() ? "" : gunplay.error().message));
		const host::GunplayLevel &level = **gunplay;
		core::Result<std::unique_ptr<render::ViewmodelSources>> sources =
				render::open_viewmodel_sources(reader, kLevel, nullptr);
		REQUIRE_MESSAGE(sources.ok(), (sources.ok() ? "" : sources.error().message));
		const host::LoadoutItem *m16 = level.loadouts.kit(1, 0)->find(0, level.loadouts.kit(1, 0)->defaults[0]);
		REQUIRE(m16 != nullptr);
		const std::array<uint32_t, host::kLoadoutAccessories> picks = m16_acog_foregrip_suppressor(*m16);
		REQUIRE(picks[0] != 0);
		REQUIRE(picks[1] != 0);
		REQUIRE(picks[2] != 0);
		const auto load = [&](const std::array<uint32_t, host::kLoadoutAccessories> &p_picks) {
			render::ViewmodelSetSpec spec =
					render::team_viewmodel_items(1, std::span<const std::string>(&m16->unlock, 1));
			spec.superbundle = kLevel;
			const host::LoadoutAccessorySet set = host::loadout_accessory_set(*m16, p_picks);
			spec.items[0].accessories = render::WeaponAccessories{ set.active, set.listed };
			return render::load_viewmodel_set(**sources, spec, nullptr);
		};
		core::Result<std::unique_ptr<render::ViewmodelSet>> plain = load({});
		REQUIRE_MESSAGE(plain.ok(), (plain.ok() ? "" : plain.error().message));
		core::Result<std::unique_ptr<render::ViewmodelSet>> picked = load(picks);
		REQUIRE_MESSAGE(picked.ok(), (picked.ok() ? "" : picked.error().message));
		const render::ViewmodelItem &a = (*plain)->items[0];
		const render::ViewmodelItem &b = (*picked)->items[0];
		const auto sockets_of = [](const render::ViewmodelItem &p_item) {
			std::set<std::string> out;
			for (const render::ViewmodelAttachment &at : p_item.attachments) {
				out.insert(at.socket_unlock);
			}
			return out;
		};
		CHECK(sockets_of(a) == std::set<std::string>{ "weapons/common/nooptics" });
		const std::set<std::string> drawn = sockets_of(b);
		CHECK(drawn.count("weapons/common/nooptics") == 0);
		CHECK(drawn ==
				std::set<std::string>{ "weapons/m16a4/u_m16a4_acog", "weapons/m16a4/u_m16a4_foregrip",
						"weapons/m16a4/u_m16a4_sound_suppressor" });
		size_t skinned = 0, zoom_only = 0, prepared = 0;
		for (const render::ViewmodelAttachment &at : b.attachments) {
			skinned += at.skinned ? 1u : 0u;
			zoom_only += at.zoom && !at.hip ? 1u : 0u;
			prepared += at.part.prepared.vertices > 0 ? 1u : 0u;
			MESSAGE("M16A4 picked attachment ", at.socket_unlock, ": ", at.part.mesh, " skinned ", at.skinned, " hip ",
					at.hip, " zoom ", at.zoom, ", ", at.part.prepared.vertices, " vertices");
		}
		CHECK(skinned == 1);
		CHECK(zoom_only == 1);
		CHECK(std::all_of(a.attachments.begin(), a.attachments.end(),
				[](const render::ViewmodelAttachment &p_a) { return p_a.hip && p_a.zoom; }));
		CHECK(prepared == b.attachments.size());
		CHECK(b.binds.size() > a.binds.size());
		CHECK(b.binds.size() <= anim::kMaxPaletteBinds);
		MESSAGE("M16A4 zoom: iron sights ", a.ads.controller, " ", a.ads.zoom_fov, " deg, render ", a.zoom_render_fov,
				"; ACOG ", b.ads.controller, " ", b.ads.zoom_fov, " deg, render ", b.zoom_render_fov);
		CHECK(b.ads.controller == "weapons/common/aimingcontrollers/aim_default_4x");
		CHECK(b.ads.zoom_fov < a.ads.zoom_fov);
		CHECK(b.zoom_render_fov == doctest::Approx(15.0f));
		const core::SnapshotDiff diff = core::diff_snapshots(before, reader.snapshot());
		CHECK(diff.clean());
	}
}

TEST_SUITE("render-catalog-accessories-3p") {
	TEST_CASE("catalog accessories 3p: a carried M16A4 with picked accessories loads their sockets' 3p meshes into its "
			  "crowd part, rigid on the socket bone and skinned to the weapon skeleton, within the palette cap") {
		core::InstallReader reader;
		if (!reader.locate({})) {
			test::skip_install_case(reader.describe().c_str());
			return;
		}
		const core::InstallSnapshot before = reader.snapshot();
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
		const host::LoadoutItem *m16 = level.loadouts.kit(1, 0)->find(0, level.loadouts.kit(1, 0)->defaults[0]);
		REQUIRE(m16 != nullptr);
		const std::array<uint32_t, host::kLoadoutAccessories> picks = m16_acog_foregrip_suppressor(*m16);
		const auto load = [&](const std::array<uint32_t, host::kLoadoutAccessories> &p_picks) {
			render::CrowdWeaponSpec item = godot_side::catalog_crowd_spec(level, m16->def);
			const host::LoadoutAccessorySet set = host::loadout_accessory_set(*m16, p_picks);
			item.accessories = render::WeaponAccessories{ set.active, set.listed };
			std::array<std::span<const render::CrowdWeaponSpec>, 2> items{};
			items[0] = std::span<const render::CrowdWeaponSpec>(&item, 1);
			return render::load_crowd_weapons(**ebx, spec, **assets, items, nullptr);
		};
		core::Result<std::unique_ptr<render::CrowdWeaponSet>> plain = load({});
		REQUIRE_MESSAGE(plain.ok(), (plain.ok() ? "" : plain.error().message));
		core::Result<std::unique_ptr<render::CrowdWeaponSet>> picked = load(picks);
		REQUIRE_MESSAGE(picked.ok(), (picked.ok() ? "" : picked.error().message));
		REQUIRE((*plain)->teams[0].size() == 1);
		REQUIRE((*picked)->teams[0].size() == 1);
		const render::CrowdWeaponPart &a = (*plain)->teams[0][0];
		const render::CrowdWeaponPart &b = (*picked)->teams[0][0];
		std::string plain_meshes, picked_meshes;
		for (const std::string &m : a.accessory_meshes) {
			plain_meshes += " " + m;
		}
		for (const std::string &m : b.accessory_meshes) {
			picked_meshes += " " + m;
		}
		MESSAGE("M16A4 3p plain:", plain_meshes, " (", a.prepared.vertices, " vertices, ", a.binds.size(),
				" binds); ACOG, foregrip, suppressor:", picked_meshes, " (", b.prepared.vertices, " vertices, ",
				b.binds.size(), " binds)");
		std::set<std::string> expected;
		const core::EbxPartition *bp =
				(*ebx)->partition_by_name(core::gameplay::resolve_weapon(**ebx, m16->unlock)->blueprint);
		REQUIRE(bp != nullptr);
		const std::vector<render::WeaponSocket> sockets =
				render::read_weapon_sockets(**ebx, bp->instances_of("SoldierWeaponData").front());
		for (const render::WeaponSocket &s : sockets) {
			if (s.unlock.ends_with("_acog") || s.unlock.ends_with("_foregrip") ||
					s.unlock.ends_with("_sound_suppressor")) {
				for (const render::WeaponSocketObject &o : s.objects) {
					if (!o.mesh_3p.empty()) {
						expected.insert(o.mesh_3p);
					}
				}
			}
		}
		CHECK(expected.size() >= 2);
		CHECK(std::set<std::string>(b.accessory_meshes.begin(), b.accessory_meshes.end()) == expected);
		CHECK_FALSE(a.accessory_meshes.empty());
		CHECK(b.prepared.vertices > a.prepared.vertices);
		CHECK(b.binds.size() <= anim::kMaxPaletteBinds);
		CHECK(b.materials.slots.size() > a.materials.slots.size());
		for (const render::PreparedSurface &s : b.prepared.surfaces) {
			CHECK(s.material_index < b.materials.slots.size());
			CHECK(s.bones.size() == s.vertex_count() * 4);
		}
		const core::SnapshotDiff diff = core::diff_snapshots(before, reader.snapshot());
		CHECK(diff.clean());
	}
}

TEST_SUITE("render-catalog-accessories-1p") {
	TEST_CASE("catalog accessories: every optic of the M416, SV98 and M16A4 has a sight picture, an unlit blended "
			  "surface "
			  "with its shader's colour texture, which the viewmodel centres at the ZoomPose; the optic's "
			  "WeaponZoomedOffsetY has the sign of that move and a fixed ratio to it per weapon") {
		core::InstallReader reader;
		if (!reader.locate({})) {
			test::skip_install_case(reader.describe().c_str());
			return;
		}
		const core::InstallSnapshot before = reader.snapshot();
		core::Result<std::shared_ptr<core::Install>> install = core::Install::open(reader);
		REQUIRE(install.ok());
		auto gunplay = host::load_gunplay_level(
				*install, "MP_Subway", "TeamDeathMatch0", host::GunplayLoadOptions{ true, true });
		REQUIRE_MESSAGE(gunplay.ok(), (gunplay.ok() ? "" : gunplay.error().message));
		const host::GunplayLevel &level = **gunplay;
		core::Result<std::unique_ptr<render::ViewmodelSources>> sources =
				render::open_viewmodel_sources(reader, kLevel, nullptr);
		REQUIRE_MESSAGE(sources.ok(), (sources.ok() ? "" : sources.error().message));
		const core::EbxLibrary &lib = *(*sources)->ebx;
		struct OpticLoad {
			size_t weapon = 0;
			uint8_t team = 0;
			const host::LoadoutItem *item = nullptr;
			size_t index = 0;
			std::unique_ptr<render::ViewmodelSet> set;
			std::string error;
		};
		const std::array<std::string_view, 3> weapons{ "weapons/m416/u_m416", "weapons/sv98/u_sv98",
			"weapons/m16a4/u_m16a4" };
		std::array<std::map<std::string, float>, 3> offsets;
		std::vector<OpticLoad> loads;
		for (size_t w = 0; w < weapons.size(); ++w) {
			uint8_t team = 0;
			const host::LoadoutItem *item = catalog_primary(level.loadouts, weapons[w], team);
			REQUIRE(item != nullptr);
			core::Result<std::vector<core::gameplay::WeaponUnlockModifier>> mods =
					core::gameplay::weapon_unlock_modifiers(lib, item->unlock, "WeaponAnimationConfigurationModifier");
			REQUIRE(mods.ok());
			for (const core::gameplay::WeaponUnlockModifier &m : *mods) {
				const std::optional<core::EbxValue> config = m.modifier.get("AnimationConfiguration");
				const std::optional<core::EbxStruct> s = config ? config->as_struct() : std::nullopt;
				const std::optional<core::EbxInstance> offset =
						s ? lib.follow(*s, "WeaponOffsetModuleData") : std::nullopt;
				const std::optional<core::EbxValue> y = offset ? offset->get("WeaponZoomedOffsetY") : std::nullopt;
				if (y && y->as_f32()) {
					offsets[w][m.unlock] = *y->as_f32();
				}
			}
			for (size_t i = 0; i < item->accessories[0].size(); ++i) {
				if (!item->accessory_unlocks[0][i].starts_with(render::kDefaultSocketUnlockPrefix)) {
					loads.push_back(OpticLoad{ w, team, item, i, {}, {} });
				}
			}
		}
		core::parallel_for(loads.size(), [&](size_t i) {
			OpticLoad &l = loads[i];
			render::ViewmodelSetSpec spec =
					render::team_viewmodel_items(l.team, std::span<const std::string>(&l.item->unlock, 1));
			spec.superbundle = kLevel;
			const host::LoadoutAccessorySet set =
					host::loadout_accessory_set(*l.item, { l.item->accessories[0][l.index], 0, 0, 0 });
			spec.items[0].accessories = render::WeaponAccessories{ set.active, set.listed };
			core::Result<std::unique_ptr<render::ViewmodelSet>> vs =
					render::load_viewmodel_set(**sources, spec, nullptr);
			if (vs) {
				l.set = std::move(*vs);
			} else {
				l.error = vs.error().message;
			}
		});
		std::array<std::vector<float>, 3> ratios;
		std::array<std::string, 3> lines;
		for (const OpticLoad &l : loads) {
			const std::string &optic = l.item->accessory_unlocks[0][l.index];
			REQUIRE_MESSAGE(l.set != nullptr, optic, ": ", l.error);
			const render::ViewmodelItem &vm = l.set->items[0];
			CHECK_MESSAGE(vm.sight.has_value(), optic, " has no sight picture");
			size_t textured = 0, sights = 0, roles = 0, single = 0, single_kept = 0;
			for (const render::ViewmodelAttachment &a : vm.attachments) {
				for (const render::ViewmodelSlotLook &look : a.looks) {
					if (!look.blend || !look.unlit) {
						continue;
					}
					++sights;
					const render::PreparedTexture *t = l.set->texture(look.color_texture, look.color_role);
					textured += !look.color_texture.empty() && t != nullptr ? 1u : 0u;
					roles += (look.blend_mode == render::ViewmodelBlend::Multiply) ==
									(look.color_role == render::TextureRole::Color)
							? 1u
							: 0u;
					single += t != nullptr && t->stored_levels == 1 ? 1u : 0u;
					single_kept += t != nullptr && t->stored_levels == 1 && t->levels == 1 ? 1u : 0u;
				}
			}
			CHECK_MESSAGE(sights > 0, optic);
			CHECK_MESSAGE(textured == sights, optic, ": ", textured, " of ", sights, " sight surfaces textured");
			CHECK_MESSAGE(roles == sights, optic, ": ", roles, " of ", sights,
					" sight surfaces sample their texture as colour exactly when they multiply");
			CHECK_MESSAGE(single > 0, optic, ": no sight texture stored with one level");
			CHECK_MESSAGE(single_kept == single, optic, ": ", single_kept, " of ", single,
					" sight textures stored with one level are drawn from it alone");
			const auto offset = offsets[l.weapon].find(optic);
			if (!vm.sight || offset == offsets[l.weapon].end()) {
				continue;
			}
			char b[160];
			std::snprintf(b, sizeof(b), " %s y %.4f at z %.3f, offset %.3f;",
					optic.substr(optic.rfind('/') + 1).c_str(), double(vm.sight->at_zoom_pose.y),
					double(vm.sight->at_zoom_pose.z), double(offset->second));
			lines[l.weapon] += b;
			if (std::fabs(offset->second) >= kSightOffsetMeasured &&
					std::fabs(vm.sight->at_zoom_pose.y) >= kSightMeasuredMetres) {
				CHECK_MESSAGE((vm.sight->at_zoom_pose.y > 0.0f) == (offset->second < 0.0f), optic);
				ratios[l.weapon].push_back(vm.sight->at_zoom_pose.y / -offset->second);
			}
		}
		for (size_t w = 0; w < weapons.size(); ++w) {
			std::vector<float> &r = ratios[w];
			REQUIRE(r.size() >= 5);
			std::sort(r.begin(), r.end());
			const float median = r[r.size() / 2];
			MESSAGE(weapons[w], ": sight centre over -WeaponZoomedOffsetY, median ", median, " of ", r.size(),
					", from ", r.front(), " to ", r.back(), ";", lines[w]);
			CHECK(r.front() > median * (1.0f - kSightRatioSpread));
			CHECK(r.back() < median * (1.0f + kSightRatioSpread));
		}
		CHECK(loads.size() >= 30);
		const core::SnapshotDiff diff = core::diff_snapshots(before, reader.snapshot());
		CHECK(diff.clean());
	}
}
