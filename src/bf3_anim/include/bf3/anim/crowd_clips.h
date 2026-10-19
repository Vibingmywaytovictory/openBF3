#pragma once
// The clip set of the 3p crowd, read from the Ant banks (S3; docs/design/s3.md §4.1, §5.2).
//
// All clips come from the Ant package b_basicassetsmp [verified 2026-10-15: Bundle, 15,929,912 bytes, 2,444
// objects, 2,405 clips bound to controllers, rig VeniceAntSke01 (skeleton RIGWep, 215 joints)]. A clip's
// "__name" is NOT unique (2,405 controllers share 848 names), so a clip is chosen in one of two ways:
//  * by a controller name that occurs exactly once in the package. This holds for the Rifle_* locomotion and
//    aim clips below; a name that occurs 0 or 2+ times is an error.
//  * through an IndexChooserControllerAsset and an index. The reload has 76 bound copies of
//    Reload_NoAddon_Stand, all children of 3P_Wep_Reload_NoAddon_Stand_Chooser (s_basicassets 0x88d04,
//    ChoiceAssetList[95], ChooseOnce 1). Its IndexAsset is the GameStateEnumerationAsset
//    SpecificWeapon_EnumGamestate (Default 31), which reads the 95-value EnumerationAsset
//    SpecificWeapon_Enumeration.
// The weapon's index comes from EBX. SoldierWeaponData.WeaponStates[0].Weapon is an AntRef whose AssetId is
// 0x15480000 | (s_basicassets guid index & 0xFFFF) [verified mapping, research 03 C8]. That object is a
// WeaponAsset, and its WeaponEnum pair names an EnumerationValueAsset whose Value is the chooser index. M16A4:
// AntRef 357063601 -> WeaponAsset 'M16A2' (0x85BB1) -> Value 41 -> controller 0x830e1, 95 frames (3.17 s).
// AK-74M (weapons/ak74m/ak74): AntRef 357087606 = 0x1548B976 -> WeaponAsset 'AK74' (0x8B976) -> Value 33 ->
// controller 0x8cb0b, 108 frames (3.6 s). [verified 2026-10-15]

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

// Roles a crowd soldier plays. Reload clips are per weapon (CrowdClipSet::reload[team]).
enum class CrowdClip : uint8_t {
	Idle, // Rifle_StandIdle_Pose
	RunFwd, // Rifle_RunFwd
	RunBwd, // Rifle_RunBwd
	RunLeft, // Rifle_RunLeft1
	RunRight, // Rifle_RunRight1
	AimUp, // Rifle_StandAimUp (additive)
	AimDown, // Rifle_StandAimDwn (additive)
	RunNoise, // Rifle_Noise_RunFwd1 (additive, DCT, end frame 40, up to 9.5 deg); optional
	Count
};
constexpr size_t kCrowdClips = size_t(CrowdClip::Count);
// The clips' rig may not exceed this many joints (checked before any per-joint allocation; VeniceAntSke01 has 215).
constexpr uint32_t kMaxCrowdRigJoints = 1024;

struct CrowdClipNames {
	std::string package = "b_basicassetsmp";
	std::array<std::string, kCrowdClips> controllers{ "Rifle_StandIdle_Pose", "Rifle_RunFwd", "Rifle_RunBwd", "Rifle_RunLeft1", "Rifle_RunRight1",
		"Rifle_StandAimUp", "Rifle_StandAimDwn", "Rifle_Noise_RunFwd1" };
	std::string reload_chooser = "3P_Wep_Reload_NoAddon_Stand_Chooser";
	std::string root_joint = "AITrajectory"; // its translation carries the root motion (removed at evaluation)
};

// One decoded, bound clip on the rig.
struct CrowdClipData {
	core::ant::ClipBinding binding; // points into CrowdClipSet::package and the library
	core::ant::DecodedClip clip;
	ClipTracks tracks; // on CrowdClipSet::rig
	BoneMask mask; // mask_from_tracks
	bool additive = false; // DecodedClip::additive (sample onto identity_pose)
	uint32_t controller_index = 0; // guid index of the ClipControllerAsset (reports; never a lookup key)
};

