#pragma once
// Skeletons for posing and skinning (plan §2.2 bf3_anim, §3.3; research/probes/ant-animation-probe.md §5, §7).
//
// Two sources describe the same characters:
//  * the Ant rig (AntRig, from s_BasicAssets): joint names, parents and the rest pose the clips animate
//    (rig defaults "<Layout>.<Joint>.q/.t"). Animation is evaluated on this hierarchy.
//  * the EBX SkeletonAsset of a mesh (animations/skeletons/venice1pske01 for 1P arms,
//    animations/skeletons/weapon/weaponske01 for weapons): BoneNames, Hierarchy, LocalPose and ModelPose.
//    A skinned MeshSet's bone slots are indices into this skeleton; its ModelPose is the bind pose, so the
//    inverse bind of bone b is inverse(ModelPose[b]).
// The two are matched by bone NAME, never by index (rig Wep_Root = 154, venice1pske01 = 153, weaponske01 = 1),
// and their hierarchies differ at the roots (rig Wep_Root under SpineX, weaponske01 under exportAnimation;
// the rig's AITrajectory adds a 90 deg turn about Y), which is why the bind pose comes from the mesh's own
// skeleton while the pose comes from the rig.

#include "bf3/anim/pose_math.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bf3::core {
class EbxPartition;
namespace ant {
struct AntRig;
}
} // namespace bf3::core

namespace bf3::anim {

struct Bone {
	std::string name;
	int32_t parent = -1; // -1 or an index below this bone's (validated)
	Xform rest; // local to the parent
};

class Skeleton {
public:
	// Fails when a parent index does not point at an earlier bone (the pose code walks bones in order).
	static core::Result<Skeleton> make(std::vector<Bone> p_bones);

	size_t size() const { return bones_.size(); }
	const std::vector<Bone> &bones() const { return bones_; }
	const Bone &bone(size_t p_index) const { return bones_[p_index]; }
	// First bone of that name (case-sensitive, like the data), or -1.
	int32_t find(std::string_view p_name) const;

private:
	std::vector<Bone> bones_;
	std::unordered_map<std::string, int32_t> index_;
};

// Skeleton from the Ant rig: the rig's joints in order, rest = rig defaults. Joints without a name get
// "joint<i>".
core::Result<Skeleton> skeleton_from_rig(const core::ant::AntRig &p_rig);

// An EBX SkeletonAsset as stored.
struct EbxSkeleton {
	std::string partition; // e.g. "animations/skeletons/venice1pske01"
	std::vector<std::string> bone_names;
	std::vector<int32_t> parents; // Hierarchy
	std::vector<Mat34> local_pose; // LocalPose, local to the parent
	std::vector<Mat34> model_pose; // ModelPose, mesh/model space (the bind pose)

	int32_t find(std::string_view p_name) const;
	size_t size() const { return bone_names.size(); }
};

// Reads the primary SkeletonAsset of a partition. Fails when the arrays disagree in length, a parent
// is out of range, or the instance is not a SkeletonAsset.
core::Result<EbxSkeleton> read_ebx_skeleton(const core::EbxPartition &p_partition, std::string p_name);
// Skeleton (names, parents, rest = LocalPose rotation/translation) of an EBX skeleton.
core::Result<Skeleton> skeleton_from_ebx(const EbxSkeleton &p_ebx);

// For each name, its bone index in p_skeleton (-1 when absent).
std::vector<int32_t> map_names(const std::vector<std::string> &p_names, const Skeleton &p_skeleton);

} // namespace bf3::anim
