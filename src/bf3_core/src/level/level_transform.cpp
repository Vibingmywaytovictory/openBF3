// Level transforms: port of IW4L level_bf3 transform.rs (Apache-2.0), operation for operation.
#include "bf3/core/level/level_transform.h"

#include <cmath>

namespace bf3::core::level {

namespace {

float length3(const Vec3 &p_v) {
	return std::sqrt(p_v[0] * p_v[0] + p_v[1] * p_v[1] + p_v[2] * p_v[2]);
}

} // namespace

LevelTransform LevelTransform::from_rows(const Rows &p_rows) {
	LevelTransform t;
	t.right = p_rows[0];
	t.up = p_rows[1];
	t.forward = p_rows[2];
	t.trans = p_rows[3];
	t.scale = 1.0f;
	return t;
}

LevelTransform LevelTransform::from_res(const res::Transform &p_transform) {
	LevelTransform t;
	t.right = p_transform.axis[0];
	t.up = p_transform.axis[1];
	t.forward = p_transform.axis[2];
	t.trans = p_transform.origin;
	t.scale = 1.0f;
	return t;
}

Vec3 LevelTransform::rotate(const Vec3 &p_v) const {
	// out starts at 0 and accumulates row by row, as the Rust loop does (0 + x keeps the sign of
	// zero out of the result exactly like it).
	Vec3 out{ 0.0f, 0.0f, 0.0f };
	const Vec3 *rows[3] = { &right, &up, &forward };
	for (int r = 0; r < 3; ++r) {
		for (int axis = 0; axis < 3; ++axis) {
			out[axis] += (*rows[r])[axis] * p_v[r];
		}
	}
	return out;
}

Vec3 LevelTransform::transform_vector(const Vec3 &p_v) const {
	Vec3 out = rotate(p_v);
	for (float &component : out) {
		component *= scale;
	}
	return out;
}

Vec3 LevelTransform::transform_point(const Vec3 &p_p) const {
	const Vec3 v = transform_vector(p_p);
	return { v[0] + trans[0], v[1] + trans[1], v[2] + trans[2] };
}

LevelTransform LevelTransform::in_parent(const LevelTransform &p_parent) const {
	LevelTransform t;
	t.right = p_parent.rotate(right);
	t.up = p_parent.rotate(up);
	t.forward = p_parent.rotate(forward);
	t.trans = p_parent.transform_point(trans);
	t.scale = scale * p_parent.scale;
	return t;
}

float LevelTransform::determinant() const {
	const Vec3 &r = right;
	const Vec3 &u = up;
	const Vec3 &f = forward;
	return r[0] * (u[1] * f[2] - u[2] * f[1]) - r[1] * (u[0] * f[2] - u[2] * f[0]) + r[2] * (u[0] * f[1] - u[1] * f[0]);
}

Vec3 LevelTransform::axis_scales() const {
	return { length3(right) * scale, length3(up) * scale, length3(forward) * scale };
}

bool LevelTransform::is_uniform(float p_tolerance) const {
	const Vec3 s = axis_scales();
	// fmax/fmin: Rust's f32::max/min ignore a NaN operand the same way.
	const float largest = std::fmax(std::fmax(s[0], s[1]), s[2]);
	return largest - std::fmin(std::fmin(s[0], s[1]), s[2]) <= p_tolerance * largest;
}

LevelTransform LevelTransform::normalized() const {
	const float scales[3] = { length3(right), length3(up), length3(forward) };
	const auto unit = [](const Vec3 &p_axis, float p_scale) -> Vec3 {
		if (p_scale > 0.0f) {
			return { p_axis[0] / p_scale, p_axis[1] / p_scale, p_axis[2] / p_scale };
		}
		return p_axis;
	};
	LevelTransform t;
	t.right = unit(right, scales[0]);
	t.up = unit(up, scales[1]);
	t.forward = unit(forward, scales[2]);
	t.trans = trans;
	t.scale = scale * (scales[0] + scales[1] + scales[2]) / 3.0f;
	return t;
}

} // namespace bf3::core::level
