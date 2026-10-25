#pragma once

#include "bf3/anim/layers.h"
#include "bf3/anim/locomotion.h"
#include "bf3/anim/pose.h"
#include "bf3/anim/skeleton.h"
#include "bf3/core/ant/ant_clip.h"
#include "bf3/core/ant/ant_library.h"
#include "bf3/core/ant/ant_package.h"
#include "bf3/core/ant/ant_source.h"
#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace bf3::anim {

enum class CrowdClip : uint8_t {
	Idle,
	RunFwd,
	RunBwd,
	RunLeft,
	RunRight,
	AimUp,
	AimDown,
	RunNoise,
	Count
};
constexpr size_t kCrowdClips = size_t(CrowdClip::Count);

constexpr uint32_t kMaxCrowdRigJoints = 1024;

struct CrowdClipNames {
	std::string package = "b_basicassetsmp";
	std::array<std::string, kCrowdClips> controllers{ "Rifle_StandIdle_Pose", "Rifle_RunFwd", "Rifle_RunBwd", "Rifle_RunLeft1", "Rifle_RunRight1",
		"Rifle_StandAimUp", "Rifle_StandAimDwn", "Rifle_Noise_RunFwd1" };
	std::string reload_chooser = "3P_Wep_Reload_NoAddon_Stand_Chooser";
	std::string root_joint = "AITrajectory";
};

struct CrowdClipData {
	core::ant::ClipBinding binding;
	core::ant::DecodedClip clip;
	ClipTracks tracks;
	BoneMask mask;
	bool additive = false;
	uint32_t controller_index = 0;
};

struct CrowdClipSet {
	std::unique_ptr<core::ant::AntPackage> package;
	Skeleton rig;
	int32_t root_joint = -1;
	std::array<CrowdClipData, kCrowdClips> clips;
	std::array<LocoClipInfo, kLocoClips> loco{};
	BoneMask upper_body;

	const CrowdClipData &get(CrowdClip p_clip) const { return clips[size_t(p_clip)]; }
};

core::Result<CrowdClipSet> load_crowd_clips(const core::ant::AntPackageSource &p_source, const core::ant::AntLibrary &p_library,
		const CrowdClipNames &p_names = {});

core::Result<uint32_t> weapon_choice_index(const core::ant::AntLibrary &p_library, uint32_t p_ant_ref_asset_id);

core::Result<CrowdClipData> load_chosen_clip(const CrowdClipSet &p_set, const core::ant::AntLibrary &p_library, std::string_view p_chooser,
		uint32_t p_index);

struct AimMeasure {
	int barrel_axis = 2;
	bool barrel_negative = false;
	float up_deg = 0.0f;
	float down_deg = 0.0f;
	float up_azimuth_deg = 0.0f, down_azimuth_deg = 0.0f;
	bool monotonic = false;

	bool qualifies() const { return up_deg >= 25.0f && down_deg <= -25.0f && monotonic && up_azimuth_deg < 5.0f && down_azimuth_deg < 5.0f; }
};
core::Result<AimMeasure> measure_aim(const CrowdClipSet &p_set, AdditiveOrder p_order);

}

#include <vector>

namespace bf3::anim {

enum class CrowdMoveClip : uint8_t {
	SneakFwd,
	SneakBwdLeft,
	SneakBwdRight,
	SneakLeft,
	SneakRight,
	CrawlFwd,
	CrawlBwd,
	CrawlLeft,
	CrawlRight,
	CrouchFire,
	DeathStand,
	DeathCrouch,
	DeathProne,
	Count
};
inline constexpr size_t kCrowdMoveClips = size_t(CrowdMoveClip::Count);

struct CrowdMoveClipNames {
	std::array<std::string, kCrowdMoveClips> controllers{ "Rifle_SneakFwd", "Rifle_SneakBwdLeft45", "Rifle_SneakBwdRight45", "Rifle_SneakLeft1",
		"Rifle_SneakRight1", "Rifle_CrawlFwd", "Rifle_CrawlBwd", "Rifle_CrawlLeft", "Rifle_CrawlRight", "Rifle_CrouchFire", "Death_StandChest",
		"Death_Crouch1", "Death_Prone1" };
};

inline constexpr size_t kCrowdMoveStances = 2;

struct CrowdMoveClips {
	std::array<CrowdClipData, kCrowdMoveClips> clips;
	std::array<bool, kCrowdMoveClips> present{};

	std::array<std::array<LocoClipInfo, kLocoClips>, kCrowdMoveStances> loco{};
	std::array<bool, kCrowdMoveStances> moves_ok{};
	std::vector<std::string> notes;

	const CrowdClipData &get(CrowdMoveClip p_clip) const { return clips[size_t(p_clip)]; }
	bool has(CrowdMoveClip p_clip) const { return present[size_t(p_clip)]; }
};

core::Result<CrowdMoveClips> load_crowd_move_clips(const CrowdClipSet &p_set, const core::ant::AntLibrary &p_library, const CrowdMoveClipNames &p_names = {});

}

#include "bf3/anim/move_group.h"

namespace bf3::anim {

enum class CrowdMoveGroupRole : uint8_t { Run, Sprint, Sneak, Count };
inline constexpr size_t kCrowdMoveGroupRoles = size_t(CrowdMoveGroupRole::Count);

struct CrowdMoveGroupNames {
	std::array<std::string, kCrowdMoveGroupRoles> groups{ "MP_RunMoveGroup_PC", "MP_SprintMoveGroup", "MP_SneakMoveGroup_PC" };
	ChooserIndices indices;
};

struct CrowdMoveGroup {
	bool ok = false;
	MoveGroupDef def;
	std::vector<CrowdClipData> clips;
	std::vector<MoveGroupClipTiming> timing;
	std::vector<Vec3> root_dir;
	float x_sign = -1.0f;
	float worst_direction_deg = 0.0f;
};

struct CrowdMoveGroupSet {
	std::array<CrowdMoveGroup, kCrowdMoveGroupRoles> groups;
	MoveGroupOptions options;
	std::vector<std::string> notes;

	const CrowdMoveGroup &get(CrowdMoveGroupRole p_role) const { return groups[size_t(p_role)]; }
};

core::Result<CrowdMoveGroupSet> load_crowd_move_groups(const CrowdClipSet &p_set, const core::ant::AntLibrary &p_library, const CrowdMoveGroupNames &p_names = {});

core::Result<CrowdClipData> load_route_clip(const CrowdClipSet &p_set, const core::ant::AntLibrary &p_library,
		const AntClipRoute &p_route, const std::string &p_where);

inline constexpr std::string_view kCrowdThrowMaskArray = "FullBody_ThrowBlendMaskArray";
inline constexpr float kCrowdMaskMoving = 0.0f;
inline constexpr float kCrowdMaskStill = 1.0f;

core::Result<BoneMask> read_blend_mask(
		const core::ant::AntLibrary &p_library, const Skeleton &p_rig, std::string_view p_array, float p_value);

struct CrowdThrowClips {
	CrowdClipData in, release;
	float in_seconds = 0.0f, release_seconds = 0.0f, release_length_seconds = 0.0f;
	BoneMask moving, still;
	std::string in_name, release_name, release_leave;
};

core::Result<CrowdThrowClips> load_crowd_throw(
		const CrowdClipSet &p_set, const core::ant::AntLibrary &p_library, const ChooserIndices &p_indices);

}
