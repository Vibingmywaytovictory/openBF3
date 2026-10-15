#pragma once
// Everything a first-person viewmodel needs, read from the install and prepared for upload (M0 exit: "M9 1p empty
// reload plays in Godot from C++ reading the install"). Godot-free: runs on a loader thread; the Godot side only
// creates resources from the result on the main thread.
//
// Default spec (all read from Win32/Levels/MP_Subway/MP_Subway and the Ant banks):
//   arms     the 1P arms of the US assault appearance: the BlueprintAndVariationPair of
//            persistence/unlocks/soldiers/visual/mp/us/upperbody/mp_us_assault_upperbody01 whose BaseAsset is a
//            1P arms mesh (characters/arms/arms1p_bareglove03/arms1p_bareglove03_mesh, variation
//            arms1p_bareglove03_afro); skins to animations/skeletons/venice1pske01
//   weapon   weapons/m9/m9_1p_mesh, skins to animations/skeletons/weapon/weaponske01
//   clip     Ant package m9, "ReloadEmpty Anim" (30 fps, frames 0..65), on the rig Venice1pAntSke01
// Both meshes are skinned to the rig's skeleton by bone name through one SkinBinds list (one skin).

#include "bf3/anim/clip_asset.h"
#include "bf3/anim/pose.h"
#include "bf3/anim/skeleton.h"
#include "bf3/anim/skinning.h"
#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/result.h"
#include "bf3/render/material_lookup.h"
#include "bf3/render/mesh_prep.h"
#include "bf3/render/texture_prep.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bf3::render {

struct ViewmodelSpec {
	std::string superbundle = "Win32/Levels/MP_Subway/MP_Subway";
	// Appearance unlock whose 1P arms pair is used; when empty or without such a pair, arms_mesh/arms_variation.
	std::string appearance = "persistence/unlocks/soldiers/visual/mp/us/upperbody/mp_us_assault_upperbody01";
	std::string arms_mesh = "characters/arms/arms1p_bareglove03/arms1p_bareglove03_mesh";
	std::string arms_variation = "characters/arms/arms1p_bareglove03/arms1p_bareglove03_afro";
	std::string arms_skeleton = "animations/skeletons/venice1pske01";
	std::string weapon_mesh = "weapons/m9/m9_1p_mesh";
	std::string weapon_skeleton = "animations/skeletons/weapon/weaponske01";
	// Weapon variant whose WeaponDef gives the 1P render FOV (SoldierWeaponData.RenderFov) and reload time.
	std::string weapon_unlock = "weapons/m9/u_m9";
	std::string ant_package = "m9";
	std::string clip = "ReloadEmpty Anim";
};

// Progress shared with the main thread (a label reads it while the loader runs).
class LoadProgress {
public:
	void set(std::string p_stage, int p_step);
	std::string stage() const;
	int step() const { return step_.load(); }
	static constexpr int kSteps = 8;

private:
	mutable std::mutex mutex_;
	std::string stage_ = "starting";
	std::atomic<int> step_{ 0 };
};

struct ViewmodelPart {
	std::string mesh; // MeshAsset partition
	std::string variation; // ObjectVariation partition ("" = default)
	uint32_t variation_hash = 0;
	MeshMaterials materials;
	PreparedMesh prepared;
	core::CachedItem cache; // how the prepared mesh was obtained (hit, converted, ... uncached without a cache)
	double seconds_mesh = 0.0; // res read + key + cache lookup or conversion
};

struct ViewmodelAssets {
	std::unique_ptr<core::ant::AntLibrary> library; // the clip binding's rig points into it
	anim::LoadedClip clip;
	anim::Skeleton rig;
	anim::ClipTracks tracks;
	anim::EbxSkeleton arms_skeleton;
	anim::EbxSkeleton weapon_skeleton;
	anim::SkinBinds binds;
	ViewmodelPart arms;
	ViewmodelPart weapon;
	float render_fov = 0.0f; // WeaponDef::render_fov (0 when the weapon did not resolve)
	float reload_time = 0.0f; // WeaponDef fire.reload_time (empty magazine)
	float reload_threshold = 0.0f;
	std::vector<PreparedTexture> textures; // every Diffuse / Normal texture the drawn surfaces use, unique by name
	std::vector<std::string> notes; // what was found (formats, fallbacks), for logs
	double seconds_ant = 0.0, seconds_ebx = 0.0, seconds_meshes = 0.0, seconds_textures = 0.0, seconds_total = 0.0;

	const PreparedTexture *texture(const std::string &p_name) const;
};

// Loads and prepares everything. Texture decode runs on all cores (bf3::core::parallel_for). With p_cache (open),
// the two meshes come from the derived-data cache when their keys match and are converted and put otherwise
// (bf3/render/mesh_cache.h); textures are always decoded from the install (plan §2.4).
core::Result<std::unique_ptr<ViewmodelAssets>> load_viewmodel(const core::InstallReader &p_install, const ViewmodelSpec &p_spec, LoadProgress *r_progress,
		core::Cache *p_cache = nullptr);

} // namespace bf3::render
