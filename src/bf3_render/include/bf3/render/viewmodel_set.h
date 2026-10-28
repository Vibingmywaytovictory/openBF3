#pragma once

#include "bf3/anim/clip_asset.h"
#include "bf3/anim/pose.h"
#include "bf3/anim/pose_math.h"
#include "bf3/anim/skeleton.h"
#include "bf3/anim/skinning.h"
#include "bf3/anim/viewmodel_anim.h"
#include "bf3/core/ant/ant_install_source.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/math.h"
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

enum class ViewmodelClipRole : uint8_t {
	Deploy = 0,
	Idle = 1,
	Fire = 2,
	FireSingle = 3,
	Reload = 4,
	ReloadEmpty = 5,
	Bolt = 6,
	ZoomIn = 7,
	ZoomOut = 8,
	ZoomIdle = 9,
	Throw = 10,
	Knife = 11,
	Sprint = 12,
	ThrowStart = 13,
	ThrowHold = 14,
	Count
};
inline constexpr size_t kViewmodelClipRoles = size_t(ViewmodelClipRole::Count);
const char *viewmodel_clip_role_name(ViewmodelClipRole p_role);

enum class ViewmodelClipSource : uint8_t {
	Missing = 0,
	Clip = 1,
	VbrStandIn = 2,
	ProceduralStandIn = 3,
};

struct ViewmodelItemSpec {
	std::string unlock;
	std::string mesh;
	std::string ant_package;
	std::array<std::string, kViewmodelClipRoles> clips;
};

struct ViewmodelSetSpec {
	std::string superbundle = "Win32/Levels/MP_Subway/MP_Subway";

	std::string appearance = "persistence/unlocks/soldiers/visual/mp/us/upperbody/mp_us_assault_upperbody01";
	std::string arms_skeleton = "animations/skeletons/venice1pske01";
	std::string weapon_skeleton = "animations/skeletons/weapon/weaponske01";
	std::vector<ViewmodelItemSpec> items;

	std::string arms_mesh = "characters/arms/arms1p_bareglove03/arms1p_bareglove03_mesh";
	std::string arms_variation = "characters/arms/arms1p_bareglove03/arms1p_bareglove03_afro";

	bool clips_only = false;

	bool knife_dog_tags = false;
};

struct ViewmodelClip {
	ViewmodelClipSource source = ViewmodelClipSource::Missing;
	std::string name;
	anim::LoadedClip clip;
	anim::ClipTracks tracks;
	float seconds = 0;

	std::string controller;
	std::string codec;

	bool blend_instead = false;
	float endpoint_deg = 0;
};

inline constexpr float kZoomPathToleranceDeg = 2.0f;

inline constexpr float kKnifeStandInSeconds = 0.5f;
inline constexpr float kThrowStandInSeconds = 0.6f;

inline constexpr float kOpticVignetteMaxFov = 30.0f;

enum class ViewmodelItemKind : uint8_t { Gun, Grenade, Knife };
const char *viewmodel_item_kind_name(ViewmodelItemKind p_kind);

struct ViewmodelAdsData {
	std::string controller;
	uint32_t zoom_levels = 0;
	float hip_fov = 55, zoom_fov = 55;
	float look_speed_hip = 1, look_speed_zoom = 1;
	float fov_transition = 0.15f, zoom_transition = 0.2f;
	float fov_in = -1, fov_out = -1, zoom_in = -1, zoom_out = -1;
};

struct ViewmodelSlotLook {
	bool alpha_test = false;
	bool double_sided = false;
	bool in_database = false;
};
inline constexpr float kViewmodelAlphaScissor = 0.5f;

inline constexpr std::string_view kDogTagTexture = "/dogtag";

struct ViewmodelAttachment {
	std::string socket_unlock;
	std::string bone;
	int32_t rig_bone = -1;
	anim::Mat34 transform;
	bool hip = true;
	bool zoom = true;
	ViewmodelPart part;
	std::vector<ViewmodelSlotLook> looks;
};
inline constexpr std::string_view kDefaultSocketUnlockPrefix = "weapons/common/no";

struct ViewmodelLocomotionClips;

struct ViewmodelItem {
	std::string unlock;
	ViewmodelPart weapon;
	anim::SkinBinds binds;
	std::array<ViewmodelClip, kViewmodelClipRoles> clips;
	float render_fov = 0;
	float zoom_render_fov = 0;
	float deploy_time = 0;
	float reload_time = 0, reload_time_bullets_left = 0, reload_threshold = 0;
	uint32_t stand_ins = 0;

	ViewmodelItemKind kind = ViewmodelItemKind::Gun;
	std::string mesh;
	std::string ant_package;
	std::string weapon_class;
	ViewmodelAdsData ads;
	float zoom_render_fov_data = 0;
	int32_t magazine_capacity = 0;

