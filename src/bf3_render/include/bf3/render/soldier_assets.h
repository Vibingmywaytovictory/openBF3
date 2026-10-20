#pragma once

#include "bf3/anim/crowd_clips.h"
#include "bf3/anim/layers.h"
#include "bf3/anim/skeleton.h"
#include "bf3/anim/skinning.h"
#include "bf3/anim/temporal_lod.h"
#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/result.h"
#include "bf3/render/material_lookup.h"
#include "bf3/render/mesh_prep.h"
#include "bf3/render/texture_prep.h"
#include "bf3/render/viewmodel_assets.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bf3::render {

struct SoldierTeamSpec {
	std::string appearance;
	std::string weapon_mesh;
	std::string weapon_unlock;
	std::string weapon_def;

	std::string team_data;
};

struct SoldierSpec {
	std::string superbundle = "Win32/Levels/MP_Subway/MP_Subway";
	std::string settings_superbundle = "Win32/default_settings_win32";
	std::string settings_partition = "default/settings_win32";
	std::string body_skeleton = "animations/skeletons/veniceantske01";
	std::string weapon_skeleton = "animations/skeletons/weapon/weaponske01";
	uint32_t lod = 0;
	std::array<SoldierTeamSpec, 2> teams{ {
			{ "persistence/unlocks/soldiers/visual/mp/us/mp_us_assault_appearance01", "weapons/m16a4/m16a4_3p_mesh", "weapons/m16a4/m16a4",
					"weapons/m16a4/u_m16a4" },
			{ "persistence/unlocks/soldiers/visual/mp/ru/mp_ru_assault_appearance01", "weapons/ak74m/ak74_3p_mesh", "weapons/ak74m/ak74",
					"weapons/ak74m/u_ak74m" },
	} };
	anim::CrowdClipNames clips;
};

struct SoldierPart {
	std::string mesh;
	std::string variation;
	uint32_t variation_hash = 0;
	bool weapon = false;
	uint8_t kit_mask = 0x0F;
	MeshMaterials materials;
	PreparedMesh prepared;
	core::CachedItem cache;
	double seconds_mesh = 0.0;
};

struct SoldierTeam {
	std::string appearance;
	std::array<std::string, 4> kit_appearances;
	std::vector<SoldierPart> parts;
	anim::SkinBinds binds;

	anim::BoneMask weapon_joints;
	std::vector<anim::Xform> weapon_local;
	uint32_t reload_index = 0;
	anim::CrowdClipData reload;
	float reload_time = 0.0f;
	uint64_t vertices = 0, triangles = 0;
};

struct SoldierAssets {
	std::unique_ptr<core::ant::AntLibrary> library;
	anim::CrowdClipSet clips;
	anim::EbxSkeleton body_skeleton;
	anim::EbxSkeleton weapon_skeleton;
	std::array<SoldierTeam, 2> teams;
	anim::TemporalLodTable lod;
	std::vector<PreparedTexture> textures;
	std::vector<std::string> notes;
	double seconds_ant = 0.0, seconds_ebx = 0.0, seconds_meshes = 0.0, seconds_textures = 0.0, seconds_total = 0.0;

	const PreparedTexture *texture(const std::string &p_name) const;
};

core::Result<std::unique_ptr<SoldierAssets>> load_soldier_assets(const core::InstallReader &p_install, const SoldierSpec &p_spec,
		LoadProgress *r_progress, core::Cache *p_cache = nullptr);

struct PinchReport {
	float min_ratio_left = 1.0f, min_ratio_right = 1.0f;
	uint32_t vertices_left = 0, vertices_right = 0;
	uint32_t below_left = 0, below_right = 0;
	float hand_ik_m = 0.0f;
};
PinchReport measure_pinch(const SoldierTeam &p_team, const anim::Skeleton &p_rig, std::span<const anim::Xform> p_model,
		std::span<const anim::Mat34> p_palette, float p_radius = 0.18f);

}
