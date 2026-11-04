#include "bf3/anim/hand_reach.h"

#include "bf3/core/ant/ant_library.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/math.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace bf3::anim {

namespace {

using core::ant::AntLibrary;
using core::ant::AntPackage;
using core::ant::AntRig;
using core::ant::GdArray;
using core::ant::GdGuid;
using core::ant::GdStruct;

constexpr uint32_t kMaxFeatureEntries = 256;
constexpr float kMinSegmentMetres = 1e-5f;
constexpr double kSamePlaceMetres = 1e-7;
constexpr double kSameTurnDegrees = 1e-4;
constexpr std::array<std::string_view, 2> kSides = { "Left", "Right" };

GdStruct resolve_field(
		const AntLibrary &p_library, const AntPackage &p_package, const GdStruct &p_from, std::string_view p_field) {
	GdGuid g;
	return p_from.valid() && p_from.read_guid(p_field, g) && !g.is_null() ? p_library.resolve(p_package, g)
																		  : GdStruct();
}

int32_t rig_joint(const AntRig &p_ant, const Skeleton &p_rig, int64_t p_ant_joint) {
	if (p_ant_joint < 0 || size_t(p_ant_joint) >= p_ant.joints.size()) {
		return -1;
	}
	return p_rig.find(p_ant.joints[size_t(p_ant_joint)].name);
}

bool ancestor_of(const Skeleton &p_rig, int32_t p_ancestor, int32_t p_joint) {
	for (int32_t j = p_joint; j >= 0 && size_t(j) < p_rig.size(); j = p_rig.bone(size_t(j)).parent) {
		if (j == p_ancestor) {
			return true;
		}
	}
	return false;
}

void refresh_chain(
		const Skeleton &p_rig, const LocalPose &p_pose, int32_t p_from, int32_t p_to, std::vector<Xform> &r_model) {
	std::array<int32_t, 32> path{};
	size_t n = 0;
	for (int32_t j = p_to; j >= 0 && n < path.size(); j = p_rig.bone(size_t(j)).parent) {
		path[n++] = j;
		if (j == p_from) {
			break;
		}
	}
	for (size_t k = n; k-- > 0;) {
		const int32_t j = path[k];
		const int32_t parent = p_rig.bone(size_t(j)).parent;
		r_model[size_t(j)] =
				parent >= 0 ? xform_mul(r_model[size_t(parent)], p_pose.joints[size_t(j)]) : p_pose.joints[size_t(j)];
	}
}

void set_model_rotation(const Skeleton &p_rig, int32_t p_joint, const Quat &p_model_q,
		const std::vector<Xform> &p_model, LocalPose &r_pose) {
	const int32_t parent = p_rig.bone(size_t(p_joint)).parent;
	const Quat parent_q = parent >= 0 ? p_model[size_t(parent)].q : Quat{};
	r_pose.joints[size_t(p_joint)].q = quat_normalized(quat_mul(quat_conj(parent_q), p_model_q));
}

Vec3 bend_axis(const Vec3 &p_to_shoulder, const Vec3 &p_to_hand, const Vec3 &p_to_target) {
	constexpr float kStraight = kMinSegmentMetres * kMinSegmentMetres;
	for (const Vec3 &other : { p_to_hand, p_to_target, Vec3{ 1.0f, 0.0f, 0.0f }, Vec3{ 0.0f, 1.0f, 0.0f } }) {
		const Vec3 n = core::vec::cross(p_to_shoulder, other);
		if (core::vec::length(n) > kStraight) {
			return core::vec::normalize(n);
		}
	}
	return Vec3{};
}

float elbow_angle(float p_upper, float p_lower, float p_span) {
	const float c = (p_upper * p_upper + p_lower * p_lower - p_span * p_span) / (2.0f * p_upper * p_lower);
	return std::acos(std::clamp(c, -1.0f, 1.0f));
}

void reach_arm(const Skeleton &p_rig, const HandReachArm &p_arm, const std::vector<Xform> &p_held, LocalPose &r_pose,
		std::vector<Xform> &r_model) {
	const float w = std::clamp(std::isfinite(p_arm.reach) ? p_arm.reach : 0.0f, 0.0f, 1.0f);
	if (!(w > 0.0f)) {
		return;
	}
	const Vec3 a = r_model[size_t(p_arm.arm)].t, b = r_model[size_t(p_arm.fore)].t, c = r_model[size_t(p_arm.hand)].t;
	const Xform place = xform_mul(r_model[size_t(p_arm.effector)],
			xform_mul(xform_inverse(p_held[size_t(p_arm.effector)]), p_held[size_t(p_arm.hand)]));
	if (vec_dist(place.t, c) < kSamePlaceMetres &&
			quat_angle_deg(place.q, r_model[size_t(p_arm.hand)].q) < kSameTurnDegrees) {
		return;
	}
	const Vec3 target = vec_add(c, vec_scale(vec_sub(place.t, c), w));
	const Quat target_q = core::ant::quat_slerp(r_model[size_t(p_arm.hand)].q, place.q, w);
	const float upper = core::vec::length(vec_sub(b, a)), lower = core::vec::length(vec_sub(c, b));
	if (!(upper > kMinSegmentMetres && lower > kMinSegmentMetres) || !std::isfinite(target.x + target.y + target.z)) {
		return;
	}
	const float span = std::clamp(core::vec::length(vec_sub(target, a)), std::fabs(upper - lower) + kMinSegmentMetres,
			upper + lower - kMinSegmentMetres);
	const Vec3 bend = bend_axis(vec_sub(a, b), vec_sub(c, b), vec_sub(target, a));
	if (core::vec::length(bend) > 0.0f) {
		const float turn =
				elbow_angle(upper, lower, span) - elbow_angle(upper, lower, core::vec::length(vec_sub(c, a)));
		const Quat q = quat_mul(quat_axis_angle(bend, turn), r_model[size_t(p_arm.fore)].q);
		set_model_rotation(p_rig, p_arm.fore, q, r_model, r_pose);
		refresh_chain(p_rig, r_pose, p_arm.fore, p_arm.hand, r_model);
	}
	const Quat swing = quat_from_to(vec_sub(r_model[size_t(p_arm.hand)].t, a), vec_sub(target, a));
	set_model_rotation(p_rig, p_arm.arm, quat_mul(swing, r_model[size_t(p_arm.arm)].q), r_model, r_pose);
	refresh_chain(p_rig, r_pose, p_arm.arm, p_arm.hand, r_model);
	set_model_rotation(p_rig, p_arm.hand, target_q, r_model, r_pose);
	refresh_chain(p_rig, r_pose, p_arm.hand, p_arm.hand, r_model);
}

}

