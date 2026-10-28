#pragma once

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

struct CrowdWeaponSpec {
	uint16_t def = 0xFFFF;
	std::string mesh;
	std::string weapon_unlock;
	std::string weapon_def;
};

struct CrowdWeaponPart {
	CrowdWeaponSpec spec;
	MeshMaterials materials;
	PreparedMesh prepared;
	core::CachedItem cache;
	anim::SkinBinds binds;
	anim::BoneMask weapon_joints;
	std::vector<anim::Xform> weapon_local;
	bool have_reload = false;
	anim::CrowdClipData reload;
	float reload_rate = 1.0f;
	bool have_throw = false;
	anim::CrowdThrowClips throw_clips;
	int32_t muzzle_joint = -1;
	anim::Xform muzzle_offset = anim::xform_identity();
	uint64_t vertices = 0, triangles = 0;
};

struct CrowdWeaponSet {
	std::array<std::vector<CrowdWeaponPart>, 2> teams;
	std::vector<PreparedTexture> textures;
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

core::Result<std::unique_ptr<CrowdWeaponSet>> load_crowd_weapons(const core::InstallReader &p_install, const SoldierSpec &p_spec,
		const SoldierAssets &p_assets, const std::array<std::span<const CrowdWeaponSpec>, 2> &p_items, core::Cache *p_cache = nullptr);
core::Result<std::unique_ptr<CrowdWeaponSet>> load_crowd_weapons(const core::EbxLibrary &p_library,
		const SoldierSpec &p_spec, const SoldierAssets &p_assets,
		const std::array<std::span<const CrowdWeaponSpec>, 2> &p_items, core::Cache *p_cache = nullptr);

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
		w.throw_clips = part.have_throw ? &part.throw_clips : nullptr;
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

}