struct CrowdClipSet {
	std::unique_ptr<core::ant::AntPackage> package; // b_basicassetsmp; owns the bytes the bindings point into
	Skeleton rig; // skeleton_from_rig of the clips' rig (VeniceAntSke01, 215 joints)
	int32_t root_joint = -1; // CrowdClipNames::root_joint in rig
	std::array<CrowdClipData, kCrowdClips> clips; // RunNoise may be empty (binding.clip invalid) when absent
	std::array<LocoClipInfo, kLocoClips> loco{}; // Idle, Fwd, Bwd, Left, Right: durations and measured root motion
	BoneMask upper_body; // union of the reload clips' masks (reports; each reload layers with its own mask)

	const CrowdClipData &get(CrowdClip p_clip) const { return clips[size_t(p_clip)]; }
};

// Loads the package, binds and decodes every named clip and measures the locomotion root motion (the rig model-space
// position of root_joint at the last frame minus the first). Fails when a controller name does not occur
// exactly once, a clip does not bind to the same rig as the others, decoding fails, or root_joint is missing.
core::Result<CrowdClipSet> load_crowd_clips(const core::ant::AntPackageSource &p_source, const core::ant::AntLibrary &p_library,
		const CrowdClipNames &p_names = {});

// Index of a WeaponAsset: AntRef AssetId -> the unique s_basicassets object with that low 16-bit guid index
// and type WeaponAsset -> WeaponEnum pair -> EnumerationValueAsset.Value. Fails when no object or more than one
// object matches, a link does not resolve, or the type is not the expected one. At most 16 reference hops;
// every hop is charged to a WorkBudget.
core::Result<uint32_t> weapon_choice_index(const core::ant::AntLibrary &p_library, uint32_t p_ant_ref_asset_id);

// Entry p_index of the IndexChooserControllerAsset named p_chooser (unique by name in s_basicassets or
// p_package), followed to a clip of p_package (the entry is a ClipControllerAsset or a chooser fallback, at
// most 8 levels deep). The clip is then decoded, bound and tracked on p_set.rig. Fails when p_index is outside
// ChoiceAssetList or nothing resolves.
core::Result<CrowdClipData> load_chosen_clip(const CrowdClipSet &p_set, const core::ant::AntLibrary &p_library, std::string_view p_chooser,
		uint32_t p_index);

// The aim additives' effect on the weapon (s3.md §5.5, the additive-order criterion): over the idle pose, the barrel
// (the Wep_Root local axis most aligned with model +Z, i.e. forward) is measured at weights 0.25, 0.5, 0.75 and 1 of
// AimUp and of AimDown, applied in order p_order with their own masks. Measured on the retail clips [verified
// 2026-10-15]: LocalPost raises the barrel 78.1 deg (azimuth 4.7 deg; the barrel is nearly vertical) and lowers it
// 60.7 deg (azimuth 0.06 deg), monotonic; LocalPre gives +26.3 deg with a 36.8 deg azimuth swing and AimDown +0.6 deg,
// not monotonic. So AimUp / AimDown are the full up / down aim, and LocalPost is the order.
struct AimMeasure {
	int barrel_axis = 2; // Wep_Root local axis 0..2
	bool barrel_negative = false; // the barrel is the negative axis
	float up_deg = 0.0f; // barrel elevation change of AimUp at weight 1
	float down_deg = 0.0f; // of AimDown (negative when it lowers the barrel)
	float up_azimuth_deg = 0.0f, down_azimuth_deg = 0.0f; // |azimuth change| at weight 1
	bool monotonic = false; // |elevation change| grows with the weight for both clips
	// AimUp raises and AimDown lowers the barrel by >= 25 deg each, monotonic, with |azimuth change| < 5 deg.
	bool qualifies() const { return up_deg >= 25.0f && down_deg <= -25.0f && monotonic && up_azimuth_deg < 5.0f && down_azimuth_deg < 5.0f; }
};
core::Result<AimMeasure> measure_aim(const CrowdClipSet &p_set, AdditiveOrder p_order);

} // namespace bf3::anim

