#pragma once

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

	std::string appearance = "persistence/unlocks/soldiers/visual/mp/us/upperbody/mp_us_assault_upperbody01";
	std::string arms_mesh = "characters/arms/arms1p_bareglove03/arms1p_bareglove03_mesh";
	std::string arms_variation = "characters/arms/arms1p_bareglove03/arms1p_bareglove03_afro";
	std::string arms_skeleton = "animations/skeletons/venice1pske01";
	std::string weapon_mesh = "weapons/m9/m9_1p_mesh";
	std::string weapon_skeleton = "animations/skeletons/weapon/weaponske01";

	std::string weapon_unlock = "weapons/m9/u_m9";
	std::string ant_package = "m9";
	std::string clip = "ReloadEmpty Anim";
};

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
	std::string mesh;
	std::string variation;
	uint32_t variation_hash = 0;
	MeshMaterials materials;
	PreparedMesh prepared;
	core::CachedItem cache;
	double seconds_mesh = 0.0;
};

struct ViewmodelAssets {
	std::unique_ptr<core::ant::AntLibrary> library;
	anim::LoadedClip clip;
	anim::Skeleton rig;
	anim::ClipTracks tracks;
	anim::EbxSkeleton arms_skeleton;
	anim::EbxSkeleton weapon_skeleton;
	anim::SkinBinds binds;
	ViewmodelPart arms;
	ViewmodelPart weapon;
	float render_fov = 0.0f;
	float reload_time = 0.0f;
	float reload_threshold = 0.0f;
	std::vector<PreparedTexture> textures;
	std::vector<std::string> notes;
	double seconds_ant = 0.0, seconds_ebx = 0.0, seconds_meshes = 0.0, seconds_textures = 0.0, seconds_total = 0.0;

	const PreparedTexture *texture(const std::string &p_name) const;
};

core::Result<std::unique_ptr<ViewmodelAssets>> load_viewmodel(const core::InstallReader &p_install, const ViewmodelSpec &p_spec, LoadProgress *r_progress,
		core::Cache *p_cache = nullptr);

}
