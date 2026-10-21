#include "bf3/anim/pose_math.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace bf3::anim {

Quat quat_mul(const Quat &a, const Quat &b) {
	return Quat{
		a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
		a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
		a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
	};
}

Quat quat_conj(const Quat &p_q) {
	return Quat{ -p_q.x, -p_q.y, -p_q.z, p_q.w };
}

Quat quat_normalized(const Quat &p_q) {
	const double n = std::sqrt(double(p_q.x) * p_q.x + double(p_q.y) * p_q.y + double(p_q.z) * p_q.z + double(p_q.w) * p_q.w);
	if (!(n > 0.0)) {
		return Quat{};
	}
	return Quat{ float(p_q.x / n), float(p_q.y / n), float(p_q.z / n), float(p_q.w / n) };
}

Vec3 quat_rotate(const Quat &q, const Vec3 &v) {

	const float tx = 2.0f * (q.y * v.z - q.z * v.y);
	const float ty = 2.0f * (q.z * v.x - q.x * v.z);
	const float tz = 2.0f * (q.x * v.y - q.y * v.x);
	return Vec3{
		v.x + q.w * tx + (q.y * tz - q.z * ty),
		v.y + q.w * ty + (q.z * tx - q.x * tz),
		v.z + q.w * tz + (q.x * ty - q.y * tx),
	};
}

double quat_angle_deg(const Quat &p_a, const Quat &p_b) {
	const double d = std::fabs(double(p_a.x) * p_b.x + double(p_a.y) * p_b.y + double(p_a.z) * p_b.z + double(p_a.w) * p_b.w);
	const double na = std::sqrt(double(p_a.x) * p_a.x + double(p_a.y) * p_a.y + double(p_a.z) * p_a.z + double(p_a.w) * p_a.w);
	const double nb = std::sqrt(double(p_b.x) * p_b.x + double(p_b.y) * p_b.y + double(p_b.z) * p_b.z + double(p_b.w) * p_b.w);
	if (!(na > 0.0) || !(nb > 0.0)) {
		return 180.0;
	}
	return 2.0 * std::acos(std::min(1.0, d / (na * nb))) * 180.0 / std::numbers::pi;
}

Quat quat_from_mat(const Mat34 &p_m) {

	double c[3][3];
	for (int col = 0; col < 3; ++col) {
		const double len = std::sqrt(double(p_m.m[0][col]) * p_m.m[0][col] + double(p_m.m[1][col]) * p_m.m[1][col] + double(p_m.m[2][col]) * p_m.m[2][col]);
		for (int row = 0; row < 3; ++row) {
			c[row][col] = len > 0.0 ? p_m.m[row][col] / len : (row == col ? 1.0 : 0.0);
		}
	}
	const double trace = c[0][0] + c[1][1] + c[2][2];
	double x, y, z, w;
	if (trace > 0.0) {
		const double s = std::sqrt(trace + 1.0) * 2.0;
		w = 0.25 * s;
		x = (c[2][1] - c[1][2]) / s;
		y = (c[0][2] - c[2][0]) / s;
		z = (c[1][0] - c[0][1]) / s;
	} else if (c[0][0] > c[1][1] && c[0][0] > c[2][2]) {
		const double s = std::sqrt(1.0 + c[0][0] - c[1][1] - c[2][2]) * 2.0;
		w = (c[2][1] - c[1][2]) / s;
		x = 0.25 * s;
		y = (c[0][1] + c[1][0]) / s;
		z = (c[0][2] + c[2][0]) / s;
	} else if (c[1][1] > c[2][2]) {
		const double s = std::sqrt(1.0 + c[1][1] - c[0][0] - c[2][2]) * 2.0;
		w = (c[0][2] - c[2][0]) / s;
		x = (c[0][1] + c[1][0]) / s;
		y = 0.25 * s;
		z = (c[1][2] + c[2][1]) / s;
	} else {
		const double s = std::sqrt(1.0 + c[2][2] - c[0][0] - c[1][1]) * 2.0;
		w = (c[1][0] - c[0][1]) / s;
		x = (c[0][2] + c[2][0]) / s;
		y = (c[1][2] + c[2][1]) / s;
		z = 0.25 * s;
	}
	return quat_normalized(Quat{ float(x), float(y), float(z), float(w) });
}

Vec3 vec_add(const Vec3 &a, const Vec3 &b) {
	return Vec3{ a.x + b.x, a.y + b.y, a.z + b.z };
}

Vec3 vec_sub(const Vec3 &a, const Vec3 &b) {
	return Vec3{ a.x - b.x, a.y - b.y, a.z - b.z };
}

Vec3 vec_scale(const Vec3 &a, float s) {
	return Vec3{ a.x * s, a.y * s, a.z * s };
}

