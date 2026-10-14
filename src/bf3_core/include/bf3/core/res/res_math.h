#pragma once
// Affine transforms in Frostbite convention (handoff §13): the rows of `axis` are the images of the
// local X, Y and Z axes (right, up, forward), `p' = origin + x*axis[0] + y*axis[1] + z*axis[2]`.
// Godot: Transform3D(Basis(axis[0], axis[1], axis[2]) as columns, origin), no permutation.
// Operation order follows IW4L math.rs exactly, so results are bit-identical to it.

#include "bf3/core/res/res_types.h"

namespace bf3::core::res {

struct Transform {
	std::array<Vec3, 3> axis{ { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } } };
	Vec3 origin{ 0.0f, 0.0f, 0.0f };

	static Transform identity() { return Transform(); }
	static Transform from_translation(const Vec3 &p_origin);
	// p_rotation is a quaternion (x, y, z, w).
	static Transform from_rotation_translation(const Vec4 &p_rotation, const Vec3 &p_origin);
	// p' = t + R(s * p): per-axis scale first, then rotation, then translation (hkQsTransform).
	static Transform from_translation_rotation_scale(const Vec3 &p_translation, const Vec4 &p_rotation, const Vec3 &p_scale);

	Vec3 transform_vector(const Vec3 &p_v) const;
	Vec3 transform_point(const Vec3 &p_p) const;
	// self * inner: a point goes through p_inner first.
	Transform then_inner(const Transform &p_inner) const;
	float determinant() const;
	// Plane n.p + w = 0 in local space to the same plane in parent space, unit normal, side kept.
	Vec4 transform_plane(const Vec4 &p_plane) const;

	friend bool operator==(const Transform &, const Transform &) = default;
};

namespace vec {
float dot(const Vec3 &p_a, const Vec3 &p_b);
Vec3 cross(const Vec3 &p_a, const Vec3 &p_b);
Vec3 sub(const Vec3 &p_a, const Vec3 &p_b);
Vec3 add(const Vec3 &p_a, const Vec3 &p_b);
Vec3 scale(const Vec3 &p_a, float p_s);
float length(const Vec3 &p_a);
Vec3 normalize(const Vec3 &p_a);
} // namespace vec

} // namespace bf3::core::res
