#include <doctest/doctest.h>

#include "bf3/anim/crowd_eval.h"
#include "bf3/anim/skinning.h"
#include "bf3/core/ant/ant_library.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/weapon_resolver.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/core/thread_pool.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/core/util/text.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/level_load.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/host/loadout.h"
#include "bf3/present/vehicle_turrets.h"
#include "bf3/render/crowd_weapons.h"
#include "bf3/render/deployed_gadgets.h"
#include "bf3/render/mesh_cache.h"
#include "bf3/render/soldier_assets.h"
#include "bf3/render/texture_prep.h"
#include "bf3/render/vehicle_meshes.h"
#include "bf3/render/viewmodel_set.h"
#include "bf3/render/weapon_camo.h"
#include "bf3/render/weapon_lights.h"
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
constexpr float kSightUnmarked = 1.0f / 255.0f;
constexpr float kSightMark = 0.75f;
constexpr float kSightBlank = 0.25f;
constexpr float kSightDrawnHalf = 0.5f;
constexpr size_t kOpticBatch = 32;

const std::set<std::string> kCurveFireWeapons{ "weapons/gadgets/defibrillator/u_defib", "weapons/m40a5/u_m40a5",
	"weapons/model98b/u_m98b", "weapons/sv98/u_sv98", "weapons/xp1_l96/u_l96", "weapons/xp2_jng90/u_jng90" };

const std::set<std::string> kGadgetItems{ "weapons/gadgets/ammobag/u_ammobag", "weapons/gadgets/c4/u_c4",
	"weapons/gadgets/claymore/u_claymore", "weapons/gadgets/defibrillator/u_defib", "weapons/gadgets/eodbot/u_eodbot",
	"weapons/gadgets/m15/u_m15", "weapons/gadgets/m224/u_m224", "weapons/gadgets/mav/u_mav",
	"weapons/gadgets/medicbag/u_medkit", "weapons/gadgets/radiobeacon/u_radiobeacon",
	"weapons/gadgets/repairtool/u_repairtool", "weapons/gadgets/soflam/u_soflam", "weapons/gadgets/t-ugs/u_ugs" };

std::set<std::string> gadget_slot_weapons() {
	std::set<std::string> out = kGadgetItems;
	out.insert({ "weapons/xp4_crossbow_prototype/u_crossbow_scoped_cobra",
			"weapons/xp4_crossbow_prototype/u_crossbow_scoped_riflescope", "weapons/gadgets/m26mass/u_m26mass",
			"weapons/gadgets/m26mass/u_m26mass_flechette", "weapons/gadgets/m26mass/u_m26mass_frag",
			"weapons/gadgets/m26mass/u_m26mass_slug", "weapons/gadgets/m320/u_m320_he",
			"weapons/gadgets/m320/u_m320_lvg", "weapons/gadgets/m320/u_m320_shg", "weapons/gadgets/m320/u_m320_smk",
			"weapons/rpg7/u_rpg7", "weapons/smaw/u_smaw", "weapons/fgm148/u_fgm148", "weapons/fim92a/u_fim92",
			"weapons/sa18igla/u_sa18igla" });
	return out;
}

