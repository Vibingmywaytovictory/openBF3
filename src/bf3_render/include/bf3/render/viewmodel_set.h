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
#include "bf3/anim/pose_math.h"
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
#include <string_view>
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

// vm: how a material slot is drawn, from the level shader database through L1's LookResolver (the shader's surface
// type; the name rule when the database lacks it). The knife's dog-tag chain is objects/shaders/proppreset_alphatest
// (surface type OpaqueAlphaTest [verified: bf3dump looks MP_Subway TeamDeathMatch0 --shader]): a strip cut out by its
// Diffuse alpha, which drawn opaque shows as long thin bars across the view.
struct ViewmodelSlotLook {
	bool alpha_test = false; // alpha scissor at kViewmodelAlphaScissor on the Diffuse alpha
	bool double_sided = false;
	bool in_database = false; // the slot's shader graph is in the level shader database
};
inline constexpr float kViewmodelAlphaScissor = 0.5f; // as L1's level materials (level_upload.cpp)

// A rigid accessory mesh on a weapon bone (SoldierWeaponData.Sockets: a SocketData's AvailableObjects, the
// WeaponRegularSocketObjectData's Asset1p / Asset1pzoom). The kits' default customization attaches the sockets whose
// UnlockAsset is one of the "weapons/common/no..." unlocks (nooptics = the iron sights of flat-top rifles, the M16A4's
// m16_ironsight and m16_sight); sockets of other unlocks (optics, rails, barrels) stay off.
struct ViewmodelAttachment {
	std::string socket_unlock; // the SocketData's UnlockAsset
	std::string bone; // SocketData.BoneName (on the rig)
	int32_t rig_bone = -1;
	anim::Mat34 transform; // in the bone's space: BoneRigidTransform * SocketData.Transform * object Transform
	bool hip = true; // drawn while not aimed (Asset1p)
	bool zoom = true; // drawn while aimed (Asset1pzoom)
	ViewmodelPart part; // unskinned
	std::vector<ViewmodelSlotLook> looks; // per material slot of part
};
inline constexpr std::string_view kDefaultSocketUnlockPrefix = "weapons/common/no";

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
	std::vector<ViewmodelAttachment> attachments; // the default customization's rigid accessories
	std::vector<ViewmodelSlotLook> weapon_looks; // per material slot of weapon
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
	std::vector<ViewmodelSlotLook> arms_looks; // vm: per material slot of arms
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

// vm: skinned-geometry sanity (the stretched-lines check). Every drawn surface of a skinned part, skinned with the
// binds and a model-space pose of the rig as the renderer does (sum of weight * model[bind.bone] * inverse_bind):
// the longest triangle edge in the bind pose and after skinning, and the vertices whose weights do not sum to 1.
// A rigid or animated weapon keeps its edge lengths; a vertex skinned to a bone the pose leaves behind (or one with
// no weight) stretches its triangles across the screen.
struct ViewmodelSurfaceSkin {
	size_t subset = 0;
	std::string material;
	size_t vertices = 0, triangles = 0;
	size_t bad_weights = 0; // |sum of weights - 1| > 0.01, or a weight on an unknown bind
	float bind_edge = 0, posed_edge = 0; // the longest triangle edge, metres
	std::string worst_bone; // the heaviest bone of the vertex at the longest posed edge
	std::vector<int32_t> binds; // the binds the surface's weights use (sorted)
	std::array<float, 3> posed_min{}, posed_max{}; // the skinned extent in the rig's model space
	bool stretched() const;
};
// A posed edge longer than this factor of the bind pose's longest edge (and longer than the floor) is stretched.
inline constexpr float kSkinStretchFactor = 1.5f;
inline constexpr float kSkinStretchFloor = 0.05f; // metres
std::vector<ViewmodelSurfaceSkin> viewmodel_surface_skin(const PreparedMesh &p_mesh, std::span<const anim::SkinBind> p_binds,
		std::span<const anim::Xform> p_model);

// The default loadouts of §5.6, read from the data: the mode's teams 1 (US) and 2 (RU), TeamDef::kits in order, and
// KitDef::default_unlock of slots 0, 1, 6, 7 (the same resolution as sv's make_kit_table, bf3_host kits.h). E11's
// coverage, bf3dump vm clips and the vm demo iterate over these, so the item set is the data's, not a copy of it.
struct ViewmodelKit {
	std::string team; // "US" (team 1) / "RU" (team 2)
	std::string kit; // gameplay/kits/usassault ...
	std::array<std::string, 4> unlocks; // slots 0, 1, 6, 7
};
inline constexpr std::string_view kViewmodelKitsMode = "TeamDeathMatch0"; // §5.6: the Metro TDM loadouts
inline constexpr size_t kViewmodelKitsPerTeam = 4; // assault, engineer, support, recon
// Reads the 8 loadouts of p_level_id's p_mode (US then RU, in the teams' kit order). Fails when a team is missing,
// does not list 4 kits, or a kit has no default for one of the 4 slots. p_level_id: "MP_Subway".
core::Result<std::vector<ViewmodelKit>> read_viewmodel_kits(const core::EbxResolver &p_db, std::string_view p_level_id,
		std::string_view p_mode = kViewmodelKitsMode);
// The level id of a superbundle path ("Win32/Levels/MP_Subway/MP_Subway" -> "MP_Subway").
std::string viewmodel_level_id(std::string_view p_superbundle);
// The loadouts §5.6 names for Metro TDM [verified: bf3dump kits MP_Subway TeamDeathMatch0]. A cross-check only
// (row e11.kits_match_data, bf3dump vm clips): nothing iterates over it in place of read_viewmodel_kits.
std::span<const ViewmodelKit> documented_viewmodel_kits();
// "" when equal, else the first difference (for the cross-check lines).
std::string viewmodel_kits_difference(std::span<const ViewmodelKit> p_data, std::span<const ViewmodelKit> p_documented);
// The set of one team (1 = US, 2 = RU): every distinct item of its kits in p_kits, with the team's arms.
ViewmodelSetSpec team_viewmodel_set(std::span<const ViewmodelKit> p_kits, int p_team);

} // namespace bf3::render
