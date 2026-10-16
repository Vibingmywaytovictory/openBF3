#pragma once
// The server pose of the hit capsules (plan R14; docs/design/m2a.md §6.5; owner lc): the 3p rig evaluated per stance
// at its idle clip with the additive aim at 33 pitch buckets, and per stance transition at 8 fractions of its clip,
// with the 9 hi-LOD capsules placed from the bones' model transforms into a CapsulePoseTable. Godot-free; the server
// links it (the new bf3_host -> bf3_anim link, §2), the client builds the same table on its loader thread.
//
// Clip source [verified: antdump list --install b_basicassetsmp, 2026-10-16; every controller name occurs exactly
// once, all on rig VeniceAntSke01]: the stance idles and aims of the stand (S3's), crouch and prone, and the six
// Rifle_T_* transitions (Dct, end frames 30 / 32 / 28 / 42 / 39 / 44). tp (wave 3) loads the same names through
// the same ServerPoseClipNames, an additive extension of S3's CrowdClipNames, and the same per-stance stand-in rule,
// so the drawn soldier and the server's capsules come from one source.
//
// Fallback per row: a row whose clip is missing, fails to bind or is not additive where required is built from wp's
// standin_target layout for that stance (transition rows: a lerp of the two stance rows by the fraction) and tagged
// capsule_row_source::kStandIn. Capsule bones are mapped to rig joints by name (as antdump `rigmap` does).

#include "bf3/anim/crowd_clips.h"
#include "bf3/anim/crowd_eval.h"
#include "bf3/anim/skeleton.h"
#include "bf3/core/ant/ant_library.h"
#include "bf3/core/ant/ant_source.h"
#include "bf3/core/gameplay/capsule_pose_table.h"
#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bf3::anim {

// Roles of the server-pose clips, in ServerPoseClipNames::controllers order.
enum class ServerPoseClip : uint8_t {
	StandIdle = 0, // Rifle_StandIdle_Pose (S3's Idle)
	StandAimUp = 1, // Rifle_StandAimUp (additive)
	StandAimDown = 2, // Rifle_StandAimDwn (additive)
	CrouchIdle = 3, // Rifle_CrouchIdle_Pose
	CrouchAimUp = 4, // Rifle_CrouchAimUp
	CrouchAimDown = 5, // Rifle_CrouchAimDwn
	ProneIdle = 6, // Rifle_ProneIdle_Pose
	ProneAimUp = 7, // Rifle_ProneAimUpPose
	ProneAimDown = 8, // Rifle_ProneAimDwnPose
	StandToCrouch = 9, // Rifle_T_StandToCrouch; the six transitions follow core::gameplay::CapsuleTransition order
	StandToProne = 10,
	CrouchToStand = 11,
	CrouchToProne = 12,
	ProneToStand = 13,
	ProneToCrouch = 14,
	Count
};
inline constexpr size_t kServerPoseClips = size_t(ServerPoseClip::Count);
static_assert(size_t(ServerPoseClip::ProneToCrouch) - size_t(ServerPoseClip::StandToCrouch) + 1 == core::gameplay::kCapsuleTransitions);

struct ServerPoseClipNames {
	std::string package = "b_basicassetsmp";
	std::array<std::string, kServerPoseClips> controllers{ "Rifle_StandIdle_Pose", "Rifle_StandAimUp", "Rifle_StandAimDwn", "Rifle_CrouchIdle_Pose",
		"Rifle_CrouchAimUp", "Rifle_CrouchAimDwn", "Rifle_ProneIdle_Pose", "Rifle_ProneAimUpPose", "Rifle_ProneAimDwnPose", "Rifle_T_StandToCrouch",
		"Rifle_T_StandToProne", "Rifle_T_CrouchToStand", "Rifle_T_CrouchToProne", "Rifle_T_ProneToStand", "Rifle_T_ProneToCrouch" };
	std::string root_joint = "AITrajectory"; // as CrowdClipNames: its translation (root motion) is removed
};

// The decoded clips on the rig. A role whose clip is absent or unusable has present[role] false and the reason in
// notes; the table builder then uses the stand-in rule for the rows that need it.
struct ServerPoseClips {
	std::unique_ptr<core::ant::AntPackage> package; // owns the bytes the bindings point into
	Skeleton rig; // skeleton_from_rig of the clips' rig (VeniceAntSke01)
	int32_t root_joint = -1;
	std::array<CrowdClipData, kServerPoseClips> clips;
	std::array<bool, kServerPoseClips> present{};
	std::vector<std::string> notes;

	const CrowdClipData &get(ServerPoseClip p_clip) const { return clips[size_t(p_clip)]; }
};

// Loads the package and binds and decodes every named clip it can. Fails only when the package or the rig cannot be
// read; a single missing or unusable clip is recorded (present false + a note), not an error.
core::Result<ServerPoseClips> load_server_pose_clips(const core::ant::AntPackageSource &p_source, const core::ant::AntLibrary &p_library,
		const ServerPoseClipNames &p_names = {});

// What the builder needs besides the clips. bf3_anim does not link bf3_sim, so the material lookups and wp's
// stand-in layout come in as plain data (the caller, bf3_host's level load, fills them from the
// SurfaceMaterialTable and sim::weapons::standin_target at feet (0, 0, 0), yaw 0).
struct CapsuleTableInputs {
	std::span<const core::gameplay::HitCapsule> capsules; // read_hit_capsules; only valid_hi_lod ones are placed
	std::span<const uint8_t> property; // per capsule: the pair's grid property (SurfaceMaterialTable)
	std::span<const uint8_t> zone; // per capsule: sim::weapons::HitZone
	// wp's standin_target layout per stance (Stand, Crouch, Prone), local frame, the hi-LOD capsules in list order:
	// the rows of the stand-in rule.
	std::array<std::vector<core::gameplay::CapsuleLocal>, core::gameplay::kCapsuleStances> standin;
	CrowdConfig aim; // full-pitch weights (S3's 78.1 deg up / 60.7 deg down) and the additive order
};