struct CatalogEntry {
	uint8_t team = 1;
	host::LoadoutWeapon weapon;
	std::string error;
	std::string gap;
	std::string facts;
	std::string fire;
	bool gadget = false;
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

struct SightBrightness {
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<float> texels;
};

SightBrightness sight_brightness(const core::LevelMount &p_mount, const std::string &p_name) {
	SightBrightness out;
	core::Result<core::res::LoadedTexture> t = core::res::load_texture(p_mount, p_name);
	std::vector<core::res::MipSlice> mips;
	std::vector<std::array<float, 4>> texels;
	if (!t || !t->mips(mips).ok() || mips.empty() || !mips[0].texels(t->texture.format, texels)) {
		return out;
	}
	out.width = mips[0].width;
	out.height = mips[0].height;
	for (const std::array<float, 4> &c : texels) {
		out.texels.push_back(std::max({ c[0], c[1], c[2] }));
	}
	return out;
}

float drawn_sight_value(const render::PreparedTexture &p_t, uint32_t p_x, uint32_t p_y) {
	const size_t block = size_t(p_y / 4) * size_t((p_t.width + 3) / 4) + p_x / 4;
	const size_t at = size_t(p_y % 4) * 4 + p_x % 4;
	switch (p_t.format) {
		case render::GpuFormat::L8:
			return float(p_t.data[size_t(p_y) * p_t.width + p_x]) / 255.0f;
		case render::GpuFormat::Bc3:
			return float(render::bc4_values(p_t.data.data() + block * 16)[at]) / 255.0f;
		case render::GpuFormat::Bc1:
			return float(render::bc1_luminance(p_t.data.data() + block * 8)[at]) / 255.0f;
		default:
			return -1.0f;
	}
}

template <class F> void each_covered_texel(const render::PreparedSurface &p_s, uint32_t p_w, uint32_t p_h, F &&p_at) {
	const auto texel = [&](int32_t p_i) {
		return std::array<float, 2>{ p_s.uv0[2 * size_t(p_i)] * float(p_w), p_s.uv0[2 * size_t(p_i) + 1] * float(p_h) };
	};
	for (size_t t = 0; t + 2 < p_s.indices.size(); t += 3) {
		const std::array<float, 2> a = texel(p_s.indices[t]), b = texel(p_s.indices[t + 1]),
								   c = texel(p_s.indices[t + 2]);
		const float area = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
		if (std::fabs(area) < 1e-6f) {
			continue;
		}
		const int32_t x0 = int32_t(std::floor(std::min({ a[0], b[0], c[0] }))),
					  x1 = int32_t(std::ceil(std::max({ a[0], b[0], c[0] })));
		const int32_t y0 = int32_t(std::floor(std::min({ a[1], b[1], c[1] }))),
					  y1 = int32_t(std::ceil(std::max({ a[1], b[1], c[1] })));
		for (int32_t y = y0; y < y1; ++y) {
			for (int32_t x = x0; x < x1; ++x) {
				const float px = float(x) + 0.5f, py = float(y) + 0.5f;
				const float u = ((b[0] - px) * (c[1] - py) - (c[0] - px) * (b[1] - py)) / area;
				const float v = ((c[0] - px) * (a[1] - py) - (a[0] - px) * (c[1] - py)) / area;
				if (u < 0.0f || v < 0.0f || u + v > 1.0f) {
					continue;
				}
				const int32_t w = int32_t(p_w), h = int32_t(p_h);
				p_at(uint32_t(((x % w) + w) % w), uint32_t(((y % h) + h) % h));
			}
		}
	}
}

struct SightAudit {
	size_t clear_vertices = 0, clear_kept = 0, marks = 0, marks_dark = 0, mixed = 0, mixed_agree = 0;
	size_t sights = 0, textured = 0, roles = 0, single = 0, single_kept = 0;
	size_t multiply_slots = 0, faded_rings = 0;
	std::vector<std::string> undecoded;
	std::vector<std::string> dark_housings;
};

SightAudit audit_sight(const render::ViewmodelSet &p_set, const core::LevelMount &p_mount,
		std::map<std::string, SightBrightness> &r_brightness) {
	SightAudit out;
	const render::ViewmodelItem &vm = p_set.items[0];
	for (const render::ViewmodelAttachment &a : vm.attachments) {
		for (const render::PreparedSurface &sf : a.part.prepared.surfaces) {
			const render::ViewmodelSlotLook *look =
					sf.material_index < a.looks.size() ? &a.looks[sf.material_index] : nullptr;
			if (look == nullptr || !look->blend || !look->unlit || sf.uv0.size() < 2 * sf.vertex_count()) {
				continue;
			}
			const render::PreparedTexture *t = p_set.texture(look->color_texture, look->color_role);
			auto [at, fresh] = r_brightness.try_emplace(look->color_texture);
			if (fresh) {
				at->second = sight_brightness(p_mount, look->color_texture);
			}
			const SightBrightness &raw = at->second;
			if (t == nullptr || raw.width != t->width || raw.height != t->height) {
				out.undecoded.push_back(look->color_texture);
				continue;
			}
			const auto raw_at = [&](uint32_t p_x, uint32_t p_y) { return raw.texels[size_t(p_y) * raw.width + p_x]; };
			if (look->blend_mode == render::ViewmodelBlend::Multiply) {
				bool ring = false;
				for (size_t k = 0; k < sf.vertex_count(); ++k) {
					const float u = sf.uv0[2 * k] - std::floor(sf.uv0[2 * k]);
					const float v = sf.uv0[2 * k + 1] - std::floor(sf.uv0[2 * k + 1]);
					const uint32_t x = std::min(t->width - 1, uint32_t(u * float(t->width)));
					const uint32_t y = std::min(t->height - 1, uint32_t(v * float(t->height)));
					if (raw_at(x, y) > kSightUnmarked) {
						continue;
					}
					++out.clear_vertices;
					out.clear_kept += drawn_sight_value(*t, x, y) >= 1.0f - kSightUnmarked ? 1u : 0u;
					ring = ring || (sf.colors.size() >= 4 * sf.vertex_count() && sf.colors[4 * k] == 0);
				}
				out.faded_rings += ring ? 1u : 0u;
				each_covered_texel(sf, t->width, t->height, [&](uint32_t p_x, uint32_t p_y) {
					if (raw_at(p_x, p_y) >= kSightMark) {
						++out.marks;
						out.marks_dark += drawn_sight_value(*t, p_x, p_y) < kSightDrawnHalf ? 1u : 0u;
					}
				});
			} else {
				each_covered_texel(sf, t->width, t->height, [&](uint32_t p_x, uint32_t p_y) {
					const float r = raw_at(p_x, p_y);
					if (r >= kSightMark || r <= kSightBlank) {
						++out.mixed;
						out.mixed_agree +=
								(drawn_sight_value(*t, p_x, p_y) >= kSightDrawnHalf) == (r >= kSightMark) ? 1u : 0u;
					}
				});
			}
		}
	}
	for (const render::ViewmodelAttachment &a : vm.attachments) {
		for (const render::ViewmodelSlotLook &look : a.looks) {
			if (look.no_specular) {
				out.dark_housings.push_back(a.part.mesh);
			}
			if (!look.blend || !look.unlit) {
				continue;
			}
			++out.sights;
			out.multiply_slots += look.blend_mode == render::ViewmodelBlend::Multiply ? 1u : 0u;
			const render::PreparedTexture *t = p_set.texture(look.color_texture, look.color_role);
			out.textured += !look.color_texture.empty() && t != nullptr ? 1u : 0u;
			out.roles += look.color_role ==
							(look.blend_mode == render::ViewmodelBlend::Multiply ? render::TextureRole::InvertedMask
																				 : render::TextureRole::Mask)
					? 1u
					: 0u;
			out.single += t != nullptr && t->stored_levels == 1 ? 1u : 0u;
			out.single_kept += t != nullptr && t->stored_levels == 1 && t->levels == 1 ? 1u : 0u;
		}
	}
	return out;
}

void check_sight(const SightAudit &p_a, const std::string &p_optic) {
	for (const std::string &u : p_a.undecoded) {
		CHECK_MESSAGE(false, p_optic, ": ", u, " not decoded");
	}
	CHECK_MESSAGE(p_a.clear_kept == p_a.clear_vertices, p_optic, ": ", p_a.clear_kept, " of ", p_a.clear_vertices,
			" multiplying sight vertices over unmarked texels draw their vertex colour");
	CHECK_MESSAGE(p_a.marks_dark == p_a.marks, p_optic, ": ", p_a.marks_dark, " of ", p_a.marks,
			" marked texels under a multiplying sight darken the scene");
	CHECK_MESSAGE(p_a.mixed_agree == p_a.mixed, p_optic, ": ", p_a.mixed_agree, " of ", p_a.mixed,
			" texels under a mixing sight draw its tint exactly where the texture is marked");
	CHECK_MESSAGE(p_a.sights > 0, p_optic);
	CHECK_MESSAGE(
			p_a.textured == p_a.sights, p_optic, ": ", p_a.textured, " of ", p_a.sights, " sight surfaces textured");
	CHECK_MESSAGE(p_a.roles == p_a.sights, p_optic, ": ", p_a.roles, " of ", p_a.sights,
			" sight surfaces sample their texture as an inverted mask when they multiply, else as a mask");
	CHECK_MESSAGE(p_a.single > 0, p_optic, ": no sight texture stored with one level");
	CHECK_MESSAGE(p_a.single_kept == p_a.single, p_optic, ": ", p_a.single_kept, " of ", p_a.single,
			" sight textures stored with one level are drawn from it alone");
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
			e.gadget = item.kind == render::ViewmodelItemKind::Gadget;
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
		std::set<std::string> gadget_slots, gadget_items;
		for (const CatalogEntry &e : entries) {
			if (std::find(host::kGadgetSlots.begin(), host::kGadgetSlots.end(), e.weapon.slot) !=
					host::kGadgetSlots.end()) {
				gadget_slots.insert(e.weapon.unlock);
			}
			if (e.gadget) {
				gadget_items.insert(e.weapon.unlock);
			}
		}
		CHECK(gadget_slots == gadget_slot_weapons());
		CHECK(gadget_items == kGadgetItems);
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
		std::set<std::string> gadget_parts;
		for (const CrowdEntry &e : entries) {
			const bool gadget_slot = std::find(host::kGadgetSlots.begin(), host::kGadgetSlots.end(), e.weapon.slot) !=
					host::kGadgetSlots.end();
			if (gadget_slot && e.error.empty() && e.set != nullptr) {
				gadget_parts.insert(e.weapon.unlock);
			}
		}
		CHECK(gadget_parts == gadget_slot_weapons());
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
	std::string camo;
	std::vector<std::string> light_prefabs;
	size_t lights = 0;
	std::string error;
	std::string facts;
};

constexpr const char *kAccessoryGap = "an unlock that no SocketData, WeaponModifierData or GunSwayModifierData names";
constexpr const char *kCamouflageGap = " (camouflage)";

std::array<std::string, 2> weapon_meshes(const core::EbxResolver &p_db, const core::EbxInstance &p_swd) {
	std::array<std::string, 2> out;
	const std::optional<core::EbxValue> states = p_swd.get("WeaponStates");
	const core::OptionalEbxArray arr = states ? states->as_array() : core::OptionalEbxArray();
	const std::optional<core::EbxValue> s0 = arr && arr->size() > 0 ? arr->get(0) : std::nullopt;
	const std::optional<core::EbxStruct> st = s0 ? s0->as_struct() : std::nullopt;
	for (size_t k = 0; st && k < out.size(); ++k) {
		if (const std::optional<core::EbxInstance> m = p_db.follow(*st, k == 0 ? "Mesh1p" : "Mesh3p")) {
			out[k] = std::string(p_db.name_of(m->partition().guid()).value_or(std::string_view()));
		}
	}
	return out;
}

std::string camo_textures(const core::EbxLibrary &p_lib, const std::array<std::string, 2> &p_meshes,
		const render::WeaponCamo &p_camo, const std::vector<std::string> &p_databases, std::string &r_error) {
	std::string out;
	for (const std::string &mesh : p_meshes) {
		core::Result<render::MeshMaterials> m = render::mesh_materials(p_lib, mesh, p_camo.variation_hash, p_databases);
		if (!m) {
			r_error = "camo " + p_camo.variation + ": " + m.error().message;
			return out;
		}
		if (m->variation_hash != p_camo.variation_hash) {
			r_error = "camo " + p_camo.variation + ": " + mesh + " has no MeshVariationDatabaseEntry for it";
			return out;
		}
		std::string texture;
		for (const render::MaterialSlot &slot : m->slots) {
			texture = texture.empty() ? slot.texture(render::kCamoTextureParameter) : texture;
		}
		if (texture.empty()) {
			r_error = "camo " + p_camo.variation + ": no slot of " + mesh + " names a Camo texture";
			return out;
		}
		out += (out.empty() ? "" : ", ") + mesh + " " + texture;
	}
	return out;
}

std::string accessory_gap(const AccessoryRow &p_row) {
	if (p_row.sockets != 0 || p_row.modifiers != 0 || !p_row.camo.empty() ||
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
		const std::vector<std::string> databases = render::variation_database_names(lib);
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
			const std::array<std::string, 2> meshes = weapon_meshes(lib, swd.front());
			for (size_t a = 0; a < host::kLoadoutAccessories; ++a) {
				bool camo_slot = false;
				for (const std::string &unlock : w.item->accessory_unlocks[a]) {
					camo_slot |= render::weapon_camo(lib, unlock, base->blueprint).has_value();
				}
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
							for (const std::string *prefab : { &o.prefab_1p, &o.prefab_3p }) {
								if (prefab->empty()) {
									continue;
								}
								core::Result<std::vector<core::level::SpotLight>> l =
										render::read_prefab_spot_lights(lib, *prefab);
								if (!l) {
									row.error = l.error().message;
									continue;
								}
								row.lights += l->size();
								row.light_prefabs.push_back(core::to_lower_ascii(*prefab));
							}
						}
					}
					if (const std::optional<render::WeaponCamo> camo =
									render::weapon_camo(lib, unlock, base->blueprint)) {
						row.camo = camo->variation;
						row.facts = "camo " + camo_textures(lib, meshes, *camo, databases, row.error) + "; ";
					} else if (camo_slot && render::unlock_links_nothing(lib, unlock)) {
						row.camo = "the default materials";
						row.facts = "camo: links nothing, the default materials; ";
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
						row.facts += "zoom fov " + std::to_string(int(d->zoom_render_fov)) + ", controller " +
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
		size_t rows = 0, with_mesh = 0, with_modifiers = 0, camos = 0, default_camos = 0, with_lights = 0;
		std::map<std::string, size_t> prefab_rows;
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
				camos += !r.camo.empty() && r.camo.starts_with("weapons/") ? 1u : 0u;
				with_lights += r.lights != 0 ? 1u : 0u;
				for (const std::string &prefab : r.light_prefabs) {
					++prefab_rows[prefab];
				}
				default_camos += !r.camo.empty() && !r.camo.starts_with("weapons/") ? 1u : 0u;
				const std::string verdict =
						!error.empty() ? "FAIL " + error : (!gap.empty() ? "data gap: " + gap : std::string("ok"));
				std::printf("render.catalog_accessories team %u %s slot %zu %s: %s; %zu sockets, %zu meshes, %zu "
							"lights, %zu modifiers; %s\n",
						unsigned(r.team), r.weapon.c_str(), r.slot + 1, r.unlock.c_str(), verdict.c_str(), r.sockets,
						r.meshes.size(), r.lights, r.modifiers, r.facts.c_str());
				if (!error.empty()) {
					failed.push_back(r.weapon + " " + r.unlock + ": " + error);
				} else if (!gap.empty()) {
					++gaps[gap];
				}
			}
		}
		MESSAGE(rows, " accessories of ", weapons.size(), " (team, primary) pairs: ", with_mesh,
				" with socket meshes (", meshes.size(), " distinct), ", with_modifiers, " with weapon modifiers, ",
				camos, " camouflages with their variation, ", default_camos,
				" that link nothing (the default materials), ", with_lights, " with spot lights; setup ", s_setup,
				" s, rows ", s_rows, " s");
		for (const auto &[gap, n] : gaps) {
			MESSAGE("data gap: ", n, " accessories: ", gap);
		}
		for (const auto &[prefab, n] : prefab_rows) {
			MESSAGE("socket prefab ", prefab, ": ", n, " accessories");
		}
		CHECK(with_lights == 244);
		CHECK(prefab_rows ==
				std::map<std::string, size_t>{ { "weapons/accessories/anpeq2/anpeq2", 122 },
						{ "weapons/accessories/anpeq2/anpeq2_3p", 102 },
						{ "weapons/accessories/anpeq2/pistol_laser", 20 },
						{ "weapons/accessories/flashlight/flashlight_1p", 122 },
						{ "weapons/accessories/flashlight/flashlight_3p", 124 },
						{ "weapons/accessories/scopeglint/scopeglint", 318 } });
		core::Result<std::vector<core::level::SpotLight>> flashlight =
				render::read_prefab_spot_lights(lib, "weapons/accessories/flashlight/flashlight_1p");
		core::Result<std::vector<core::level::SpotLight>> laser =
				render::read_prefab_spot_lights(lib, "weapons/accessories/anpeq2/anpeq2");
		REQUIRE(flashlight.ok());
		REQUIRE(laser.ok());
		REQUIRE(flashlight->size() == 1);
		REQUIRE(laser->size() == 1);
		const core::level::SpotLight &f = flashlight->front();
		CHECK((f.color == core::Vec3{ 0.789f, 0.708f, 0.582f }));
		CHECK(f.intensity == 20.0f);
		CHECK(f.radius == 30.0f);
		CHECK(f.shape == core::level::SpotLightShape::Frustum);
		CHECK(render::spot_light_half_angle_deg(f) == 10.0f);
		CHECK(f.cast_shadows);
		CHECK(core::to_lower_ascii(f.texture) == "weapons/accessories/flashlight/flashlightprojection_01");
		CHECK(f.local.forward[2] == -1.0f);
		const core::level::SpotLight &l = laser->front();
		CHECK((l.color == core::Vec3{ 1.0f, 0.0f, 0.0f }));
		CHECK(l.intensity == 100.0f);
		CHECK(l.radius == 500.0f);
		CHECK(render::spot_light_half_angle_deg(l) == doctest::Approx(0.11f));
		CHECK_FALSE(l.cast_shadows);
		CHECK(core::to_lower_ascii(l.texture) == "weapons/accessories/targetpointer/laserprojection");
		CHECK(rows == listed);
		CHECK(gaps == std::map<std::string, size_t>{ { kAccessoryGap, 14 } });
		CHECK(camos == 76);
		CHECK(default_camos == 38);
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
		CHECK(b.lights.empty());
		CHECK(b.weapon.variation_hash == 0);
		uint8_t m416_team = 0;
		const host::LoadoutItem *m416 = catalog_primary(level.loadouts, "weapons/m416/u_m416", m416_team);
		REQUIRE(m416 != nullptr);
		std::array<uint32_t, host::kLoadoutAccessories> m416_picks{};
		for (size_t slot = 0; slot < host::kLoadoutAccessories; ++slot) {
			const uint32_t light = accessory_ending(*m416, slot, "_flashlight");
			m416_picks[slot] = light != 0 ? light : accessory_ending(*m416, slot, "_camo_1");
		}
		render::ViewmodelSetSpec lit =
				render::team_viewmodel_items(m416_team, std::span<const std::string>(&m416->unlock, 1));
		lit.superbundle = kLevel;
		const host::LoadoutAccessorySet lit_set = host::loadout_accessory_set(*m416, m416_picks);
		lit.items[0].accessories = render::WeaponAccessories{ lit_set.active, lit_set.listed };
		core::Result<std::unique_ptr<render::ViewmodelSet>> m416_set =
				render::load_viewmodel_set(**sources, lit, nullptr);
		REQUIRE_MESSAGE(m416_set.ok(), (m416_set.ok() ? "" : m416_set.error().message));
		const render::ViewmodelItem &c = (*m416_set)->items[0];
		REQUIRE(c.lights.size() == 1);
		const render::ViewmodelLight &light = c.lights.front();
		MESSAGE("M416 flashlight: ", light.socket.prefab, " on ", light.socket.bone, " (rig bone ", light.rig_bone,
				"), projector ", light.socket.light.texture, "; camo variation ", c.weapon.variation_hash);
		CHECK(light.rig_bone >= 0);
		CHECK(light.socket.socket_unlock == "weapons/m416/u_m416_flashlight");
		CHECK(light.socket.light.intensity == 20.0f);
		CHECK((*m416_set)->texture(light.socket.light.texture) != nullptr);
		CHECK(light.socket.light.cast_shadows_min_level == 3);
		CHECK_FALSE(core::level::spot_light_shadows(light.socket.light, 2));
		CHECK(core::level::spot_light_shadows(light.socket.light, 3));
		std::vector<std::string> toggled;
		for (const render::ViewmodelAttachment &a : c.attachments) {
			if (a.light_toggle) {
				toggled.push_back(core::to_lower_ascii(a.part.mesh));
			}
		}
		CHECK(toggled == std::vector<std::string>{ "weapons/accessories/flashlight/flashlightbeam_1p_mesh" });
		CHECK(c.weapon.variation_hash == 1200125952u);
		CHECK(c.weapon.materials.variation_hash == 1200125952u);
		std::string camo;
		for (const render::MaterialSlot &slot : c.weapon.materials.slots) {
			camo = camo.empty() ? slot.texture(render::kCamoTextureParameter) : camo;
		}
		CHECK(core::to_lower_ascii(camo) == "characters/shared/xp2_clothcamo/abu");
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
		CHECK(b.materials.variation_hash == 0);
		uint8_t m416_team = 0;
		const host::LoadoutItem *m416 = catalog_primary(level.loadouts, "weapons/m416/u_m416", m416_team);
		REQUIRE(m416 != nullptr);
		std::array<uint32_t, host::kLoadoutAccessories> camo_picks{};
		for (size_t slot = 0; slot < host::kLoadoutAccessories; ++slot) {
			camo_picks[slot] = accessory_ending(*m416, slot, "_camo_1");
		}
		render::CrowdWeaponSpec camo_item = godot_side::catalog_crowd_spec(level, m416->def);
		const host::LoadoutAccessorySet camo_set = host::loadout_accessory_set(*m416, camo_picks);
		camo_item.accessories = render::WeaponAccessories{ camo_set.active, camo_set.listed };
		std::array<std::span<const render::CrowdWeaponSpec>, 2> camo_items{};
		camo_items[0] = std::span<const render::CrowdWeaponSpec>(&camo_item, 1);
		core::Result<std::unique_ptr<render::CrowdWeaponSet>> camo =
				render::load_crowd_weapons(**ebx, spec, **assets, camo_items, nullptr);
		REQUIRE_MESSAGE(camo.ok(), (camo.ok() ? "" : camo.error().message));
		REQUIRE((*camo)->teams[0].size() == 1);
		const render::CrowdWeaponPart &c = (*camo)->teams[0][0];
		std::string camo_texture;
		for (const render::MaterialSlot &slot : c.materials.slots) {
			camo_texture = camo_texture.empty() ? slot.texture(render::kCamoTextureParameter) : camo_texture;
		}
		MESSAGE("M416 3p with camo 1: variation ", c.materials.variation_hash, ", Camo texture ", camo_texture);
		CHECK(c.materials.variation_hash == 1200125952u);
		CHECK(core::to_lower_ascii(camo_texture) == "characters/shared/xp2_clothcamo/abu");
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
		std::set<std::string> dark_housings;
		std::map<std::string, SightBrightness> brightness;
		size_t acog_marks = 0, faded_rings = 0;
		for (const OpticLoad &l : loads) {
			const std::string &optic = l.item->accessory_unlocks[0][l.index];
			REQUIRE_MESSAGE(l.set != nullptr, optic, ": ", l.error);
			const render::ViewmodelItem &vm = l.set->items[0];
			CHECK_MESSAGE(vm.sight.has_value(), optic, " has no sight picture");
			const SightAudit audit = audit_sight(*l.set, lib.mount(0), brightness);
			check_sight(audit, optic);
			for (const std::string &mesh : audit.dark_housings) {
				dark_housings.insert(optic + " " + mesh);
			}
			faded_rings += audit.faded_rings;
			if (optic.ends_with("_acog")) {
				acog_marks += audit.marks;
			}
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
		std::string housings;
		for (const std::string &h : dark_housings) {
			housings += " " + h;
		}
		MESSAGE(dark_housings.size(), " surfaces darkened to black with a black Specular texture:", housings);
		CHECK(dark_housings.count("weapons/sv98/u_sv98_ballistic_scope "
								  "weapons/accessories/ballistic_scope/ballisticscope_zoom_1p_mesh") == 1);
		MESSAGE(acog_marks, " marked texels under the ACOGs' multiplying sights, ", faded_rings,
				" multiplying rings fading by vertex colour over unmarked texels");
		CHECK(acog_marks > 0);
		CHECK(faded_rings > 0);
		CHECK(loads.size() >= 30);
		const core::SnapshotDiff diff = core::diff_snapshots(before, reader.snapshot());
		CHECK(diff.clean());
	}
}

