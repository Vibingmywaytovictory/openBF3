#pragma once
// Poses and clip playback on a Skeleton (plan §3.3 "Pose SoA, sampler"; research/probes/ant-animation-probe.md §5).
//
//   ClipTracks tracks = bind_clip_tracks(binding, clip, skeleton);   // channel -> bone, once per clip
//   LocalPose pose = rest_pose(skeleton);
//   sample_pose(clip, tracks, frame, pose, scratch);                  // rest + the clip's channels
//   model_pose(skeleton, pose, model);                                // local -> model space
//
// A clip channel drives the rig joint ClipBinding::tracks[c].joint; bind_clip_tracks maps that joint to a
// bone of the target skeleton by name, so any skeleton that names the rig's joints can be posed (the rig
// skeleton itself, or an EBX skeleton). Quaternion channels set the bone's local rotation, vec3 channels on
// a Translation DOF its local translation; Scale and float DOFs are ignored (none in the 1P weapon clips).
// Joints the clip does not drive keep their rest transform.

#include "bf3/anim/pose_math.h"
#include "bf3/anim/skeleton.h"
#include "bf3/core/ant/ant_clip.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <vector>

namespace bf3::core::ant {
struct ClipBinding;
}

namespace bf3::anim {

struct LocalPose {
	std::vector<Xform> joints; // one per bone, local to the parent
};

LocalPose rest_pose(const Skeleton &p_skeleton);

struct ClipTracks {
	std::vector<int32_t> rotation_bone; // per quaternion channel: target bone or -1
	std::vector<int32_t> translation_bone; // per vec3 channel: target bone or -1 (also -1 for non-translation DOFs)
	float fps = 30.0f; // controller FPS (frames per second of the clip's key times)
	uint32_t end_frame = 0;
	uint32_t channels_bound = 0; // channels that landed on a bone
	uint32_t channels_unbound = 0; // channels whose joint the skeleton does not name (or no joint)

	float duration_seconds() const { return fps > 0.0f ? float(end_frame) / fps : 0.0f; }
};

// Maps every channel of a bound clip to the target skeleton. Fails when the binding has no rig or its
// track count does not match the clip's channel counts.
core::Result<ClipTracks> bind_clip_tracks(const core::ant::ClipBinding &p_binding, const core::ant::DecodedClip &p_clip,
		const Skeleton &p_skeleton);

// Writes a sampled clip pose into r_pose (bones the clip does not drive are left as they are).
void apply_clip_pose(const ClipTracks &p_tracks, const core::ant::ClipPose &p_sample, LocalPose &r_pose);
// rest pose + the clip sampled at p_frame (fractional frames interpolate; see core::ant::sample_clip).
// r_scratch is reused between calls. Returns false when the clip arrays are inconsistent.
bool sample_pose(const Skeleton &p_skeleton, const core::ant::DecodedClip &p_clip, const ClipTracks &p_tracks, float p_frame, LocalPose &r_pose,
		core::ant::ClipPose &r_scratch);

// Local -> model space (bones are ordered parents first, see Skeleton::make).
void model_pose(const Skeleton &p_skeleton, const LocalPose &p_pose, std::vector<Xform> &r_model);

// Largest rotation (degrees) and translation (metres) difference between two model-space poses over
// the bones where p_mask is true (all bones when p_mask is empty).
struct PoseDiff {
	double max_deg = 0.0;
	double max_m = 0.0;
	int32_t worst_rotation_bone = -1;
	int32_t worst_translation_bone = -1;
};
PoseDiff pose_diff(const std::vector<Xform> &p_a, const std::vector<Xform> &p_b, const std::vector<bool> &p_mask = {});

} // namespace bf3::anim
