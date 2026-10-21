#pragma once

#include "bf3/core/ant/ant_clip.h"
#include "bf3/core/math.h"

#include <array>

namespace bf3::anim {

using Quat = core::ant::Quat;
using Vec3 = core::ant::Vec3;

struct Xform {
	Quat q;
	Vec3 t;
};

struct Mat34 {
	std::array<std::array<float, 4>, 3> m{ { { 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f } } };

	Vec3 column(int p_axis) const { return { m[0][p_axis], m[1][p_axis], m[2][p_axis] }; }
	Vec3 origin() const { return { m[0][3], m[1][3], m[2][3] }; }
};

Quat quat_mul(const Quat &p_a, const Quat &p_b);
Quat quat_conj(const Quat &p_q);
Quat quat_normalized(const Quat &p_q);
Vec3 quat_rotate(const Quat &p_q, const Vec3 &p_v);

double quat_angle_deg(const Quat &p_a, const Quat &p_b);

Quat quat_from_mat(const Mat34 &p_m);

Vec3 vec_add(const Vec3 &p_a, const Vec3 &p_b);
Vec3 vec_sub(const Vec3 &p_a, const Vec3 &p_b);
Vec3 vec_scale(const Vec3 &p_a, float p_s);
double vec_dist(const Vec3 &p_a, const Vec3 &p_b);

Xform xform_identity();

Xform xform_mul(const Xform &p_a, const Xform &p_b);
Xform xform_inverse(const Xform &p_x);
Vec3 xform_point(const Xform &p_x, const Vec3 &p_p);

Mat34 mat_from_xform(const Xform &p_x);

Mat34 mat_from_linear_transform(const std::array<core::Vec3, 4> &p_rows);
Mat34 mat_mul(const Mat34 &p_a, const Mat34 &p_b);

bool mat_inverse(const Mat34 &p_m, Mat34 &r_inverse);
Vec3 mat_point(const Mat34 &p_m, const Vec3 &p_p);
Vec3 mat_vector(const Mat34 &p_m, const Vec3 &p_v);

float mat_max_diff(const Mat34 &p_a, const Mat34 &p_b);

}
