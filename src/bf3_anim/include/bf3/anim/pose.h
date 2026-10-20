#pragma once

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
	std::vector<Xform> joints;
};

LocalPose rest_pose(const Skeleton &p_skeleton);

struct ClipTracks {
	std::vector<int32_t> rotation_bone;
	std::vector<int32_t> translation_bone;
	float fps = 30.0f;
	uint32_t end_frame = 0;
	uint32_t channels_bound = 0;
	uint32_t channels_unbound = 0;

	float duration_seconds() const { return fps > 0.0f ? float(end_frame) / fps : 0.0f; }
};

core::Result<ClipTracks> bind_clip_tracks(const core::ant::ClipBinding &p_binding, const core::ant::DecodedClip &p_clip,
		const Skeleton &p_skeleton);

void apply_clip_pose(const ClipTracks &p_tracks, const core::ant::ClipPose &p_sample, LocalPose &r_pose);

bool sample_pose(const Skeleton &p_skeleton, const core::ant::DecodedClip &p_clip, const ClipTracks &p_tracks, float p_frame, LocalPose &r_pose,
		core::ant::ClipPose &r_scratch);

void model_pose(const Skeleton &p_skeleton, const LocalPose &p_pose, std::vector<Xform> &r_model);

struct PoseDiff {
	double max_deg = 0.0;
	double max_m = 0.0;
	int32_t worst_rotation_bone = -1;
	int32_t worst_translation_bone = -1;
};
PoseDiff pose_diff(const std::vector<Xform> &p_a, const std::vector<Xform> &p_b, const std::vector<bool> &p_mask = {});

}
