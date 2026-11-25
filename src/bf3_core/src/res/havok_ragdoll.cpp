#include "bf3/core/res/havok_ragdoll.h"

#include "res_reader.h"

#include <bit>
#include <cmath>

namespace bf3::core::res {

using detail::Reader;

namespace {

constexpr uint32_t kVariantBytes = 12;
constexpr uint32_t kVariantObject = 8;
constexpr uint32_t kPhysicsDataSystems = 12;
constexpr uint32_t kSystemBodies = 8;
constexpr uint32_t kSystemConstraints = 20;

constexpr uint32_t kBodyShape = 16;
constexpr uint32_t kBodyName = 120;
constexpr uint32_t kBodyFriction = 144;
constexpr uint32_t kBodyRestitution = 148;
constexpr uint32_t kBodyTransform = 240;
constexpr uint32_t kBodyRotation = 336;
constexpr uint32_t kBodyCenterOfMass = 368;
constexpr uint32_t kBodyLinearDamping = 404;
constexpr uint32_t kBodyAngularDamping = 406;
constexpr uint32_t kBodyInertiaAndMassInv = 416;

constexpr uint32_t kSphereRadius = 16;
constexpr uint32_t kCapsuleRadius = 16;
constexpr uint32_t kCapsuleA = 32;
constexpr uint32_t kCapsuleB = 48;
constexpr uint32_t kTranslateChild = 24;
constexpr uint32_t kTranslateOffset = 32;

constexpr uint32_t kInstanceData = 12;
constexpr uint32_t kInstanceChild = 20;
constexpr uint32_t kInstanceParent = 24;
constexpr uint32_t kInstanceName = 40;

constexpr uint32_t kAtomTransforms = 16;
constexpr uint32_t kTransformA = 32;
constexpr uint32_t kTransformB = 96;

constexpr uint16_t kAtomSetLocalTransforms = 2;
constexpr uint16_t kAtomAngLimit = 14;
constexpr uint16_t kAtomTwistLimit = 15;
constexpr uint16_t kAtomConeLimit = 16;
constexpr uint16_t kAtomAngFriction = 17;

constexpr uint32_t kRagdollFriction = 256;
constexpr uint32_t kRagdollTwist = 268;
constexpr uint32_t kRagdollCone = 288;
constexpr uint32_t kRagdollPlanes = 308;

constexpr uint32_t kHingeFriction = 196;
constexpr uint32_t kHingeLimit = 208;

constexpr float kMaxPivotGap = 1.0e-3f;

float half_value(uint16_t p_bits) {
	return std::bit_cast<float>(uint32_t(p_bits) << 16);
}

class RagdollReader {
public:
	explicit RagdollReader(const Packfile &p_pack) : pack(p_pack), r(Bytes(p_pack.data.data(), p_pack.data.size())) {}

	const Packfile &pack;
	Reader r;

	bool is(uint32_t p_object, std::string_view p_class) const {
		const std::string *c = pack.class_at(p_object);
		return c != nullptr && *c == p_class;
	}

	ResError object_ptr(uint32_t p_slot, std::string_view p_class, uint32_t &r_object) const {
		if (!pack.pointer(p_slot, r_object)) {
			return ResError::malformed("null object pointer", p_slot);
		}
		if (!is(r_object, p_class)) {
			return ResError::malformed("unexpected object class", r_object);
		}
		return ResError();
	}

	Transform transform(uint32_t p_at) {
		Transform t;
		t.right = r.vec3(p_at);
		t.up = r.vec3(p_at + 16);
		t.forward = r.vec3(p_at + 32);
		t.trans = r.vec3(p_at + 48);
		return t;
	}

	ResError atom(uint32_t p_at, uint16_t p_type) {
		const uint16_t type = r.u16(p_at);
		if (!r.ok()) {
			return r.error();
		}
		return type == p_type ? ResError() : ResError::malformed("constraint atom type", p_at);
	}