double vec_dist(const Vec3 &a, const Vec3 &b) {
	const double dx = double(a.x) - b.x, dy = double(a.y) - b.y, dz = double(a.z) - b.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

Xform xform_identity() {
	return Xform{};
}

Xform xform_mul(const Xform &p_a, const Xform &p_b) {
	return Xform{ quat_normalized(quat_mul(p_a.q, p_b.q)), vec_add(quat_rotate(p_a.q, p_b.t), p_a.t) };
}

Xform xform_inverse(const Xform &p_x) {
	const Quat inv = quat_conj(p_x.q);
	return Xform{ inv, vec_scale(quat_rotate(inv, p_x.t), -1.0f) };
}

Vec3 xform_point(const Xform &p_x, const Vec3 &p_p) {
	return vec_add(quat_rotate(p_x.q, p_p), p_x.t);
}

Mat34 mat_from_xform(const Xform &p_x) {
	const Quat q = quat_normalized(p_x.q);
	const Vec3 cx = quat_rotate(q, Vec3{ 1.0f, 0.0f, 0.0f });
	const Vec3 cy = quat_rotate(q, Vec3{ 0.0f, 1.0f, 0.0f });
	const Vec3 cz = quat_rotate(q, Vec3{ 0.0f, 0.0f, 1.0f });
	Mat34 r;
	r.m[0] = { cx.x, cy.x, cz.x, p_x.t.x };
	r.m[1] = { cx.y, cy.y, cz.y, p_x.t.y };
	r.m[2] = { cx.z, cy.z, cz.z, p_x.t.z };
	return r;
}

Mat34 mat_from_linear_transform(const std::array<core::Vec3, 4> &p_rows) {
	Mat34 r;
	for (int row = 0; row < 3; ++row) {
		r.m[row] = { p_rows[0][row], p_rows[1][row], p_rows[2][row], p_rows[3][row] };
	}
	return r;
}

Mat34 mat_mul(const Mat34 &a, const Mat34 &b) {
	Mat34 r;
	for (int row = 0; row < 3; ++row) {
		for (int col = 0; col < 4; ++col) {
			float v = a.m[row][0] * b.m[0][col] + a.m[row][1] * b.m[1][col] + a.m[row][2] * b.m[2][col];
			if (col == 3) {
				v += a.m[row][3];
			}
			r.m[row][col] = v;
		}
	}
	return r;
}

bool mat_inverse(const Mat34 &p_m, Mat34 &r_inverse) {
	const auto &m = p_m.m;
	const double a00 = m[0][0], a01 = m[0][1], a02 = m[0][2];
	const double a10 = m[1][0], a11 = m[1][1], a12 = m[1][2];
	const double a20 = m[2][0], a21 = m[2][1], a22 = m[2][2];
	const double c00 = a11 * a22 - a12 * a21, c01 = a02 * a21 - a01 * a22, c02 = a01 * a12 - a02 * a11;
	const double c10 = a12 * a20 - a10 * a22, c11 = a00 * a22 - a02 * a20, c12 = a02 * a10 - a00 * a12;
	const double c20 = a10 * a21 - a11 * a20, c21 = a01 * a20 - a00 * a21, c22 = a00 * a11 - a01 * a10;
	const double det = a00 * c00 + a01 * c10 + a02 * c20;
	if (!(std::fabs(det) > 1e-12)) {
		r_inverse = Mat34{};
		return false;
	}
	const double inv = 1.0 / det;
	const double r3[3][3] = { { c00 * inv, c01 * inv, c02 * inv }, { c10 * inv, c11 * inv, c12 * inv }, { c20 * inv, c21 * inv, c22 * inv } };
	for (int row = 0; row < 3; ++row) {
		const double t = -(r3[row][0] * m[0][3] + r3[row][1] * m[1][3] + r3[row][2] * m[2][3]);
		r_inverse.m[row] = { float(r3[row][0]), float(r3[row][1]), float(r3[row][2]), float(t) };
	}
	return true;
}

Vec3 mat_point(const Mat34 &p_m, const Vec3 &p) {
	const auto &m = p_m.m;
	return Vec3{ m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3], m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
		m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3] };
}

Vec3 mat_vector(const Mat34 &p_m, const Vec3 &v) {
	const auto &m = p_m.m;
	return Vec3{ m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z, m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z };
}

float mat_max_diff(const Mat34 &p_a, const Mat34 &p_b) {
	float d = 0.0f;
	for (int row = 0; row < 3; ++row) {
		for (int col = 0; col < 4; ++col) {
			d = std::max(d, std::fabs(p_a.m[row][col] - p_b.m[row][col]));
		}
	}
	return d;
}

}