// ---- M2a tp (docs/design/m2a.md §9; docs/design/m2a-tp.md) -----------------------------------------------------------

#include <vector>

namespace bf3::anim {

// The clips a 3p soldier needs beyond S3's stand set and lc's stance idles, aims and transitions (those come through
// server_pose.h's ServerPoseClipNames / load_server_pose_clips, so the drawn soldier and the server's capsules share
// one source, m2a.md §6.5): the crouch and prone moves, the crouch fire and the three stance deaths. Every controller
// name below occurs exactly once in b_basicassetsmp [verified 2026-10-18, antdump list --install b_basicassetsmp:
// Rifle_SneakBwd occurs twice (end frames 32 and 42), so the crouched backward move blends Rifle_SneakBwdLeft45 and
// Rifle_SneakBwdRight45 at one phase]. Rifle_CrouchFire is a 21-frame clip on FullBodyWeaponRootNoFingers (the aim
// clips' target); there is no unique stand or prone rifle fire controller, so those stances kick procedurally
// (crowd_eval.h, kCrowdFireKick*).
enum class CrowdMoveClip : uint8_t {
	SneakFwd, // Rifle_SneakFwd (crouch forward, end frame 42)
	SneakBwdLeft, // Rifle_SneakBwdLeft45 (32)
	SneakBwdRight, // Rifle_SneakBwdRight45 (32)
	SneakLeft, // Rifle_SneakLeft1 (45)
	SneakRight, // Rifle_SneakRight1 (45)
	CrawlFwd, // Rifle_CrawlFwd (35)
	CrawlBwd, // Rifle_CrawlBwd (65)
	CrawlLeft, // Rifle_CrawlLeft (51)
	CrawlRight, // Rifle_CrawlRight (45)
	CrouchFire, // Rifle_CrouchFire (21, FullBodyWeaponRootNoFingers)
	DeathStand, // Death_StandChest (35)
	DeathCrouch, // Death_Crouch1 (36)
	DeathProne, // Death_Prone1 (15, FullBodyAllWeaponParts)
	Count
};
inline constexpr size_t kCrowdMoveClips = size_t(CrowdMoveClip::Count);

struct CrowdMoveClipNames {
	std::array<std::string, kCrowdMoveClips> controllers{ "Rifle_SneakFwd", "Rifle_SneakBwdLeft45", "Rifle_SneakBwdRight45", "Rifle_SneakLeft1",
		"Rifle_SneakRight1", "Rifle_CrawlFwd", "Rifle_CrawlBwd", "Rifle_CrawlLeft", "Rifle_CrawlRight", "Rifle_CrouchFire", "Death_StandChest",
		"Death_Crouch1", "Death_Prone1" };
};

// The stances whose moves this set carries (index 0 crouch, 1 prone; stand is S3's CrowdClipSet::loco).
inline constexpr size_t kCrowdMoveStances = 2;

struct CrowdMoveClips {
	std::array<CrowdClipData, kCrowdMoveClips> clips; // bound into CrowdClipSet::package, tracked on CrowdClipSet::rig
	std::array<bool, kCrowdMoveClips> present{}; // false: absent, not unique, unbound or without tracks (a note says why)
	// Per stance (crouch, prone): the LocoClip table the run blend uses (Idle stays zero: the stance idle is lc's pose).
	// Crouch Bwd is the 50/50 blend of SneakBwdLeft / SneakBwdRight at one phase: its duration is their mean and its
	// root motion the mean of their displacements over that duration. moves_ok[s] is true when every direction of the
	// stance is present and has root motion; otherwise the evaluator moves that stance with the stand cycles (counted).
	std::array<std::array<LocoClipInfo, kLocoClips>, kCrowdMoveStances> loco{};
	std::array<bool, kCrowdMoveStances> moves_ok{};
	std::vector<std::string> notes;

