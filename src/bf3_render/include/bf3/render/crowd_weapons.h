#pragma once
// The 3p weapon of every kit item for the crowd, as its own skinned mesh (M2a wave 3; docs/design/m2a.md §9, R5;
// W3.0 contract, docs/design/m2a-w3.md §3 "tp's 3p weapon route"; owner tp, which writes load_crowd_weapons in its new
// src/crowd_weapons.cpp). Godot-free: it runs on the loader thread after load_soldier_assets.
//
// Why a separate set: S3's SoldierAssets bakes one weapon into each team (the last SoldierPart of SoldierTeam, its binds
// in the team's one SkinBinds list; SoldierSpec names one weapon per team), and the SkinUploader draws one combined mesh
// per team. M2a draws the kit item each soldier holds, so, without editing soldier_assets.* or skin_upload.* (S3's):
//   1. load_soldier_assets as S3 does (both teams, the body and the default weapon);
//   2. load_crowd_weapons: per team, every item's 3p mesh with its own SkinBinds (weaponske01 bound into the rig, as S3
//      binds its weapon part), the reload clip by the item's chooser index, the weapon layout of step 3 and the 3p
//      muzzle (Wep_Root and the FireEffects3p offset, as G6 does for the 1p muzzle when there is no muzzle joint);
//   3. strip_team_weapon_parts on the loaded SoldierAssets before SkinUploader::build, so the team meshes draw the body,
//      head and helmet only (the uploader iterates SoldierTeam::parts; the team's binds and palette stay as they are);
//   4. crowd_weapons_of gives the CrowdTeam::weapons views for CrowdEvaluator::make;
//   5. per frame, Bf3GameCrowd (tp, game_crowd.cpp) draws each soldier's weapon with its own RS mesh per item and one RS
//      skeleton per soldier sized from that item's binds, from CrowdEvaluator::weapon_of / weapon_palette.
// Item 0 of each team is the team's SoldierSpec weapon (M16A4, AK-74M), so index 0 means what it means in S3.

#include "bf3/anim/crowd_eval.h"
#include "bf3/render/soldier_assets.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bf3::render {

// One kit item's 3p weapon, as the loader asks for it (tp fills these from the level's gunplay defs and kits).
struct CrowdWeaponSpec {
	uint16_t def = 0xFFFF; // the match def id (sim::weapons::kNoWeapon when not a match def): the def -> index key
	std::string mesh; // 3p SkinnedMeshAsset (e.g. weapons/m16a4/m16a4_3p_mesh)
	std::string weapon_unlock; // SoldierWeaponData partition: WeaponStates[0].Weapon (AntRef) -> reload chooser index
	std::string weapon_def; // WeaponDef source (reload time -> reload_rate)
};

// The prepared item (owned by CrowdWeaponSet; the anim::CrowdWeapon views point into it).
struct CrowdWeaponPart {
	CrowdWeaponSpec spec;
	MeshMaterials materials;
	PreparedMesh prepared; // LOD SoldierSpec::lod, bones = indices into binds
	core::CachedItem cache;
	anim::SkinBinds binds; // this weapon only (weaponske01 bound into the rig)
	anim::BoneMask weapon_joints; // crowd_eval.h step 3, rig-sized (empty: no fix)
	std::vector<anim::Xform> weapon_local; // rig-sized
	bool have_reload = false; // reload holds a clip
	anim::CrowdClipData reload; // load_chosen_clip(reload chooser, the item's index)
	float reload_rate = 1.0f;
	int32_t muzzle_joint = -1; // rig joint (-1: none, the FX triggers use the eye fallback)
	anim::Xform muzzle_offset = anim::xform_identity(); // the effect frame: +Y along the barrel (m2a-fx2.md)
	uint64_t vertices = 0, triangles = 0;
};

