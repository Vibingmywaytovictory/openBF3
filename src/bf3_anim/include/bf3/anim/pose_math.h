#pragma once
// Small rigid-transform math for poses (plan §2.2 bf3_anim, §3.3). Frostbite space as stored: metres, Y up,
// quaternions x, y, z, w; a joint transform maps joint space to its parent's space, p' = q * p + t.
// Mat34 is the affine form [R | t] for column vectors (rows of R are x, y, z of the images of the axes);
// a Frostbite LinearTransform (rows right, up, forward, trans) becomes the columns right, up, forward and
// the translation trans, so Godot's Transform3D(Basis(columns), origin) takes the same numbers.

#include "bf3/core/ant/ant_clip.h"

#include <array>

namespace bf3::anim {

using Quat = core::ant::Quat;
using Vec3 = core::ant::Vec3;

// Rigid transform (rotation, then translation).
struct Xform {
	Quat q;
	Vec3 t;
};

// Affine 3x4, column-vector convention: p' = m * [p, 1].
struct Mat34 {
	std::array<std::array<float, 4>, 3> m{ { { 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f } } };

	Vec3 column(int p_axis) const { return { m[0][p_axis], m[1][p_axis], m[2][p_axis] }; }
	Vec3 origin() const { return { m[0][3], m[1][3], m[2][3] }; }
};

Quat quat_mul(const Quat &p_a, const Quat &p_b); // p_a * p_b: rotate by p_b first
Quat quat_conj(const Quat &p_q);
Quat quat_normalized(const Quat &p_q); // identity for a zero quaternion
Vec3 quat_rotate(const Quat &p_q, const Vec3 &p_v);
// Angle of the rotation from p_a to p_b in degrees (sign-insensitive: q and -q are the same rotation).
double quat_angle_deg(const Quat &p_a, const Quat &p_b);
// Rotation part of an orthonormal 3x3 (scale is divided out per column first).
Quat quat_from_mat(const Mat34 &p_m);

Vec3 vec_add(const Vec3 &p_a, const Vec3 &p_b);
Vec3 vec_sub(const Vec3 &p_a, const Vec3 &p_b);
Vec3 vec_scale(const Vec3 &p_a, float p_s);
double vec_dist(const Vec3 &p_a, const Vec3 &p_b);

Xform xform_identity();
// p_a * p_b: apply p_b, then p_a (parent * local = model space of the child).
Xform xform_mul(const Xform &p_a, const Xform &p_b);
Xform xform_inverse(const Xform &p_x);
Vec3 xform_point(const Xform &p_x, const Vec3 &p_p);

Mat34 mat_from_xform(const Xform &p_x);
// A Frostbite LinearTransform: rows right, up, forward, trans (EbxStruct::transform()).
Mat34 mat_from_linear_transform(const std::array<std::array<float, 3>, 4> &p_rows);
Mat34 mat_mul(const Mat34 &p_a, const Mat34 &p_b);
// General affine inverse; returns false (and identity) for a singular matrix.
bool mat_inverse(const Mat34 &p_m, Mat34 &r_inverse);
Vec3 mat_point(const Mat34 &p_m, const Vec3 &p_p);
Vec3 mat_vector(const Mat34 &p_m, const Vec3 &p_v);
// Largest absolute difference of the 12 entries.
float mat_max_diff(const Mat34 &p_a, const Mat34 &p_b);

} // namespace bf3::anim