// Builds the table (capsule_pose_table.h layout) from the clips and the hi-LOD capsules (their bone, offset, length,
// axis and radius [verified: material_grid.h]); rows without usable clips come from p_inputs.standin and are tagged.
// Fails when the capsule list is empty, the per-capsule spans do not match it, or no capsule maps to a rig joint.
core::Result<core::gameplay::CapsulePoseTable> build_capsule_pose_table(const Skeleton &p_rig, const ServerPoseClips &p_clips,
		const CapsuleTableInputs &p_inputs);

// ---- lc additions (as built; docs/design/m2a-lc.md) ----------------------------------------------------------------

// The rules the builder follows, named so tp (wave 3) and the E18 reference evaluation can follow the same ones.
//
// Pitch buckets: kCapsuleDownBuckets equal steps from -aim_down_full_pitch_deg to 0 and kCapsuleUpBuckets from 0 to
// +aim_up_full_pitch_deg (S3's 60.7 / 78.1), so 0 is a bucket (the additive switches clips at 0) and the steps are
// ~4.34 deg.
inline constexpr size_t kCapsuleDownBuckets = 14;
inline constexpr size_t kCapsuleUpBuckets = 18;
static_assert(kCapsuleDownBuckets + kCapsuleUpBuckets + 1 == core::gameplay::kCapsulePitchBuckets);
std::array<float, core::gameplay::kCapsulePitchBuckets> capsule_pitch_buckets(const CrowdConfig &p_aim);
// Transition fractions: fraction k of kCapsuleTransitionFractions is k / (kCapsuleTransitionFractions - 1) of the
// clip's own length (frame = fraction x end_frame): 0 and 1 are steps, so 7 intervals.
float capsule_transition_fraction(size_t p_step);

// Aim per stance: the additive weight of AimUp / AimDown is min(1, |pitch| / full) with full = the barrel elevation
// change of that stance's aim clip at weight 1 over its idle (S3's measure_aim, LocalPost, the same rule that gave
// stand 78.1 / 60.7). Stand takes the CrowdConfig values; crouch and prone are measured from their clips and fall
// back to the stand values when the measurement fails or gives less than kMinAimFullDeg (reported).
inline constexpr float kMinAimFullDeg = 5.0f;
struct StanceAim {
	float up_full_deg = 78.1f;
	float down_full_deg = 60.7f;
	bool measured = false; // false: the CrowdConfig values (stand, or a fallback)
};
std::array<StanceAim, core::gameplay::kCapsuleStances> server_pose_stance_aim(const ServerPoseClips &p_clips, const CrowdConfig &p_aim,
		std::vector<std::string> *r_notes = nullptr);
// The additive weight for p_pitch_deg of a stance (> 0 uses AimUp, < 0 AimDown, 0 none).
float server_pose_aim_weight(const StanceAim &p_aim, float p_pitch_deg);

// Capsule placement from a bone's model transform [inferred; docs/design/m2a-lc.md "capsule placement"]: the offset
// is a model-space vector of the bind pose carried by the bone, so segment start a = bone position + (bone rotation x
// inverse bind rotation) applied to the offset; end b = a + length along the bone's local axis BoneAxis (0 x, 1 y,
// 2 z), signed toward the bone's main child (the child with the longest rest translation; + when it has none). A
// length of at most 0.01 m (the head) is a sphere at a (b = a). The bind pose is the rig's rest (equal to the EBX
// skeleton's ModelPose). Read as a bone-local point instead, the two Spine capsules (offsets y -0.1 and +0.25) would
// sit behind and 25 cm in front of the spine at one height rather than belly below chest (the Chest sphere's centre
// 0.94 m and 15 cm before the body in the stand idle); bf3dump lagcomp prints both readings.
inline constexpr float kSphereCapsuleLength = 0.01f;
// The sign of a bone's axis (+1 or -1) by that rule.
float capsule_axis_sign(const Skeleton &p_rig, int32_t p_bone, int32_t p_axis);

// What the builder did (E18 prints it): the row tags and why, the aim per stance, the capsule -> joint map.
struct CapsuleTableReport {
	std::array<uint8_t, core::gameplay::kCapsuleRows> row_source{};
	std::array<StanceAim, core::gameplay::kCapsuleStances> aim{};
	std::vector<int32_t> joint; // per placed (hi-LOD) capsule: rig joint, -1 = unmapped (placed from the stand-in)
	std::vector<std::string> notes;
	uint32_t pose_evaluations = 0;
	double seconds = 0;
};
core::Result<core::gameplay::CapsulePoseTable> build_capsule_pose_table(const Skeleton &p_rig, const ServerPoseClips &p_clips,
		const CapsuleTableInputs &p_inputs, CapsuleTableReport *r_report);

// The capsules/ cache key inputs (m2a.md §13.1): FNV-1a 64 over the rig (joint names, parents, rest), every clip's
// decoded keys and channel binding, the capsule list and the inputs; with kCapsuleTableVersion.
uint64_t capsule_table_source_hash(const Skeleton &p_rig, const ServerPoseClips &p_clips, const CapsuleTableInputs &p_inputs);

} // namespace bf3::anim
