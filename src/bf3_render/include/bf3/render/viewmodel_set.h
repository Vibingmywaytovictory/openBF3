#pragma once
// A kit's first-person viewmodels (docs/design/m2a.md §11.1; owner vm): M9's load_viewmodel (viewmodel_assets.h)
// generalised to the local kit. The team's 1p arms, each item's 1p mesh on weaponske01, materials and textures, and
// the item's Ant clips per role. Godot-free; runs on the loader thread (kit changes load before the next spawn);
// the Godot side (Bf3Viewmodel) only creates resources from the result on the main thread.
//
// Clip names are resolved per item package (bf3dump vm clips <item> lists them). VBR clips (146 MP clips, mostly
// pistol and launcher zoom [verified: research §3.3]) use the Frame IdlePose -> ZoomPose stand-in over the zoom
// time; a missing clip gets a procedural stand-in (deploy: raise from below over DeployTime; fire: kick). Every
// stand-in is counted and named (E11's report lists them).

#include "bf3/anim/clip_asset.h"
#include "bf3/anim/pose.h"
#include "bf3/anim/skeleton.h"
#include "bf3/anim/skinning.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/result.h"
#include "bf3/render/viewmodel_assets.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bf3::render {

// The clip roles of one item (viewmodel_anim.h's layers use the same order).
enum class ViewmodelClipRole : uint8_t {
	Deploy = 0,
	Idle = 1,
	Fire = 2,
	FireSingle = 3,
	Reload = 4,
	ReloadEmpty = 5,
	Bolt = 6, // bolt or pump
	ZoomIn = 7,
	ZoomOut = 8,
	ZoomIdle = 9,
	Throw = 10, // M67
	Knife = 11,
	Count
};
inline constexpr size_t kViewmodelClipRoles = size_t(ViewmodelClipRole::Count);
const char *viewmodel_clip_role_name(ViewmodelClipRole p_role);

enum class ViewmodelClipSource : uint8_t {
	Missing = 0, // the role does not apply to the item (no Throw on a rifle)
	Clip = 1, // decoded from the item's package
	VbrStandIn = 2, // Frame IdlePose -> ZoomPose over the zoom time
	ProceduralStandIn = 3, // raise from below / kick
};

struct ViewmodelItemSpec {
	std::string unlock; // weapons/m16a4/u_m16a4 (the WeaponDef gives RenderFov, DeployTime, ReloadTime)
	std::string mesh; // weapons/m16a4/m16a4_1p_mesh ("" = from the unlock's blueprint)
	std::string ant_package; // the item's Ant package ("" = resolved from the unlock)
	std::array<std::string, kViewmodelClipRoles> clips; // "__name" per role ("" = resolved by the role's rule)
};

struct ViewmodelSetSpec {
	std::string superbundle = "Win32/Levels/MP_Subway/MP_Subway";
	// The team's appearance unlock whose 1p arms pair is used (US: M9's default; RU from the RU appearance [inferred]).
	std::string appearance = "persistence/unlocks/soldiers/visual/mp/us/upperbody/mp_us_assault_upperbody01";
	std::string arms_skeleton = "animations/skeletons/venice1pske01";
	std::string weapon_skeleton = "animations/skeletons/weapon/weaponske01";
	std::vector<ViewmodelItemSpec> items; // the kit's slots 0, 1, 6, 7 (kits.h), in slot order
};

struct ViewmodelClip {
	ViewmodelClipSource source = ViewmodelClipSource::Missing;
	std::string name; // the clip's "__name" (or the stand-in's description)
	anim::LoadedClip clip; // valid for Clip and VbrStandIn
	anim::ClipTracks tracks; // on ViewmodelSet::rig
	float seconds = 0; // authored length (E5 prints it next to ReloadTime)
};

struct ViewmodelItem {
	std::string unlock;
	ViewmodelPart weapon; // the 1p mesh, materials, prepared mesh
	anim::SkinBinds binds; // arms + weapon on the rig
	std::array<ViewmodelClip, kViewmodelClipRoles> clips;
	float render_fov = 0; // SoldierWeaponData.RenderFov (55 on the M16A3)
	float zoom_render_fov = 0; // an optic's zoomRenderFov (0 = none)
	float deploy_time = 0;
	float reload_time = 0, reload_time_bullets_left = 0, reload_threshold = 0;
	uint32_t stand_ins = 0; // clips with a VbrStandIn or ProceduralStandIn source
};

struct ViewmodelSet {
	std::unique_ptr<core::ant::AntLibrary> library; // the clip bindings' rig points into it
	anim::Skeleton rig; // Venice1pAntSke01
	anim::EbxSkeleton arms_skeleton;
	anim::EbxSkeleton weapon_skeleton;
	ViewmodelPart arms;
	std::vector<ViewmodelItem> items; // ViewmodelSetSpec::items order
	std::vector<PreparedTexture> textures; // unique by name over every part
	std::vector<std::string> notes; // stand-ins, fallbacks, formats (logs and E11's report)
	double seconds_total = 0;

	const PreparedTexture *texture(const std::string &p_name) const;
};

// Loads and prepares the whole set (textures decode on all cores; meshes through p_cache when given, as
// load_viewmodel). A missing clip is a stand-in, not an error; a missing mesh or rig is an error naming the item.
core::Result<std::unique_ptr<ViewmodelSet>> load_viewmodel_set(const core::InstallReader &p_install, const ViewmodelSetSpec &p_spec,
		LoadProgress *r_progress, core::Cache *p_cache = nullptr);

} // namespace bf3::render
