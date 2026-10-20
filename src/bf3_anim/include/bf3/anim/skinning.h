#pragma once

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
	int32_t bone = -1;
	std::string name;
	Mat34 inverse_bind;
};

class SkinBinds {
public:

	core::Result<int32_t> bind(const EbxSkeleton &p_ebx, uint32_t p_ebx_bone, const Skeleton &p_skeleton);

	core::Result<std::vector<std::vector<int32_t>>> bind_lod(const core::res::MeshLod &p_lod, const EbxSkeleton &p_ebx, const Skeleton &p_skeleton);

	const std::vector<SkinBind> &binds() const { return binds_; }
	size_t size() const { return binds_.size(); }

private:
	std::vector<SkinBind> binds_;
	std::map<std::pair<std::string, uint32_t>, int32_t> by_source_;
};

void skin_matrices(const std::vector<Xform> &p_model, const std::vector<SkinBind> &p_binds, std::vector<Mat34> &r_matrices);

}
