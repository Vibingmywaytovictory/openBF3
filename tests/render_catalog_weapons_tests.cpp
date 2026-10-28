#include <doctest/doctest.h>

#include "bf3/anim/crowd_eval.h"
#include "bf3/anim/skinning.h"
#include "bf3/core/ant/ant_library.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/thread_pool.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/core/util/text.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/loadout.h"
#include "bf3/render/viewmodel_set.h"
#include "test_support.h"

#include <algorithm>
#include <chrono>
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
