#pragma once

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

enum class ServerPoseClip : uint8_t {
	StandIdle = 0,
	StandAimUp = 1,
	StandAimDown = 2,
	CrouchIdle = 3,
	CrouchAimUp = 4,
	CrouchAimDown = 5,
	ProneIdle = 6,
	ProneAimUp = 7,
	ProneAimDown = 8,
	StandToCrouch = 9,
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
	std::string root_joint = "AITrajectory";
};

struct ServerPoseClips {
	std::unique_ptr<core::ant::AntPackage> package;
	Skeleton rig;
	int32_t root_joint = -1;
	std::array<CrowdClipData, kServerPoseClips> clips;
	std::array<bool, kServerPoseClips> present{};
	std::vector<std::string> notes;

	const CrowdClipData &get(ServerPoseClip p_clip) const { return clips[size_t(p_clip)]; }
};

core::Result<ServerPoseClips> load_server_pose_clips(const core::ant::AntPackageSource &p_source, const core::ant::AntLibrary &p_library,
		const ServerPoseClipNames &p_names = {});

struct CapsuleTableInputs {
	std::span<const core::gameplay::HitCapsule> capsules;
	std::span<const uint8_t> property;
	std::span<const uint8_t> zone;

	std::array<std::vector<core::gameplay::CapsuleLocal>, core::gameplay::kCapsuleStances> standin;
	CrowdConfig aim;
};

core::Result<core::gameplay::CapsulePoseTable> build_capsule_pose_table(const Skeleton &p_rig, const ServerPoseClips &p_clips,
		const CapsuleTableInputs &p_inputs);

inline constexpr size_t kCapsuleDownBuckets = 14;
inline constexpr size_t kCapsuleUpBuckets = 18;
static_assert(kCapsuleDownBuckets + kCapsuleUpBuckets + 1 == core::gameplay::kCapsulePitchBuckets);
std::array<float, core::gameplay::kCapsulePitchBuckets> capsule_pitch_buckets(const CrowdConfig &p_aim);

float capsule_transition_fraction(size_t p_step);

inline constexpr float kMinAimFullDeg = 5.0f;
struct StanceAim {
	float up_full_deg = 78.1f;
	float down_full_deg = 60.7f;
	bool measured = false;
};
std::array<StanceAim, core::gameplay::kCapsuleStances> server_pose_stance_aim(const ServerPoseClips &p_clips, const CrowdConfig &p_aim,
		std::vector<std::string> *r_notes = nullptr);

float server_pose_aim_weight(const StanceAim &p_aim, float p_pitch_deg);

inline constexpr float kSphereCapsuleLength = 0.01f;

float capsule_axis_sign(const Skeleton &p_rig, int32_t p_bone, int32_t p_axis);

struct CapsuleTableReport {
	std::array<uint8_t, core::gameplay::kCapsuleRows> row_source{};
	std::array<StanceAim, core::gameplay::kCapsuleStances> aim{};
	std::vector<int32_t> joint;
	std::vector<std::string> notes;
	uint32_t pose_evaluations = 0;
	double seconds = 0;
};
core::Result<core::gameplay::CapsulePoseTable> build_capsule_pose_table(const Skeleton &p_rig, const ServerPoseClips &p_clips,
		const CapsuleTableInputs &p_inputs, CapsuleTableReport *r_report);

uint64_t capsule_table_source_hash(const Skeleton &p_rig, const ServerPoseClips &p_clips, const CapsuleTableInputs &p_inputs);

}
