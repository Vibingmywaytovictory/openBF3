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
#include "bf3/anim/viewmodel_anim.h"
#include "bf3/core/ant/ant_install_source.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/result.h"
#include "bf3/render/viewmodel_assets.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
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
	Sprint = 12, // vm: the package's SprintPose frame (the animator's base-layer sprint; anim::VmClip::Sprint)
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

	// ---- vm additions ----
	// The arms when the appearance has no 1p arms pair (or is empty).
	std::string arms_mesh = "characters/arms/arms1p_bareglove03/arms1p_bareglove03_mesh";
	std::string arms_variation = "characters/arms/arms1p_bareglove03/arms1p_bareglove03_afro";
	// Ant clips and EBX data only: no meshes, materials or textures (bf3dump vm clips, the E5 / E11 reports).
	bool clips_only = false;
};

struct ViewmodelClip {
	ViewmodelClipSource source = ViewmodelClipSource::Missing;
	std::string name; // the clip's "__name" (or the stand-in's description)
	anim::LoadedClip clip; // valid for Clip and VbrStandIn
	anim::ClipTracks tracks; // on ViewmodelSet::rig
	float seconds = 0; // authored length (E5 prints it next to ReloadTime)

	// ---- vm additions ----
	std::string controller; // the clip's ClipControllerAsset ("" for stand-ins)
	std::string codec; // Dct / Frame / Raw / Vbr ... ("" for procedural stand-ins)
	// A zoom clip whose end frames do not land on IdlePose / ZoomPose (beyond kZoomPathToleranceDeg on a driven
	// bone): the animator blends IdlePose -> ZoomPose instead (counted as a stand-in).
	bool blend_instead = false;
	float endpoint_deg = 0; // the worst end-frame mismatch of a zoom clip (reports)
};

// vm: a zoom clip drives the ADS transition only when its first and last frames match the poses it joins.
inline constexpr float kZoomPathToleranceDeg = 2.0f;
// vm: procedural stand-in lengths with no data behind them [inferred].
inline constexpr float kKnifeStandInSeconds = 0.5f;
inline constexpr float kThrowStandInSeconds = 0.6f;
// vm (§16 Q6): an aimed zoom level at or below this world FOV is an optic; Bf3Viewmodel draws the procedural
// vignette stand-in over it (scope filters are M7).
inline constexpr float kOpticVignetteMaxFov = 30.0f;

enum class ViewmodelItemKind : uint8_t { Gun, Grenade, Knife };
const char *viewmodel_item_kind_name(ViewmodelItemKind p_kind);

// vm: the aiming controller's zoom data (SoldierAimingSimulationData + its ZoomLevelData) [verified data on the
// kit weapons, bf3dump vm clips prints it]. A transition time < 0 = no ZoomLevelSpecificTransitionTime for it.
struct ViewmodelAdsData {
	std::string controller; // partition
	uint32_t zoom_levels = 0;
	float hip_fov = 55, zoom_fov = 55; // ZoomLevels[0] / [1] FieldOfView
	float look_speed_hip = 1, look_speed_zoom = 1;
	float fov_transition = 0.15f, zoom_transition = 0.2f; // the generic FovTransitionTime / ZoomTransitionTime
	float fov_in = -1, fov_out = -1, zoom_in = -1, zoom_out = -1; // 0 -> 1 and 1 -> 0
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

	// ---- vm additions ----
	ViewmodelItemKind kind = ViewmodelItemKind::Gun;
	std::string mesh; // the 1p mesh partition used
	std::string ant_package; // "" = none (knife, M67: the static package's idle only)
	std::string weapon_class;
	ViewmodelAdsData ads;
	float zoom_render_fov_data = 0; // SoldierWeaponData.ZoomRenderFov as stored (zoom_render_fov keeps the optic's)
	int32_t magazine_capacity = 0;
	// The Ant packages the clip bindings point into (LoadedClip::package of the first clip of a package owns
	// nothing here; every package of the item is kept alive by this list).
	std::vector<std::unique_ptr<core::ant::AntPackage>> packages;
};

// vm: the animator's clip table of an item (stand-ins as VmClipRef::stand_in with the stand-in's seconds).
std::array<anim::VmClipRef, anim::kVmClips> viewmodel_clip_refs(const ViewmodelItem &p_item);
// vm: the ADS camera parameters of an item under the tuning (fov_transition_specific picks the per-transition
// FOV times; ZoomTransitionTime gives the viewmodel blend, the tuning's ads_*_seconds when the data has none).
anim::AdsParams viewmodel_ads_params(const ViewmodelItem &p_item, const anim::ViewmodelTuning &p_tuning);

struct ViewmodelSet {
	std::shared_ptr<core::ant::AntLibrary> library; // the clip bindings' rig points into it (vm: shared by the team sets)
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

// ---- vm additions -------------------------------------------------------------------------------------------

// What several sets share: the install, the Ant banks and library (the slow part, 23-80 s), the level superbundle.
struct ViewmodelSources {
	std::shared_ptr<core::Install> install;
	std::unique_ptr<core::ant::AntInstallSource> ant;
	std::shared_ptr<core::ant::AntLibrary> library;
	std::unique_ptr<core::EbxLibrary> ebx;
	std::string superbundle;
	double seconds_ant = 0, seconds_ebx = 0;
};
core::Result<std::unique_ptr<ViewmodelSources>> open_viewmodel_sources(const core::InstallReader &p_install, const std::string &p_superbundle,
		LoadProgress *r_progress);
// p_spec.superbundle must be p_sources.superbundle.
core::Result<std::unique_ptr<ViewmodelSet>> load_viewmodel_set(ViewmodelSources &p_sources, const ViewmodelSetSpec &p_spec, LoadProgress *r_progress,
		core::Cache *p_cache = nullptr);

// vm (E11): how an item's deploy, fire and reload are drawn: a clip, a counted stand-in, or not applicable (the
// knife has no magazine). covered = every applicable one is a clip or a stand-in.
struct ViewmodelCoverageEntry {
	const char *what = "deploy"; // deploy / fire / reload
	ViewmodelClipRole role = ViewmodelClipRole::Deploy; // the role that draws it
	ViewmodelClipSource source = ViewmodelClipSource::Missing;
	bool applicable = true;
	std::string name;
	float seconds = 0;
};
struct ViewmodelCoverage {
	std::array<ViewmodelCoverageEntry, 3> entries;
	uint32_t clips = 0, stand_ins = 0, not_applicable = 0;
	bool covered = false;
};
ViewmodelCoverage viewmodel_coverage(const ViewmodelItem &p_item);
const char *viewmodel_clip_source_name(ViewmodelClipSource p_source);

// The default loadouts of §5.6 (the kits' slot 0, 1, 6, 7 defaults on MP_Subway [verified: bf3dump kits]).
struct ViewmodelKit {
	const char *team; // "US" / "RU"
	const char *kit; // gameplay/kits/usassault ...
	std::array<const char *, 4> unlocks; // slots 0, 1, 6, 7
};
std::span<const ViewmodelKit> default_viewmodel_kits(); // 8 loadouts: US then RU, assault engineer support recon
// The set of one team (1 = US, 2 = RU): every distinct item of its four default kits, with the team's arms.
ViewmodelSetSpec team_viewmodel_set(int p_team);

} // namespace bf3::render
