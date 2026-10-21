#include "bf3/anim/skeleton.h"

#include "bf3/core/ant/ant_library.h"
#include "bf3/core/ebx.h"

#include <string>

namespace bf3::anim {

core::Result<Skeleton> Skeleton::make(std::vector<Bone> p_bones) {
	Skeleton s;
	for (size_t i = 0; i < p_bones.size(); ++i) {
		const int32_t parent = p_bones[i].parent;
		if (parent < -1 || parent >= int32_t(i)) {
			return core::make_error("skeleton: bone " + std::to_string(i) + " (" + p_bones[i].name + ") has parent " + std::to_string(parent) +
					", not an earlier bone");
		}
		p_bones[i].rest.q = quat_normalized(p_bones[i].rest.q);
		s.index_.emplace(p_bones[i].name, int32_t(i));
	}
	s.bones_ = std::move(p_bones);
	return s;
}

int32_t Skeleton::find(std::string_view p_name) const {
	const auto it = index_.find(std::string(p_name));
	return it == index_.end() ? -1 : it->second;
}

core::Result<Skeleton> skeleton_from_rig(const core::ant::AntRig &p_rig) {
	std::vector<Bone> bones;
	bones.reserve(p_rig.joints.size());
	for (size_t i = 0; i < p_rig.joints.size(); ++i) {
		const core::ant::AntJoint &j = p_rig.joints[i];
		Bone b;
		b.name = j.name.empty() ? "joint" + std::to_string(i) : std::string(j.name);
		b.parent = j.parent;
		b.rest.q = Quat{ j.rest_q[0], j.rest_q[1], j.rest_q[2], j.rest_q[3] };
		b.rest.t = Vec3{ j.rest_t[0], j.rest_t[1], j.rest_t[2] };
		bones.push_back(std::move(b));
	}
	core::Result<Skeleton> s = Skeleton::make(std::move(bones));
	if (!s) {
		return core::error_at("rig " + std::string(p_rig.name), s.error());
	}
	return s;
}

int32_t EbxSkeleton::find(std::string_view p_name) const {
	for (size_t i = 0; i < bone_names.size(); ++i) {
		if (bone_names[i] == p_name) {
			return int32_t(i);
		}
	}
	return -1;
}

namespace {

bool read_transforms(const core::EbxInstance &p_instance, const char *p_field, std::vector<Mat34> &r_out) {
	const std::optional<core::EbxValue> v = p_instance.get(p_field);
	if (!v || !v->as_array()) {
		return false;
	}
	for (core::EbxValue e : *v->as_array()) {
		const std::optional<std::array<core::Vec3, 4>> rows = e.as_transform();
		if (!rows) {
			return false;
		}
		r_out.push_back(mat_from_linear_transform(*rows));
	}
	return true;
}

}

core::Result<EbxSkeleton> read_ebx_skeleton(const core::EbxPartition &p_partition, std::string p_name) {
	const core::EbxInstance prim = p_partition.primary();
	const std::string where = "skeleton " + p_name;
	if (!prim.is_a("SkeletonAsset")) {
		return core::make_error(where + ": primary instance is " + std::string(prim.type_name()) + ", not a SkeletonAsset");
	}
	EbxSkeleton s;
	s.partition = std::move(p_name);
	const std::optional<core::EbxValue> names = prim.get("BoneNames");
	const std::optional<core::EbxValue> hierarchy = prim.get("Hierarchy");
	if (!names || !names->as_array() || !hierarchy || !hierarchy->as_array()) {
		return core::make_error(where + ": no BoneNames / Hierarchy");
	}
	for (core::EbxValue v : *names->as_array()) {
		s.bone_names.push_back(std::string(v.as_str().value_or("")));
	}
	for (core::EbxValue v : *hierarchy->as_array()) {
		s.parents.push_back(v.as_i32().value_or(-2));
	}
	if (!read_transforms(prim, "LocalPose", s.local_pose) || !read_transforms(prim, "ModelPose", s.model_pose)) {
		return core::make_error(where + ": LocalPose / ModelPose unreadable");
	}
	const size_t n = s.bone_names.size();
	if (s.parents.size() != n || s.local_pose.size() != n || s.model_pose.size() != n) {
		return core::make_error(where + ": " + std::to_string(n) + " names, " + std::to_string(s.parents.size()) + " parents, " +
				std::to_string(s.local_pose.size()) + " local and " + std::to_string(s.model_pose.size()) + " model transforms");
	}
	for (size_t i = 0; i < n; ++i) {
		if (s.parents[i] < -1 || s.parents[i] >= int32_t(n)) {
			return core::make_error(where + ": bone " + std::to_string(i) + " has parent " + std::to_string(s.parents[i]));
		}
	}
	return s;
}

core::Result<Skeleton> skeleton_from_ebx(const EbxSkeleton &p_ebx) {
	std::vector<Bone> bones(p_ebx.size());
	for (size_t i = 0; i < bones.size(); ++i) {
		bones[i].name = p_ebx.bone_names[i];
		bones[i].parent = p_ebx.parents[i];
		bones[i].rest.q = quat_from_mat(p_ebx.local_pose[i]);
		bones[i].rest.t = p_ebx.local_pose[i].origin();
	}
	return Skeleton::make(std::move(bones));
}

std::vector<int32_t> map_names(const std::vector<std::string> &p_names, const Skeleton &p_skeleton) {
	std::vector<int32_t> out;
	out.reserve(p_names.size());
	for (const std::string &n : p_names) {
		out.push_back(p_skeleton.find(n));
	}
	return out;
}

}
