#pragma once

#include "bf3/anim/pose.h"
#include "bf3/anim/pose_math.h"
#include "bf3/anim/skeleton.h"

#include <cstdint>
#include <vector>

namespace bf3::anim {

struct BoneMask {
	std::vector<float> weight;

	size_t size() const { return weight.size(); }
	size_t count_nonzero() const {
		size_t n = 0;
		for (const float w : weight) {
			n += w > 0.0f ? 1 : 0;
		}
		return n;
	}
};

BoneMask mask_from_tracks(const Skeleton &p_skeleton, const ClipTracks &p_tracks);

BoneMask mask_union(const BoneMask &p_a, const BoneMask &p_b);

LocalPose identity_pose(const Skeleton &p_skeleton);

void blend_poses(const LocalPose &p_a, const LocalPose &p_b, float p_weight, LocalPose &r_out);

void blend_poses(const LocalPose *const *p_poses, const float *p_weights, size_t p_count, LocalPose &r_out);

void override_masked(LocalPose &r_base, const LocalPose &p_layer, const BoneMask &p_mask, float p_weight);

enum class AdditiveOrder : uint8_t {
	LocalPost,
	LocalPre,
};

void apply_additive(LocalPose &r_base, const LocalPose &p_delta, const BoneMask &p_mask, float p_weight, AdditiveOrder p_order);

}
