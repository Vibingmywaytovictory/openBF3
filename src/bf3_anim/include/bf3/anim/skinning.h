#pragma once
// Skinning binds between skinned MeshSets and a posed skeleton (research/probes/ant-animation-probe.md §7;
// README "Skinned meshes").
//
// A vertex's bone lane l in subset s names the mesh skeleton's bone MeshLod::skeleton_bone(s, l) (an index
// into the mesh's EBX SkeletonAsset). That bone is found in the posed skeleton by name; its inverse bind is
// inverse(EbxSkeleton::model_pose[index]). One SkinBinds list can collect the binds of several meshes (arms
// and weapon share one list, so a renderer needs one skin per character): a bind is the pair (posed bone,
// inverse bind), and meshes that name the same pair share it.
//
//   skin matrix of bind i = model_pose[binds[i].bone] * binds[i].inverse_bind
//
// which maps the mesh's bind-pose vertices into the posed skeleton's model space.

#include "bf3/anim/pose_math.h"
#include "bf3/anim/skeleton.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace bf3::core::res {
struct MeshLod;
}

namespace bf3::anim {

struct SkinBind {
	int32_t bone = -1; // bone of the posed skeleton
	std::string name; // its name
	Mat34 inverse_bind; // mesh space -> bone space
};

class SkinBinds {
public:
	// The bind for EBX bone p_ebx_bone of p_ebx, posed by the same-named bone of p_skeleton; added on first
	// use. Fails when the index is out of range, the name is absent from p_skeleton, or the bind pose is
	// singular.
	core::Result<int32_t> bind(const EbxSkeleton &p_ebx, uint32_t p_ebx_bone, const Skeleton &p_skeleton);
	// Per bone lane of every subset of p_lod: the bind index (lane_binds[subset][lane]), from the LOD's bone
	// slots. Every slot must resolve; returns the first failure.
	core::Result<std::vector<std::vector<int32_t>>> bind_lod(const core::res::MeshLod &p_lod, const EbxSkeleton &p_ebx, const Skeleton &p_skeleton);

	const std::vector<SkinBind> &binds() const { return binds_; }
	size_t size() const { return binds_.size(); }

private:
	std::vector<SkinBind> binds_;
	std::map<std::pair<std::string, uint32_t>, int32_t> by_source_; // (ebx partition, ebx bone) -> bind
};

// Skin matrix per bind for a model-space pose of the skeleton the binds were made against.
void skin_matrices(const std::vector<Xform> &p_model, const std::vector<SkinBind> &p_binds, std::vector<Mat34> &r_matrices);

} // namespace bf3::anim