	ResError body(uint32_t p_at, RagdollBodyDef &r_body);
	ResError joint(uint32_t p_at, const std::vector<uint32_t> &p_bodies, RagdollJointDef &r_joint);
};

ResError RagdollReader::body(uint32_t p_at, RagdollBodyDef &r_body) {
	if (!pack.string(p_at + kBodyName, r_body.name)) {
		return ResError::malformed("rigid body without a name", p_at + kBodyName);
	}
	if (r_body.name.rfind(kRagdollBodyPrefix, 0) != 0) {
		return ResError::malformed("ragdoll body name prefix", p_at + kBodyName);
	}
	r_body.bone = r_body.name.substr(kRagdollBodyPrefix.size());
	r_body.rest = transform(p_at + kBodyTransform);
	r_body.rotation = r.vec4(p_at + kBodyRotation);
	r_body.center_of_mass = r.vec3(p_at + kBodyCenterOfMass);
	r_body.friction = r.f32(p_at + kBodyFriction);
	r_body.restitution = r.f32(p_at + kBodyRestitution);
	r_body.linear_damping = half_value(r.u16(p_at + kBodyLinearDamping));
	r_body.angular_damping = half_value(r.u16(p_at + kBodyAngularDamping));
	const Vec4 inv = r.vec4(p_at + kBodyInertiaAndMassInv);
	if (!r.ok()) {
		return r.error();
	}
	if (!(inv[3] > 0.0f) || !std::isfinite(inv[3])) {
		return ResError::malformed("rigid body inverse mass", p_at + kBodyInertiaAndMassInv);
	}
	r_body.mass = 1.0f / inv[3];
	r_body.inverse_inertia = Vec3{ inv[0], inv[1], inv[2] };
	uint32_t shape = 0;
	if (!pack.pointer(p_at + kBodyShape, shape)) {
		return ResError::malformed("rigid body without a shape", p_at + kBodyShape);
	}
	if (is(shape, "hkpCapsuleShape")) {
		r_body.shape = RagdollShapeKind::Capsule;
		r_body.radius = r.f32(shape + kCapsuleRadius);
		r_body.a = r.vec3(shape + kCapsuleA);
		r_body.b = r.vec3(shape + kCapsuleB);
	} else if (is(shape, "hkpConvexTranslateShape")) {
		uint32_t sphere = 0;
		const ResError e = object_ptr(shape + kTranslateChild, "hkpSphereShape", sphere);
		if (!e.ok()) {
			return e;
		}
		r_body.shape = RagdollShapeKind::Sphere;
		r_body.radius = r.f32(sphere + kSphereRadius);
		r_body.a = r.vec3(shape + kTranslateOffset);
		r_body.b = r_body.a;
	} else {
		return ResError::unsupported("ragdoll body shape", shape);
	}
	if (!r.ok()) {
		return r.error();
	}
	if (!(r_body.radius > 0.0f) || !std::isfinite(r_body.radius)) {
		return ResError::malformed("ragdoll shape radius", shape);
	}
	return ResError();
}

ResError RagdollReader::joint(uint32_t p_at, const std::vector<uint32_t> &p_bodies, RagdollJointDef &r_joint) {
	uint32_t data = 0, child = 0, parent = 0;
	if (!pack.pointer(p_at + kInstanceData, data) || !pack.pointer(p_at + kInstanceChild, child) ||
			!pack.pointer(p_at + kInstanceParent, parent)) {
		return ResError::malformed("constraint instance pointers", p_at);
	}
	pack.string(p_at + kInstanceName, r_joint.name);
	auto index_of = [&](uint32_t p_body, uint32_t &r_index) {
		for (uint32_t i = 0; i < p_bodies.size(); ++i) {
			if (p_bodies[i] == p_body) {
				r_index = i;
				return true;
			}
		}
		return false;
	};
	if (!index_of(child, r_joint.child) || !index_of(parent, r_joint.parent) || r_joint.child == r_joint.parent) {
		return ResError::malformed("constraint bodies", p_at);
	}
	ResError e = atom(data + kAtomTransforms, kAtomSetLocalTransforms);
	if (!e.ok()) {
		return e;
	}
	r_joint.child_frame = transform(data + kTransformA);
	r_joint.parent_frame = transform(data + kTransformB);
	if (is(data, "hkpRagdollConstraintData")) {
		r_joint.kind = RagdollJointKind::Ragdoll;
		for (const auto &[at, type] :
				{ std::pair{ kRagdollFriction, kAtomAngFriction }, std::pair{ kRagdollTwist, kAtomTwistLimit },
						std::pair{ kRagdollCone, kAtomConeLimit }, std::pair{ kRagdollPlanes, kAtomConeLimit } }) {
			if (e = atom(data + at, type); !e.ok()) {
				return e;
			}
		}
		r_joint.max_friction_torque = r.f32(data + kRagdollFriction + 8);
		r_joint.twist_min = r.f32(data + kRagdollTwist + 8);
		r_joint.twist_max = r.f32(data + kRagdollTwist + 12);
		r_joint.cone = r.f32(data + kRagdollCone + 12);
		r_joint.plane_min = r.f32(data + kRagdollPlanes + 8);
		r_joint.plane_max = r.f32(data + kRagdollPlanes + 12);
	} else if (is(data, "hkpLimitedHingeConstraintData")) {
		r_joint.kind = RagdollJointKind::LimitedHinge;
		for (const auto &[at, type] :
				{ std::pair{ kHingeFriction, kAtomAngFriction }, std::pair{ kHingeLimit, kAtomAngLimit } }) {
			if (e = atom(data + at, type); !e.ok()) {
				return e;
			}
		}
		r_joint.max_friction_torque = r.f32(data + kHingeFriction + 8);
		r_joint.hinge_min = r.f32(data + kHingeLimit + 4);
		r_joint.hinge_max = r.f32(data + kHingeLimit + 8);
	} else {
		return ResError::unsupported("ragdoll constraint data", data);
	}
	return r.error();
}

}

int32_t RagdollDef::find_body(std::string_view p_name) const {
	for (size_t i = 0; i < bodies.size(); ++i) {
		if (bodies[i].bone == p_name || bodies[i].name == p_name) {
			return int32_t(i);
		}
	}
	return -1;
}

float RagdollDef::pivot_gap(const RagdollJointDef &p_joint) const {
	const Vec3 a = bodies[p_joint.child].rest.transform_point(p_joint.child_frame.trans);
	const Vec3 b = bodies[p_joint.parent].rest.transform_point(p_joint.parent_frame.trans);
	return vec::length(vec::sub(a, b));
}

ResError RagdollDef::parse(Bytes p_res, const ResMeta &p_res_meta, RagdollDef &r_def) {
	r_def = RagdollDef();
	const uint32_t packfile32_len = uint32_t(p_res_meta[0]) | (uint32_t(p_res_meta[1]) << 8) |
			(uint32_t(p_res_meta[2]) << 16) | (uint32_t(p_res_meta[3]) << 24);
	const uint32_t packfile64_len = uint32_t(p_res_meta[4]) | (uint32_t(p_res_meta[5]) << 8) |
			(uint32_t(p_res_meta[6]) << 16) | (uint32_t(p_res_meta[7]) << 24);
	if (uint64_t(packfile32_len) + uint64_t(packfile64_len) != p_res.size()) {
		return ResError::malformed("ragdoll resource length", p_res.size());
	}
	Packfile pack;
	ResError e = Packfile::parse_inline(p_res.subspan(0, packfile32_len), pack);
	if (!e.ok()) {
		return e;
	}
	r_def.packfile_version = pack.version;
	RagdollReader rd(pack);
	uint32_t variants = 0, count = 0;
	if (e = pack.array(pack.contents_offset, variants, count); !e.ok()) {
		return e;
	}
	uint32_t physics = 0;
	bool found = false;
	for (uint32_t i = 0; i < count && !found; ++i) {
		uint32_t object = 0;
		if (pack.pointer(variants + kVariantBytes * i + kVariantObject, object) && rd.is(object, "hkpPhysicsData")) {
			physics = object;
			found = true;
		}
	}
	if (!found) {
		return ResError::malformed("ragdoll packfile without physics data", pack.contents_offset);
	}
	uint32_t systems = 0;
	if (e = pack.array(physics + kPhysicsDataSystems, systems, count); !e.ok()) {
		return e;
	}
	if (count != 1) {
		return ResError::unsupported("ragdoll physics systems", count);
	}
	uint32_t system = 0;
	if (e = rd.object_ptr(systems, "hkpPhysicsSystem", system); !e.ok()) {
		return e;
	}
	uint32_t body_list = 0, body_count = 0, joint_list = 0, joint_count = 0;
	if (e = pack.array(system + kSystemBodies, body_list, body_count); !e.ok()) {
		return e;
	}
	if (e = pack.array(system + kSystemConstraints, joint_list, joint_count); !e.ok()) {
		return e;
	}
	if (body_count == 0 || body_count > kMaxRagdollBodies || joint_count >= body_count + 1) {
		return ResError::unsupported("ragdoll body count", body_count);
	}
	std::vector<uint32_t> body_objects;
	for (uint32_t i = 0; i < body_count; ++i) {
		uint32_t b = 0;
		if (e = rd.object_ptr(body_list + 4 * i, "hkpRigidBody", b); !e.ok()) {
			return e;
		}
		body_objects.push_back(b);
		RagdollBodyDef body;
		if (e = rd.body(b, body); !e.ok()) {
			return e;
		}
		r_def.bodies.push_back(std::move(body));
	}
	for (uint32_t i = 0; i < joint_count; ++i) {
		uint32_t c = 0;
		if (e = rd.object_ptr(joint_list + 4 * i, "hkpConstraintInstance", c); !e.ok()) {
			return e;
		}
		RagdollJointDef joint;
		if (e = rd.joint(c, body_objects, joint); !e.ok()) {
			return e;
		}
		if (!(r_def.pivot_gap(joint) <= kMaxPivotGap)) {
			return ResError::malformed("ragdoll joint pivots apart", c);
		}
		r_def.joints.push_back(std::move(joint));
	}
	return ResError();
}

}