TEST_SUITE("render-catalog-scope") {
	TEST_CASE("catalog scope: every sniper's optics read their zoom-mesh factors from WeaponStates[0] and the optic's "
			  "unlock; only an optic with factors and a multiply surround hides the rifle, the vertex colour is drawn "
			  "only on multiply rings") {
		core::InstallReader reader;
		if (!reader.locate({})) {
			test::skip_install_case(reader.describe().c_str());
			return;
		}
		core::Result<std::shared_ptr<core::Install>> install = core::Install::open(reader);
		REQUIRE(install.ok());
		auto gunplay = host::load_gunplay_level(
				*install, "MP_Subway", "TeamDeathMatch0", host::GunplayLoadOptions{ true, true });
		REQUIRE_MESSAGE(gunplay.ok(), (gunplay.ok() ? "" : gunplay.error().message));
		core::Result<std::unique_ptr<render::ViewmodelSources>> sources =
				render::open_viewmodel_sources(reader, kLevel, nullptr);
		REQUIRE_MESSAGE(sources.ok(), (sources.ok() ? "" : sources.error().message));
		const core::EbxLibrary &lib = *(*sources)->ebx;
		struct Sniper {
			std::string_view unlock;
			size_t hiding, surround_kept, clear;
		};
		constexpr std::array<Sniper, 11> kSnipers{ Sniper{ "weapons/mk11/u_mk11", 4, 3, 5 },
			Sniper{ "weapons/sv98/u_sv98", 4, 3, 5 }, Sniper{ "weapons/sks/u_sks", 4, 3, 5 },
			Sniper{ "weapons/m40a5/u_m40a5", 4, 3, 5 }, Sniper{ "weapons/model98b/u_m98b", 4, 3, 5 },
			Sniper{ "weapons/m39ebr/u_m39ebr", 4, 3, 5 }, Sniper{ "weapons/svd/u_svd_us", 4, 3, 5 },
			Sniper{ "weapons/xp1_qbu-88/u_qbu-88_sniper", 4, 3, 5 }, Sniper{ "weapons/xp1_l96/u_l96", 6, 3, 5 },
			Sniper{ "weapons/xp2_hk417/u_hk417", 4, 3, 5 }, Sniper{ "weapons/xp2_jng90/u_jng90", 4, 3, 5 } };
		constexpr std::array<std::pair<std::string_view, float>, 3> kZoomOut{
			std::pair{ "weapons/mk11/u_mk11_rifle_scope", 0.85f },
			std::pair{ "weapons/xp2_hk417/u_hk417_rifle_scope", 0.85f }, std::pair{ "weapons/svd/u_svd_irnv", 0.75f }
		};
		struct OpticLoad {
			size_t sniper = 0;
			uint8_t team = 0;
			const host::LoadoutItem *item = nullptr;
			size_t index = 0;
			bool has_factors = false;
			std::unique_ptr<render::ViewmodelSet> set;
			std::string error;
		};
		std::vector<OpticLoad> loads;
		for (size_t w = 0; w < kSnipers.size(); ++w) {
			uint8_t team = 0;
			const host::LoadoutItem *item = catalog_primary((*gunplay)->loadouts, kSnipers[w].unlock, team);
			REQUIRE_MESSAGE(item != nullptr, kSnipers[w].unlock);
			core::Result<std::vector<core::gameplay::WeaponUnlockModifier>> mods =
					core::gameplay::weapon_unlock_modifiers(lib, item->unlock, "WeaponAnimationConfigurationModifier");
			REQUIRE(mods.ok());
			for (size_t i = 0; i < item->accessories[0].size(); ++i) {
				if (item->accessory_unlocks[0][i].starts_with(render::kDefaultSocketUnlockPrefix)) {
					continue;
				}
				bool has_factors = false;
				for (const core::gameplay::WeaponUnlockModifier &m : *mods) {
					const std::optional<core::EbxValue> f = m.modifier.get("ZoomInOutMeshTransitionFactors");
					const core::OptionalEbxArray arr = f ? f->as_array() : core::OptionalEbxArray();
					has_factors |= m.unlock == item->accessory_unlocks[0][i] && arr && arr->size() > 0;
				}
				loads.push_back(OpticLoad{ w, team, item, i, has_factors, {}, {} });
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
		std::array<std::array<size_t, 3>, kSnipers.size()> counts{};
		size_t rings = 0, zero_mix = 0, irnv_reticles = 0;
		for (const OpticLoad &l : loads) {
			const std::string &optic = l.item->accessory_unlocks[0][l.index];
			REQUIRE_MESSAGE(l.set != nullptr, optic, ": ", l.error);
			const render::ViewmodelItem &vm = l.set->items[0];
			REQUIRE_MESSAGE(vm.sight.has_value(), optic);
			const render::ViewmodelSight &sight = *vm.sight;
			float want_out = 1.0f;
			for (const auto &[unlock, out] : kZoomOut) {
				want_out = optic == unlock ? out : want_out;
			}
			CHECK_MESSAGE(vm.zoom_mesh_in == doctest::Approx(1.0f), optic);
			CHECK_MESSAGE(vm.zoom_mesh_out == doctest::Approx(want_out), optic);
			CHECK_MESSAGE(sight.hides_weapon == (l.has_factors && sight.surround), optic);
			CHECK_MESSAGE((!l.has_factors || sight.surround), optic, " has factors but no multiply surround");
			++counts[l.sniper][sight.hides_weapon ? 0 : sight.surround ? 1 : 2];
			for (const render::ViewmodelAttachment &a : vm.attachments) {
				irnv_reticles += optic.ends_with("_irnv") && a.part.mesh.find("reticule") != std::string::npos &&
								render::viewmodel_scope_part(vm, a)
						? 1u
						: 0u;
				for (const render::PreparedSurface &s : a.part.prepared.surfaces) {
					if (s.material_index >= a.looks.size() || s.vertex_count() == 0 ||
							s.colors.size() < 4 * s.vertex_count()) {
						continue;
					}
					const render::ViewmodelSlotLook &look = a.looks[s.material_index];
					uint8_t lowest = 255, highest = 0;
					for (size_t k = 0; k < s.vertex_count(); ++k) {
						lowest = std::min(lowest, s.colors[4 * k]);
						highest = std::max(highest, s.colors[4 * k]);
					}
					if (render::viewmodel_slot_vertex_colour(look)) {
						++rings;
						CHECK_MESSAGE(lowest == 0, optic, " ", a.part.mesh, " ring has no black vertex");
						CHECK_MESSAGE(highest == 255, optic, " ", a.part.mesh, " ring has no clear vertex");
					} else if (look.blend && look.unlit) {
						zero_mix += lowest == 0 ? 1u : 0u;
					}
				}
			}
			if (optic.ends_with("rx01") || optic.ends_with("rx_01") || optic.ends_with("kobra") ||
					optic.ends_with("eotech") || optic.ends_with("pka") || optic.ends_with("acog") ||
					optic.ends_with("m145") || optic.ends_with("pso-1")) {
				CHECK_MESSAGE(!sight.hides_weapon, optic, " keeps the rifle");
			}
			if (optic.ends_with("ballistic_scope") || optic.ends_with("pks-07")) {
				CHECK_MESSAGE(sight.hides_weapon, optic, " hides the rifle");
			}
		}
		for (size_t w = 0; w < kSnipers.size(); ++w) {
			MESSAGE(kSnipers[w].unlock, ": ", counts[w][0], " optics hide the rifle, ", counts[w][1],
					" have a surround and keep it, ", counts[w][2], " have no surround");
			CHECK(counts[w][0] == kSnipers[w].hiding);
			CHECK(counts[w][1] == kSnipers[w].surround_kept);
			CHECK(counts[w][2] == kSnipers[w].clear);
		}
		CHECK(rings >= kSnipers.size() * 7);
		CHECK_MESSAGE(zero_mix >= kSnipers.size(), zero_mix,
				" unlit Mix slots hold a zero vertex colour (the PKS-07 and ballistic reticles) and draw without it");
		CHECK_MESSAGE(irnv_reticles >= kSnipers.size(), irnv_reticles, " IRNV reticles stay with the scope");
		render::ViewmodelItem item;
		item.zoom_mesh_in = 1.0f;
		item.zoom_mesh_out = 0.85f;
		CHECK_FALSE(render::viewmodel_zoom_mesh_shown(false, 0.99f, item));
		CHECK(render::viewmodel_zoom_mesh_shown(false, 1.0f, item));
		CHECK(render::viewmodel_zoom_mesh_shown(true, 0.9f, item));
		CHECK_FALSE(render::viewmodel_zoom_mesh_shown(true, 0.8f, item));
	}
}

TEST_SUITE("render-catalog-deployables") {
	TEST_CASE(
			"catalog deployables: each kit gadget of Metro TDM resolves its projectile's mesh, materials and textures, "
			"and the radio beacon the effect its OnSpawned starts") {
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
		core::Result<host::LevelDatabase> db =
				host::open_level_database(*install, "MP_Subway", "TeamDeathMatch0", 0, nullptr);
		REQUIRE_MESSAGE(db.ok(), (db.ok() ? "" : db.error().message));
		std::vector<render::DeployedGadgetSpec> specs;
		for (const sim::weapons::GadgetDef &g : level.gadgets) {
			REQUIRE(g.def < level.defs.size());
			specs.push_back(render::DeployedGadgetSpec{ g.def, level.defs[g.def].firing_data });
		}
		REQUIRE(specs.size() == 6);
		core::Result<std::unique_ptr<render::DeployedGadgetSet>> set =
				render::load_deployed_gadgets(db->level->db(), db->level->mount(), specs);
		REQUIRE_MESSAGE(set.ok(), (set.ok() ? "" : set.error().message));
		std::map<std::string, std::string> meshes;
		std::map<std::string, std::vector<std::string>> effects;
		for (const sim::weapons::GadgetDef &g : level.gadgets) {
			const render::DeployedGadgetPart *p = (*set)->part(g.def);
			REQUIRE(p != nullptr);
			MESSAGE(g.weapon, ": ", p->object.entity_type, " ", p->object.mesh, ", ", p->prepared.vertices,
					" vertices, ", p->prepared.surfaces.size(), " surfaces, ", p->object.effects.size(), " effects");
			meshes[g.weapon] = p->object.mesh;
			effects[g.weapon] = p->object.effects;
			CHECK(p->prepared.vertices > 0);
			CHECK_FALSE(p->prepared.skinned);
			for (const render::PreparedSurface &s : p->prepared.surfaces) {
				REQUIRE(s.material_index < p->materials.slots.size());
				const std::string diffuse = p->materials.slots[s.material_index].texture("Diffuse");
				CAPTURE(diffuse);
				CHECK((diffuse.empty() || (*set)->texture(diffuse) != nullptr));
			}
		}
		for (const std::string &n : (*set)->notes) {
			MESSAGE(n);
		}
		CHECK((*set)->notes.empty());
		CHECK(meshes ==
				std::map<std::string, std::string>{
						{ "weapons/gadgets/ammobag/ammobag", "weapons/gadgets/ammobag/ammobag_projectile_mesh" },
						{ "weapons/gadgets/c4/c4", "weapons/gadgets/c4/c4_projectile_mesh" },
						{ "weapons/gadgets/claymore/claymore", "weapons/gadgets/claymore/claymore_projectile_mesh" },
						{ "weapons/gadgets/m15/m15", "weapons/gadgets/m15/m15projectile_mesh" },
						{ "weapons/gadgets/medicbag/medicbag", "weapons/gadgets/medicbag/mediccrate_projectile_mesh" },
						{ "weapons/gadgets/radiobeacon/radio_beacon",
								"weapons/gadgets/radiobeacon/radiobeacon_projectile_mesh" } });
		CHECK(effects ==
				std::map<std::string, std::vector<std::string>>{ { "weapons/gadgets/ammobag/ammobag", {} },
						{ "weapons/gadgets/c4/c4", {} }, { "weapons/gadgets/claymore/claymore", {} },
						{ "weapons/gadgets/m15/m15", {} }, { "weapons/gadgets/medicbag/medicbag", {} },
						{ "weapons/gadgets/radiobeacon/radio_beacon",
								{ "sound/weapons/handheld/radiobeacon/radiobeacon_object_sfx" } } });
		const core::SnapshotDiff diff = core::diff_snapshots(before, reader.snapshot());
		std::printf("render.catalog_deployables install.unchanged %s\n", diff.clean() ? "yes" : diff.summary().c_str());
		CHECK(diff.clean());
	}
}

TEST_SUITE("render-catalog-optics-1p") {
	TEST_CASE("catalog optics: every optic of every primary weapon of Metro TDM's loadout catalog, per team, draws its "
			  "unlit sight surfaces as the M416's do: a multiplying one darkens the scene by its marks and keeps the "
			  "rest at the vertex colour, a mixing one draws its tint exactly where its texture is marked") {
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
			uint8_t team = 0;
			const host::LoadoutItem *item = nullptr;
			size_t index = 0;
			std::unique_ptr<render::ViewmodelSet> set;
			std::string error;
		};
		std::vector<OpticLoad> loads;
		std::set<std::pair<uint8_t, std::string>> seen;
		std::set<std::string> sight_meshes;
		size_t optics = 0;
		for (uint8_t team = 1; team <= 2; ++team) {
			for (uint8_t k = 0; k < host::kKitCount; ++k) {
				const host::LoadoutKit *kit = level.loadouts.kit(team, k);
				for (size_t w = 0; kit != nullptr && w < kit->slots[0].size(); ++w) {
					const host::LoadoutItem &item = kit->slots[0][w];
					if (!seen.insert({ team, item.unlock }).second) {
						continue;
					}
					core::Result<core::gameplay::WeaponDef> def = core::gameplay::resolve_weapon(lib, item.unlock);
					const core::EbxPartition *bp = def ? lib.partition_by_name(def->blueprint) : nullptr;
					REQUIRE_MESSAGE(bp != nullptr, item.unlock);
					const std::vector<render::WeaponSocket> sockets =
							render::read_weapon_sockets(lib, bp->instances_of("SoldierWeaponData").front());
					for (size_t i = 0; i < item.accessories[0].size(); ++i) {
						const std::string &optic = item.accessory_unlocks[0][i];
						if (optic.starts_with(render::kDefaultSocketUnlockPrefix)) {
							continue;
						}
						++optics;
						std::string meshes;
						for (const render::WeaponSocket &s : sockets) {
							if (s.unlock != optic) {
								continue;
							}
							for (const render::WeaponSocketObject &o : s.objects) {
								meshes += o.mesh_1p + "|" + o.mesh_1p_zoom + "|";
							}
						}
						REQUIRE_MESSAGE(!meshes.empty(), optic, " has no 1p mesh in its weapon's sockets");
						if (sight_meshes.insert(meshes).second) {
							loads.push_back(OpticLoad{ team, &item, i, {}, {} });
						}
					}
				}
			}
		}
		std::map<std::string, SightBrightness> brightness;
		std::map<std::string, size_t> multiply_by_optic;
		size_t multiply_slots = 0, mixing_slots = 0, faded_rings = 0, marks = 0;
		for (size_t first = 0; first < loads.size(); first += kOpticBatch) {
			const size_t count = std::min(kOpticBatch, loads.size() - first);
			core::parallel_for(count, [&](size_t i) {
				OpticLoad &l = loads[first + i];
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
			for (size_t i = first; i < first + count; ++i) {
				OpticLoad &l = loads[i];
				const std::string &optic = l.item->accessory_unlocks[0][l.index];
				REQUIRE_MESSAGE(l.set != nullptr, optic, ": ", l.error);
				const SightAudit audit = audit_sight(*l.set, lib.mount(0), brightness);
				check_sight(audit, optic);
				multiply_slots += audit.multiply_slots;
				mixing_slots += audit.sights - audit.multiply_slots;
				faded_rings += audit.faded_rings;
				marks += audit.marks;
				if (audit.multiply_slots > 0) {
					multiply_by_optic[optic.substr(optic.rfind('_') + 1)] += 1;
				}
				l.set.reset();
			}
		}
		std::string families;
		for (const auto &[name, n] : multiply_by_optic) {
			families += " " + name + " " + std::to_string(n);
		}
		MESSAGE(optics, " optics of ", seen.size(), " team primaries, ", loads.size(),
				" distinct 1p optic meshes: ", multiply_slots, " multiplying sight surfaces (", marks,
				" marked texels, ", faded_rings, " rings fading by vertex colour), ", mixing_slots,
				" mixing; optics with a multiplying sight by unlock suffix:", families);
		CHECK(optics >= 1000);
		CHECK(multiply_by_optic.count("acog") == 1);
		CHECK(multiply_by_optic.count("m145") == 1);
		const core::SnapshotDiff diff = core::diff_snapshots(before, reader.snapshot());
		CHECK(diff.clean());
	}
}

TEST_SUITE("render-vehicle-meshes") {
	TEST_CASE("vehicle meshes: MP_007 Conquest's Growler, VDV Buggy, T-90 and M1A2 load split by part, each part "
			  "placed by its mesh part transform, and each wheel has its own part at the wheel's data centre") {
		core::InstallReader reader;
		if (!reader.locate({})) {
			test::skip_install_case(reader.describe().c_str());
			return;
		}
		const core::InstallSnapshot before = reader.snapshot();
		core::Result<std::shared_ptr<core::Install>> install = core::Install::open(reader);
		REQUIRE(install.ok());
		core::Result<host::LevelDatabase> db =
				host::open_level_database(*install, "MP_007", "ConquestSmall0", 0, nullptr);
		REQUIRE_MESSAGE(db.ok(), (db.ok() ? "" : db.error().message));
		core::Result<std::shared_ptr<const host::LevelVehicles>> level =
				host::load_level_vehicles(*db, sim::VehicleTuning{});
		REQUIRE_MESSAGE(level.ok(), (level.ok() ? "" : level.error().message));
		std::vector<render::VehicleMeshSpec> specs;
		for (const host::LevelVehicleKind &k : (*level)->kinds) {
			specs.push_back(render::VehicleMeshSpec{ k.blueprint, k.mesh, k.component_meshes });
		}
		REQUIRE(specs.size() == 4);
		core::Result<std::unique_ptr<render::VehicleMeshSet>> set =
				render::load_vehicle_meshes(db->level->db(), db->level->mount(), specs);
		REQUIRE_MESSAGE(set.ok(), (set.ok() ? "" : set.error().message));
		for (const host::LevelVehicleKind &k : (*level)->kinds) {
			const render::VehicleMeshKind *mk = (*set)->kind(k.blueprint);
			REQUIRE(mk != nullptr);
			std::vector<render::VehicleWheelRest> wheels;
			for (size_t w = 0; w < k.model->wheel_count; ++w) {
				wheels.push_back(render::VehicleWheelRest{ k.model->wheels[w].data_centre, k.model->wheels[w].radius });
			}
			const std::vector<int8_t> wheel_of = render::vehicle_wheel_parts(*mk, wheels);
			std::set<int8_t> matched;
			size_t placed = 0, components = 0, vertices = 0;
			for (size_t p = 0; p < mk->parts.size(); ++p) {
				const render::VehicleMeshPart &part = mk->parts[p];
				vertices += part.prepared.vertices;
				components += part.component ? 1u : 0u;
				const core::Vec3 &t = part.part_transform.trans;
				placed += (t[0] != 0.0f || t[1] != 0.0f || t[2] != 0.0f) ? 1u : 0u;
				if (wheel_of[p] >= 0) {
					matched.insert(wheel_of[p]);
					const core::Vec3 &c = k.model->wheels[size_t(wheel_of[p])].data_centre;
					std::printf("vehicle meshes: %s wheel %d is part %u at (%.2f %.2f %.2f), the wheel's data centre "
								"(%.2f %.2f %.2f), radius %.2f, part half height %.2f\n",
							k.blueprint.c_str(), int(wheel_of[p]), unsigned(part.part), double(t[0]), double(t[1]),
							double(t[2]), double(c[0]), double(c[1]), double(c[2]),
							double(k.model->wheels[size_t(wheel_of[p])].radius),
							double(0.5f * (part.part_bounds_max[1] - part.part_bounds_min[1])));
				}
			}
			std::printf("vehicle meshes: %s: %zu parts (%zu from component meshes, %zu with a part transform off the "
						"origin), %zu vertices, %zu of %u wheels matched\n",
					k.blueprint.c_str(), mk->parts.size(), components, placed, vertices, matched.size(),
					unsigned(k.model->wheel_count));
			CHECK(matched.size() == k.model->wheel_count);
			CHECK(placed > mk->parts.size() / 2);
			size_t riding = 0;
			std::set<int> pivoted;
			for (size_t p = 0; p < mk->parts.size() && k.armament != nullptr; ++p) {
				const int t = present::part_turret(
						k.part_turrets, k.component_mesh_turrets, mk->part_mesh[p], mk->parts[p].part);
				if (t < 0 || size_t(t) >= k.armament->turrets.size()) {
					continue;
				}
				++riding;
				const core::Vec3 pivot = sim::vehicles::turret_world(
						*k.armament, sim::vehicles::ArmamentState{}, core::Transform::identity(), size_t(t))
												 .trans;
				const core::Vec3 &at = mk->parts[p].part_transform.trans;
				if (!mk->parts[p].component && core::vec::length(core::vec::sub(at, pivot)) < 0.05f) {
					pivoted.insert(t);
					std::printf("vehicle meshes: %s part %u sits at turret %d's pivot (%.3f %.3f %.3f)\n",
							k.blueprint.c_str(), unsigned(mk->parts[p].part), t, double(pivot[0]), double(pivot[1]),
							double(pivot[2]));
				}
			}
			std::printf("vehicle meshes: %s: %zu parts ride a turret, %zu of %zu turrets have a part at their pivot\n",
					k.blueprint.c_str(), riding, pivoted.size(),
					k.armament != nullptr ? k.armament->turrets.size() : size_t(0));
			if (k.blueprint == "vehicles/m1a2/m1abrams" || k.blueprint == "vehicles/t90/t90") {
				CHECK(riding >= 10);
				CHECK(pivoted.size() >= 3);
			}
			std::string unheld;
			for (size_t p = 0; p < mk->parts.size(); ++p) {
				if (mk->part_mesh[p] == 0 &&
						std::find(k.unheld_parts.begin(), k.unheld_parts.end(), mk->parts[p].part) !=
								k.unheld_parts.end()) {
					unheld += " " + std::to_string(mk->parts[p].part);
				}
			}
			std::printf(
					"vehicle meshes: %s: parts of unheld unlocks not drawn:%s\n", k.blueprint.c_str(), unheld.c_str());
			const core::gameplay::VehicleCameraDef *gunner = nullptr;
			for (size_t c = 0; !k.seat_defs.empty() && c < k.seat_defs[0].cameras.size(); ++c) {
				const core::gameplay::VehicleCameraDef &camera = k.seat_defs[0].cameras[c];
				gunner = gunner == nullptr && camera.first_person && camera.turret >= 0 ? &camera : gunner;
			}
			if (k.blueprint == "vehicles/m1a2/m1abrams") {
				REQUIRE(gunner != nullptr);
				const core::Vec3 eye = gunner->transform.trans;
				std::vector<unsigned> blocking;
				for (size_t p = 0; p < mk->parts.size(); ++p) {
					const render::VehicleMeshPart &part = mk->parts[p];
					if (mk->part_mesh[p] == 0 &&
							std::find(k.unheld_parts.begin(), k.unheld_parts.end(), part.part) !=
									k.unheld_parts.end()) {
						continue;
					}
					core::Vec3 lo{ 1e9f, 1e9f, 1e9f }, hi{ -1e9f, -1e9f, -1e9f };
					for (int corner = 0; corner < 8; ++corner) {
						const core::Vec3 q{ (corner & 1) != 0 ? part.bounds_max[0] : part.bounds_min[0],
							(corner & 2) != 0 ? part.bounds_max[1] : part.bounds_min[1],
							(corner & 4) != 0 ? part.bounds_max[2] : part.bounds_min[2] };
						const core::Vec3 w = part.rest.transform_point(part.part_transform.transform_point(q));
						for (size_t a = 0; a < 3; ++a) {
							lo[a] = std::min(lo[a], w[a]);
							hi[a] = std::max(hi[a], w[a]);
						}
					}
					const auto inside = [&](const core::Vec3 &p_at) {
						return p_at[0] >= lo[0] && p_at[0] <= hi[0] && p_at[1] >= lo[1] && p_at[1] <= hi[1] &&
								p_at[2] >= lo[2] && p_at[2] <= hi[2];
					};
					bool ahead = false;
					for (int step = 1; step <= 60 && !inside(eye); ++step) {
						ahead = ahead || inside(core::Vec3{ eye[0], eye[1], eye[2] + 0.05f * float(step) });
					}
					if (ahead) {
						blocking.push_back(part.part);
					}
				}
				std::printf("vehicle meshes: %zu drawn parts lie within 3 m ahead of the M1A2 gunner's first-person "
							"camera at (%.3f %.3f %.3f)\n",
						blocking.size(), double(eye[0]), double(eye[1]), double(eye[2]));
				CHECK(blocking.empty());
			}
			REQUIRE(mk->looks.size() == mk->materials.size());
			size_t glass = 0, blended = 0;
			for (size_t m = 0; m < mk->materials.size(); ++m) {
				REQUIRE(mk->looks[m].size() == mk->materials[m].slots.size());
				for (size_t i = 0; i < mk->looks[m].size(); ++i) {
					const render::LookMaterial &look = mk->looks[m][i];
					glass += look.glass ? 1u : 0u;
					blended += look.blend == render::LookBlend::Blend ? 1u : 0u;
					std::printf("vehicle meshes: %s mesh %zu slot %zu shader %s: blend %d glass %d double sided %d "
								"drawn %d\n",
							k.blueprint.c_str(), m, i, mk->materials[m].slots[i].shader.c_str(), int(look.blend),
							int(look.glass), int(look.double_sided), int(look.drawn));
				}
			}
			if (k.blueprint == "vehicles/growleritv/growleritv") {
				CHECK_MESSAGE(glass > 0, "the Growler's windscreen slots resolve to glass");
				CHECK(blended >= glass);
			}
		}
		for (const std::string &n : (*set)->notes) {
			MESSAGE(n);
		}
		const core::SnapshotDiff diff = core::diff_snapshots(before, reader.snapshot());
		CHECK(diff.clean());
	}
}
