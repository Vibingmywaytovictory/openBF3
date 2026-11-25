#pragma once

#include "bf3/core/res/havok.h"
#include "bf3/core/res/res_math.h"
#include "bf3/core/res/res_types.h"

#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::res {

inline constexpr std::string_view kRagdollBodyPrefix = "r_";
inline constexpr size_t kMaxRagdollBodies = 64;

enum class RagdollShapeKind : uint8_t { Capsule, Sphere };
enum class RagdollJointKind : uint8_t { Ragdoll, LimitedHinge };

struct RagdollBodyDef {
	std::string name;
	std::string bone;
	Transform rest;
	Vec4 rotation{ 0.0f, 0.0f, 0.0f, 1.0f };
	Vec3 center_of_mass{};
	float mass = 0.0f;
	Vec3 inverse_inertia{};
	float friction = 0.0f;
	float restitution = 0.0f;
	float linear_damping = 0.0f;
	float angular_damping = 0.0f;
	RagdollShapeKind shape = RagdollShapeKind::Capsule;
	Vec3 a{};
	Vec3 b{};
	float radius = 0.0f;
};

struct RagdollJointDef {
	std::string name;
	RagdollJointKind kind = RagdollJointKind::Ragdoll;
	uint32_t child = 0;
	uint32_t parent = 0;
	Transform child_frame;
	Transform parent_frame;
	float twist_min = 0.0f;
	float twist_max = 0.0f;
	float cone = 0.0f;
	float plane_min = 0.0f;
	float plane_max = 0.0f;
	float hinge_min = 0.0f;
	float hinge_max = 0.0f;
	float max_friction_torque = 0.0f;
};

struct RagdollDef {
	std::string packfile_version;
	std::vector<RagdollBodyDef> bodies;
	std::vector<RagdollJointDef> joints;

	static ResError parse(Bytes p_res, const ResMeta &p_res_meta, RagdollDef &r_def);

	int32_t find_body(std::string_view p_name) const;
	float pivot_gap(const RagdollJointDef &p_joint) const;
};

}
