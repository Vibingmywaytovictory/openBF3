#pragma once

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
}

namespace bf3::anim {

struct Bone {
	std::string name;
	int32_t parent = -1;
	Xform rest;
};

class Skeleton {
public:

	static core::Result<Skeleton> make(std::vector<Bone> p_bones);

	size_t size() const { return bones_.size(); }
	const std::vector<Bone> &bones() const { return bones_; }
	const Bone &bone(size_t p_index) const { return bones_[p_index]; }

	int32_t find(std::string_view p_name) const;

private:
	std::vector<Bone> bones_;
	std::unordered_map<std::string, int32_t> index_;
};

core::Result<Skeleton> skeleton_from_rig(const core::ant::AntRig &p_rig);

struct EbxSkeleton {
	std::string partition;
	std::vector<std::string> bone_names;
	std::vector<int32_t> parents;
	std::vector<Mat34> local_pose;
	std::vector<Mat34> model_pose;

	int32_t find(std::string_view p_name) const;
	size_t size() const { return bone_names.size(); }
};

core::Result<EbxSkeleton> read_ebx_skeleton(const core::EbxPartition &p_partition, std::string p_name);

core::Result<Skeleton> skeleton_from_ebx(const EbxSkeleton &p_ebx);

std::vector<int32_t> map_names(const std::vector<std::string> &p_names, const Skeleton &p_skeleton);

}