	const CrowdClipData &get(CrowdMoveClip p_clip) const { return clips[size_t(p_clip)]; }
	bool has(CrowdMoveClip p_clip) const { return present[size_t(p_clip)]; }
};

// Binds and decodes the named clips from p_set's package (the one load_crowd_clips read: no second package load) on
// p_set's rig and measures the moves' root motion as load_crowd_clips does. Fails only when p_set has no package or no
// rig; a clip that is missing, not unique, on another rig or unusable is a note (present false).
core::Result<CrowdMoveClips> load_crowd_move_clips(const CrowdClipSet &p_set, const core::ant::AntLibrary &p_library, const CrowdMoveClipNames &p_names = {});

} // namespace bf3::anim

// ---- PT1 run (docs/design/pt1-run.md) --------------------------------------------------------------------------------
//
// BF3's 3p move groups (move_group.h) with their clips on the crowd rig: the stand run, the sprint and the crouch
// sneak. The clips are the ClipControllerAssets the groups' choosers pick for ChooserIndices (Rifle), each loaded from
// the clip set's package by its controller's Anim index (so a controller name that occurs twice in the package, as
// Rifle_SneakBwd, is no ambiguity). Each clip's length and root motion are measured as load_crowd_clips measures the
// S3 cycles; the outer X sign (move_group_x) is the one that puts the measured root directions of the moving clips on
// their directions' X (MP_Run: -1, the left run at X -pi/2 moves towards +X of the rig).

#include "bf3/anim/move_group.h"

namespace bf3::anim {

enum class CrowdMoveGroupRole : uint8_t { Run, Sprint, Sneak, Count };
inline constexpr size_t kCrowdMoveGroupRoles = size_t(CrowdMoveGroupRole::Count);

struct CrowdMoveGroupNames {
	std::array<std::string, kCrowdMoveGroupRoles> groups{ "MP_RunMoveGroup_PC", "MP_SprintMoveGroup", "MP_SneakMoveGroup_PC" };
	ChooserIndices indices; // Rifle: the crowd draws every item with the rifle cycles (as S3; pt1-run.md §6)
};

struct CrowdMoveGroup {
	bool ok = false; // read, every clip loaded and measured
	MoveGroupDef def;
	std::vector<CrowdClipData> clips; // per def.clips, bound into CrowdClipSet::package, tracked on CrowdClipSet::rig
	std::vector<MoveGroupClipTiming> timing; // per def.clips (poses: seconds 0)
	std::vector<Vec3> root_dir; // per def.clips: the measured unit root direction (zero for poses)
	float x_sign = -1.0f; // move_group_x's sign (measured)
	float worst_direction_deg = 0.0f; // the largest |measured root heading - its direction's X| over the moving clips
};

struct CrowdMoveGroupSet {
	std::array<CrowdMoveGroup, kCrowdMoveGroupRoles> groups;
	MoveGroupOptions options;
	std::vector<std::string> notes; // per group: what was read, or why it is not ok

	const CrowdMoveGroup &get(CrowdMoveGroupRole p_role) const { return groups[size_t(p_role)]; }
};

// Reads the named groups and loads their clips from p_set's package onto p_set's rig. Fails only when p_set has no
// package or rig or the library no static package; a group that does not read or load is a note (ok false) and the
// evaluator then keeps the stance's S3 / tp cycles for it.
core::Result<CrowdMoveGroupSet> load_crowd_move_groups(const CrowdClipSet &p_set, const core::ant::AntLibrary &p_library, const CrowdMoveGroupNames &p_names = {});

} // namespace bf3::anim
