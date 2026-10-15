#pragma once
// Pose layering for the 3p crowd (S3; docs/design/s3.md §5.3-§5.5; plan §3.3 "blend, additive, masks"):
// weighted blends of full poses, a masked override (the upper-body reload over the run blend) and additive
// layers (the aim pose and the run noise).
//
//   LocalPose run = blend of the two direction cycles     (blend_poses)
//   override_masked(run, reload, reload_mask, w_reload)    (the bones the reload drives, nothing else)
//   apply_additive(run, aim_delta, aim_mask, 1, order)     (an aim-up/down delta on top)
//
// Masks come from the data, never from a list of bone names: a clip's mask is the set of bones its channels
// drive (mask_from_tracks). The reload Reload_NoAddon_Stand drives 94 rotations and 37 translations on Spine,
// Spine1, Spine2, the arms, the roll joints and the Wep_* bones. It drives neither Hips nor the legs
// [verified 2026-10-15], so its DOF set is exactly the upper-body mask.
//
// Additive clips store deltas, not poses [verified: Rifle_StandAimFwd samples to identity on every channel
// (0.0 deg, 0 m); Rifle_StandAimUp reaches 36 deg (Wep_Root 36, LeftArm 35, Spine 21)]. An additive clip is
// therefore sampled onto identity_pose, not rest_pose. Which side the delta multiplies on is open
// (s3.md §16 Q2). AdditiveOrder names both; the ctest anim.s3 picks one by the measured weapon pitch.

#include "bf3/anim/pose.h"
#include "bf3/anim/pose_math.h"
#include "bf3/anim/skeleton.h"

#include <cstdint>
#include <vector>

namespace bf3::anim {

// Per-bone layer weight in [0, 1], one per skeleton bone.
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

// 1 for every bone a channel of p_tracks drives (rotation or translation), 0 elsewhere.
BoneMask mask_from_tracks(const Skeleton &p_skeleton, const ClipTracks &p_tracks);
// Union (per-bone maximum) of two masks of the same size; an empty mask counts as all zero.
BoneMask mask_union(const BoneMask &p_a, const BoneMask &p_b);

// Every joint identity (the base an additive clip is sampled onto).
LocalPose identity_pose(const Skeleton &p_skeleton);

// r_out = lerp(p_a, p_b, p_weight) per joint: shortest-arc normalized lerp of rotations, linear translations.
// The three poses have the same size. r_out may alias p_a.
void blend_poses(const LocalPose &p_a, const LocalPose &p_b, float p_weight, LocalPose &r_out);
// r_out = sum of p_weights[i] * p_poses[i] (rotations sign-aligned to the first pose, then normalized);
// weights below 1e-4 are skipped, and the weights need not sum to 1 (they are normalized). Locomotion
// uses at most three inputs (two direction cycles and idle).
void blend_poses(const LocalPose *const *p_poses, const float *p_weights, size_t p_count, LocalPose &r_out);
// r_base[j] = lerp(r_base[j], p_layer[j], p_weight * p_mask[j]) for every joint.
void override_masked(LocalPose &r_base, const LocalPose &p_layer, const BoneMask &p_mask, float p_weight);

enum class AdditiveOrder : uint8_t {
	LocalPost, // q = q_base * slerp(identity, q_delta, w); t = t_base + w * t_delta
	LocalPre, // q = slerp(identity, q_delta, w) * q_base; t = t_base + w * t_delta
};
// Adds a delta pose (sampled onto identity_pose) scaled by p_weight * p_mask[j].
void apply_additive(LocalPose &r_base, const LocalPose &p_delta, const BoneMask &p_mask, float p_weight, AdditiveOrder p_order);

} // namespace bf3::anim