bool HandReachArm::usable(size_t p_joints) const {
	auto in = [&](int32_t p_j) { return p_j >= 0 && size_t(p_j) < p_joints; };
	return in(arm) && in(fore) && in(hand) && in(effector) && std::isfinite(reach) && reach > 0.0f;
}

bool HandReach::any(size_t p_joints) const {
	return arms[0].usable(p_joints) || arms[1].usable(p_joints);
}

core::Result<HandReach> read_hand_reach(
		const AntLibrary &p_library, const Skeleton &p_rig, std::string_view p_rig_name) {
	const std::string where = "1p hand reach of rig " + std::string(p_rig_name);
	const AntPackage *stat = p_library.static_package();
	if (stat == nullptr) {
		return core::make_error(where + ": no static package");
	}
	const AntRig *ant = nullptr;
	for (const AntRig &r : p_library.rigs()) {
		if (r.name == p_rig_name) {
			ant = &r;
		}
	}
	if (ant == nullptr) {
		return core::make_error(where + ": no such Ant rig");
	}
	core::gameplay::WorkBudget budget;
	GdStruct rig_asset;
	uint32_t hits = 0;
	for (size_t i = 0; i < stat->object_count(); ++i) {
		if (core::Status s = budget.charge(1); !s.ok()) {
			return core::error_at(where, s.error());
		}
		const GdStruct o = stat->object(i);
		if (o.type_name() == "RigAsset" && o.name() == p_rig_name) {
			++hits;
			rig_asset = o;
		}
	}
	if (hits != 1) {
		return core::make_error(where + ": " + std::to_string(hits) + " RigAssets of that name (must be exactly one)");
	}
	const GdStruct collection = resolve_field(p_library, *stat, rig_asset, "FeatureCollection");
	const GdArray features = collection.valid() ? collection.array("FeatureAssets") : GdArray();
	GdStruct particle_ik, joint_to_effector;
	for (uint32_t i = 0; features.valid() && i < features.size() && i < kMaxFeatureEntries; ++i) {
		GdGuid g;
		const GdStruct f = features.read_guid(i, g) && !g.is_null() ? p_library.resolve(*stat, g) : GdStruct();
		if (core::ant::gd_is_a(f, "ParticleIKFeatureAsset")) {
			particle_ik = f;
		} else if (core::ant::gd_is_a(f, "JointToEffectorFeatureAsset")) {
			joint_to_effector = f;
		}
	}
	if (!particle_ik.valid() || !joint_to_effector.valid()) {
		return core::make_error(
				where + ": no ParticleIKFeatureAsset and JointToEffectorFeatureAsset in its FeatureCollection");
	}
	const GdStruct trajectory = resolve_field(p_library, *stat, particle_ik, "TrajectoryTemplate");
	const GdStruct humanoid = resolve_field(p_library, *stat, particle_ik, "HumanoidTemplate");
	const GdArray trajectory_ids = trajectory.valid() ? trajectory.array("IDList") : GdArray();
	const GdArray ids = humanoid.valid() ? humanoid.array("IDList") : GdArray();
	const GdArray mapped = particle_ik.array("MappedJoints");
	if (!trajectory_ids.valid() || !ids.valid() || !mapped.valid()) {
		return core::make_error(where +
				": the ParticleIK feature has no TrajectoryTemplate and HumanoidTemplate IDList or MappedJoints");
	}
	const uint32_t first = trajectory_ids.size();
	auto mapped_joint = [&](const std::string &p_name) {
		for (uint32_t k = 0; k < ids.size() && first + k < mapped.size() && k < kMaxFeatureEntries; ++k) {
			GdGuid g;
			const GdStruct id = ids.read_guid(k, g) && !g.is_null() ? p_library.resolve(*stat, g) : GdStruct();
			int64_t joint = -1;
			if (id.valid() && id.name() == p_name && mapped.read_int(first + k, joint)) {
				return rig_joint(*ant, p_rig, joint);
			}
		}
		return int32_t(-1);
	};
	HandReach out;
	for (size_t s = 0; s < kSides.size(); ++s) {
		const std::string side(kSides[s]);
		HandReachArm &arm = out.arms[s];
		arm.arm = mapped_joint(side + "Arm");
		arm.fore = mapped_joint(side + "ForeArm");
		arm.hand = mapped_joint(side + "Hand");
		if (arm.arm < 0 || arm.fore < 0 || arm.hand < 0 || !ancestor_of(p_rig, arm.arm, arm.fore) ||
				!ancestor_of(p_rig, arm.fore, arm.hand) || arm.arm == arm.fore || arm.fore == arm.hand) {
			return core::make_error(
					where + ": the ParticleIK feature maps no " + side + "Arm, ForeArm and Hand chain onto the rig");
		}
	}
	const GdArray effectors = joint_to_effector.array("EffectorJoints");
	const GdArray reaches = joint_to_effector.array("ReachFloats");
	for (uint32_t i = 0; effectors.valid() && reaches.valid() && i < effectors.size() && i < reaches.size() &&
			i < kMaxFeatureEntries;
			++i) {
		int64_t joint = -1;
		if (!effectors.read_int(i, joint) || joint < 0) {
			continue;
		}
		const int32_t effector = rig_joint(*ant, p_rig, joint);
		if (effector < 0) {
			return core::make_error(where + ": effector " + std::to_string(i) + " names no joint of the rig");
		}
		GdGuid g;
		const GdStruct reach = reaches.read_guid(i, g) && !g.is_null() ? p_library.resolve(*stat, g) : GdStruct();
		float value = 0.0f;
		if (!reach.valid() || !reach.read_float("Default", value) || !std::isfinite(value)) {
			return core::make_error(where + ": effector " + std::to_string(i) + " has no reach float with a Default");
		}
		const std::string_view name = p_rig.bone(size_t(effector)).name;
		for (HandReachArm &arm : out.arms) {
			if (name.ends_with(p_rig.bone(size_t(arm.hand)).name)) {
				if (arm.effector >= 0) {
					return core::make_error(where + ": two effectors for " + p_rig.bone(size_t(arm.hand)).name);
				}
				arm.effector = effector;
				arm.reach = std::clamp(value, 0.0f, 1.0f);
			}
		}
	}
	for (const HandReachArm &arm : out.arms) {
		if (arm.effector < 0 || ancestor_of(p_rig, arm.arm, arm.effector)) {
			return core::make_error(where + ": no effector outside the arm for " + p_rig.bone(size_t(arm.hand)).name);
		}
	}
	return out;
}

void reach_hands(const Skeleton &p_rig, const HandReach &p_reach, const std::vector<Xform> &p_held, LocalPose &r_pose,
		std::vector<Xform> &r_model) {
	if (r_pose.joints.size() != p_rig.size() || p_held.size() != p_rig.size() || !p_reach.any(p_rig.size())) {
		return;
	}
	model_pose(p_rig, r_pose, r_model);
	for (const HandReachArm &arm : p_reach.arms) {
		if (arm.usable(p_rig.size()) && !ancestor_of(p_rig, arm.arm, arm.effector)) {
			reach_arm(p_rig, arm, p_held, r_pose, r_model);
		}
	}
}

}