struct CrowdWeaponSet {
	std::array<std::vector<CrowdWeaponPart>, 2> teams; // crowd team 0 US, 1 RU; <= anim::kMaxCrowdWeapons each
	std::vector<PreparedTexture> textures; // the weapon surfaces' textures that SoldierAssets::textures lacks
	std::vector<std::string> notes;
	double seconds = 0.0;

	const PreparedTexture *texture(const std::string &p_name) const {
		for (const PreparedTexture &t : textures) {
			if (t.name == p_name) {
				return &t;
			}
		}
		return nullptr;
	}
};

// tp's body. p_items[t] are team t's items; item 0 should be the team's SoldierSpec weapon. The reload clips bind into
// p_assets.clips and p_assets.library, so p_assets must outlive the result. A team with more than
// anim::kMaxCrowdWeapons items, a mesh that does not resolve or a bind count over anim::kMaxPaletteBinds fails the load;
// a missing reload clip, reload time or muzzle is a note.
core::Result<std::unique_ptr<CrowdWeaponSet>> load_crowd_weapons(const core::InstallReader &p_install, const SoldierSpec &p_spec,
		const SoldierAssets &p_assets, const std::array<std::span<const CrowdWeaponSpec>, 2> &p_items, core::Cache *p_cache = nullptr);

// Step 3 above: removes the weapon SoldierPart (SoldierPart::weapon) of every team and subtracts its vertices and
// triangles, so a SkinUploader built afterwards draws the body only. Returns the parts removed. The team's binds,
// weapon layout and reload stay (S3's CrowdTeam fields keep working).
inline uint32_t strip_team_weapon_parts(SoldierAssets &r_assets) {
	uint32_t removed = 0;
	for (SoldierTeam &team : r_assets.teams) {
		for (size_t p = team.parts.size(); p-- > 0;) {
			if (!team.parts[p].weapon) {
				continue;
			}
			team.vertices -= std::min<uint64_t>(team.vertices, team.parts[p].prepared.vertices);
			team.triangles -= std::min<uint64_t>(team.triangles, team.parts[p].prepared.triangles);
			team.parts.erase(team.parts.begin() + std::ptrdiff_t(p));
			++removed;
		}
	}
	return removed;
}

// Step 4: the CrowdTeam::weapons views of team p_team (empty for a team past the end). They point into p_set.
inline std::vector<anim::CrowdWeapon> crowd_weapons_of(const CrowdWeaponSet &p_set, uint32_t p_team) {
	std::vector<anim::CrowdWeapon> out;
	if (p_team >= p_set.teams.size()) {
		return out;
	}
	for (const CrowdWeaponPart &part : p_set.teams[p_team]) {
		if (out.size() >= anim::kMaxCrowdWeapons) {
			break;
		}
		anim::CrowdWeapon w;
		w.binds = &part.binds;
		w.reload = part.have_reload ? &part.reload : nullptr;
		w.reload_rate = part.reload_rate;
		w.weapon_joints = part.weapon_joints.weight.empty() ? nullptr : &part.weapon_joints;
		w.weapon_local = part.weapon_local;
		w.muzzle_joint = part.muzzle_joint;
		w.muzzle_offset = part.muzzle_offset;
		out.push_back(w);
	}
	return out;
}

// The def -> CrowdSoldierInput::weapon table: the index of p_def among team p_team's items, or -1 (Bf3GameCrowd then
// draws item 0 and counts GameCrowdStats::unknown_weapons).
inline int32_t crowd_weapon_index(const CrowdWeaponSet &p_set, uint32_t p_team, uint16_t p_def) {
	if (p_team >= p_set.teams.size()) {
		return -1;
	}
	const std::vector<CrowdWeaponPart> &items = p_set.teams[p_team];
	for (size_t i = 0; i < items.size() && i < anim::kMaxCrowdWeapons; ++i) {
		if (items[i].spec.def == p_def) {
			return int32_t(i);
		}
	}
	return -1;
}

} // namespace bf3::render
