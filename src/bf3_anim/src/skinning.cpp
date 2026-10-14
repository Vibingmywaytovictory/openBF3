#include "bf3/anim/skinning.h"

#include "bf3/core/res/mesh_set.h"

#include <string>

namespace bf3::anim {

core::Result<int32_t> SkinBinds::bind(const EbxSkeleton &p_ebx, uint32_t p_ebx_bone, const Skeleton &p_skeleton) {
	const auto key = std::make_pair(p_ebx.partition, p_ebx_bone);
	if (const auto it = by_source_.find(key); it != by_source_.end()) {
		return it->second;
	}
	if (p_ebx_bone >= p_ebx.size()) {
		return core::make_error("skin: bone " + std::to_string(p_ebx_bone) + " outside " + p_ebx.partition + " (" + std::to_string(p_ebx.size()) + " bones)");
	}
	const std::string &name = p_ebx.bone_names[p_ebx_bone];
	const int32_t bone = p_skeleton.find(name);
	if (bone < 0) {
		return core::make_error("skin: " + p_ebx.partition + " bone " + name + " has no bone of that name in the posed skeleton");
	}
	SkinBind b;
	b.bone = bone;
	b.name = name;
	if (!mat_inverse(p_ebx.model_pose[p_ebx_bone], b.inverse_bind)) {
		return core::make_error("skin: " + p_ebx.partition + " bone " + name + " has a singular ModelPose");
	}
	const int32_t index = int32_t(binds_.size());
	binds_.push_back(std::move(b));
	by_source_.emplace(key, index);
	return index;
}

core::Result<std::vector<std::vector<int32_t>>> SkinBinds::bind_lod(const core::res::MeshLod &p_lod, const EbxSkeleton &p_ebx, const Skeleton &p_skeleton) {
	std::vector<std::vector<int32_t>> lanes(p_lod.subsets.size());
	for (size_t s = 0; s < p_lod.subsets.size(); ++s) {
		const std::vector<uint16_t> &slots = p_lod.subsets[s].bone_slots;
		lanes[s].reserve(slots.size());
		for (uint16_t slot : slots) {
			core::Result<int32_t> b = bind(p_ebx, slot, p_skeleton);
			if (!b) {
				return core::error_at("lod " + p_lod.name + " subset " + std::to_string(s), b.error());
			}
			lanes[s].push_back(*b);
		}
	}
	return lanes;
}

void skin_matrices(const std::vector<Xform> &p_model, const std::vector<SkinBind> &p_binds, std::vector<Mat34> &r_matrices) {
	r_matrices.resize(p_binds.size());
	for (size_t i = 0; i < p_binds.size(); ++i) {
		const SkinBind &b = p_binds[i];
		const Mat34 pose = (b.bone >= 0 && size_t(b.bone) < p_model.size()) ? mat_from_xform(p_model[size_t(b.bone)]) : Mat34{};
		r_matrices[i] = mat_mul(pose, b.inverse_bind);
	}
}

} // namespace bf3::anim