	std::vector<std::unique_ptr<core::ant::AntPackage>> packages;
	std::vector<ViewmodelAttachment> attachments;
	std::vector<ViewmodelSlotLook> weapon_looks;
	uint32_t hidden_surfaces = 0;
	std::shared_ptr<const ViewmodelLocomotionClips> locomotion;
	anim::VmThrowFlow throw_flow;
	std::string throw_flow_name;
};

std::array<anim::VmClipRef, anim::kVmClips> viewmodel_clip_refs(const ViewmodelItem &p_item);

anim::AdsParams viewmodel_ads_params(const ViewmodelItem &p_item, const anim::ViewmodelTuning &p_tuning);

struct ViewmodelSet {
	std::shared_ptr<core::ant::AntLibrary> library;
	anim::Skeleton rig;
	anim::EbxSkeleton arms_skeleton;
	anim::EbxSkeleton weapon_skeleton;
	ViewmodelPart arms;
	std::vector<ViewmodelSlotLook> arms_looks;
	std::vector<ViewmodelItem> items;
	std::vector<PreparedTexture> textures;
	std::vector<std::string> notes;
	double seconds_total = 0;

	const PreparedTexture *texture(const std::string &p_name) const;
};

core::Result<std::unique_ptr<ViewmodelSet>> load_viewmodel_set(const core::InstallReader &p_install, const ViewmodelSetSpec &p_spec,
		LoadProgress *r_progress, core::Cache *p_cache = nullptr);

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

core::Result<std::unique_ptr<ViewmodelSet>> load_viewmodel_set(ViewmodelSources &p_sources, const ViewmodelSetSpec &p_spec, LoadProgress *r_progress,
		core::Cache *p_cache = nullptr);

struct ViewmodelCoverageEntry {
	const char *what = "deploy";
	ViewmodelClipRole role = ViewmodelClipRole::Deploy;
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

struct ViewmodelSurfaceSkin {
	size_t subset = 0;
	std::string material;
	size_t vertices = 0, triangles = 0;
	size_t bad_weights = 0;
	float bind_edge = 0, posed_edge = 0;
	std::string worst_bone;
	std::vector<int32_t> binds;
	core::Vec3 posed_min{}, posed_max{};
	bool stretched() const;
};

inline constexpr float kSkinStretchFactor = 1.5f;
inline constexpr float kSkinStretchFloor = 0.05f;
std::vector<ViewmodelSurfaceSkin> viewmodel_surface_skin(const PreparedMesh &p_mesh, std::span<const anim::SkinBind> p_binds,
		std::span<const anim::Xform> p_model);

struct ViewmodelKit {
	std::string team;
	std::string kit;
	std::array<std::string, 4> unlocks;
	std::string gadget;
};
inline constexpr std::string_view kViewmodelKitsMode = "TeamDeathMatch0";
inline constexpr size_t kViewmodelKitsPerTeam = 4;

core::Result<std::vector<ViewmodelKit>> read_viewmodel_kits(const core::EbxResolver &p_db, std::string_view p_level_id,
		std::string_view p_mode = kViewmodelKitsMode);

std::string viewmodel_level_id(std::string_view p_superbundle);

std::span<const ViewmodelKit> documented_viewmodel_kits();

std::string viewmodel_kits_difference(std::span<const ViewmodelKit> p_data, std::span<const ViewmodelKit> p_documented);

ViewmodelSetSpec team_viewmodel_set(std::span<const ViewmodelKit> p_kits, int p_team);

ViewmodelSetSpec team_viewmodel_items(int p_team, std::span<const std::string> p_unlocks);
}

namespace bf3::render {

size_t add_assault_gadget(std::span<ViewmodelKit> r_kits, std::string_view p_unlock);

}

#include "bf3/anim/move_group.h"

namespace bf3::render {

inline constexpr std::string_view kVmRunDeltaChooser = "1P_RunLoopChooser";
inline constexpr std::string_view kVmSprintDeltaChooser = "Wep_SprintDeltaChooser";

struct ViewmodelLocomotionClips {
	uint32_t weapon_type = 0;
	ViewmodelClip run, sprint;
	float run_start_frame = 0, run_end_frame = 0, sprint_start_frame = 0, sprint_end_frame = 0;
	std::string run_path, sprint_path;
};

void load_viewmodel_locomotion(const core::ant::AntLibrary &p_library, const anim::Skeleton &p_rig, uint32_t p_ant_ref, ViewmodelItem &r_item,
		std::vector<std::string> &r_notes);

anim::VmLocomotion viewmodel_locomotion(const ViewmodelItem &p_item);

void load_viewmodel_throw_flow(const core::ant::AntLibrary &p_library, const anim::Skeleton &p_rig, uint32_t p_ant_ref,
		ViewmodelItem &r_item, std::vector<std::string> &r_notes);

}
