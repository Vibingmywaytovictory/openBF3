#include "bf3/core/math.h"

#include <cmath>

namespace bf3::core {

namespace vec {
float dot(const Vec3 &p_a, const Vec3 &p_b) {
	return p_a[0] * p_b[0] + p_a[1] * p_b[1] + p_a[2] * p_b[2];
}
Vec3 cross(const Vec3 &p_a, const Vec3 &p_b) {
	return { p_a[1] * p_b[2] - p_a[2] * p_b[1], p_a[2] * p_b[0] - p_a[0] * p_b[2], p_a[0] * p_b[1] - p_a[1] * p_b[0] };
}
Vec3 sub(const Vec3 &p_a, const Vec3 &p_b) {
	return { p_a[0] - p_b[0], p_a[1] - p_b[1], p_a[2] - p_b[2] };
}
Vec3 add(const Vec3 &p_a, const Vec3 &p_b) {
	return { p_a[0] + p_b[0], p_a[1] + p_b[1], p_a[2] + p_b[2] };
}
Vec3 scale(const Vec3 &p_a, float p_s) {
	return { p_a[0] * p_s, p_a[1] * p_s, p_a[2] * p_s };
}
float length(const Vec3 &p_a) {
	return std::sqrt(dot(p_a, p_a));
}
float horizontal_distance(const Vec3 &p_a, const Vec3 &p_b) {
	const float dx = p_a[0] - p_b[0];
	const float dz = p_a[2] - p_b[2];
	return std::sqrt(dx * dx + dz * dz);
}

Vec3 normalize(const Vec3 &p_a) {
	const float len = length(p_a);
	return len > 0.0f ? scale(p_a, 1.0f / len) : p_a;
}
}

Transform Transform::from_axes(const std::array<Vec3, 3> &p_axes, const Vec3 &p_trans) {
	return { p_axes[0], p_axes[1], p_axes[2], p_trans };
}

Transform Transform::from_translation(const Vec3 &p_trans) {
	Transform t;
	t.trans = p_trans;
	return t;
}

Transform Transform::from_rotation_translation(const std::array<float, 4> &p_rotation, const Vec3 &p_trans) {
	const float x = p_rotation[0], y = p_rotation[1], z = p_rotation[2], w = p_rotation[3];
	Transform t;
	t.right = { 1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y + z * w), 2.0f * (x * z - y * w) };
	t.up = { 2.0f * (x * y - z * w), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z + x * w) };
	t.forward = { 2.0f * (x * z + y * w), 2.0f * (y * z - x * w), 1.0f - 2.0f * (x * x + y * y) };
	t.trans = p_trans;
	return t;
}

Transform Transform::from_translation_rotation_scale(
		const Vec3 &p_translation, const std::array<float, 4> &p_rotation, const Vec3 &p_scale) {
	Transform t = from_rotation_translation(p_rotation, p_translation);
	for (size_t row = 0; row < 3; row++) {
		for (float &component : t.axis(row)) {
			component *= p_scale[row];
		}
	}
	return t;
}

Vec3 Transform::transform_vector(const Vec3 &p_v) const {
	Vec3 out{ 0.0f, 0.0f, 0.0f };
	for (size_t row = 0; row < 3; row++) {
		for (size_t a = 0; a < 3; a++) {
			out[a] += axis(row)[a] * p_v[row];
		}
	}
	return out;
}

Vec3 Transform::transform_point(const Vec3 &p_p) const {
	const Vec3 v = transform_vector(p_p);
	return { v[0] + trans[0], v[1] + trans[1], v[2] + trans[2] };
}

Transform Transform::then_inner(const Transform &p_inner) const {
	return { transform_vector(p_inner.right), transform_vector(p_inner.up), transform_vector(p_inner.forward),
		transform_point(p_inner.trans) };
}

float Transform::determinant() const {
	return vec::dot(right, vec::cross(up, forward));
}

std::array<float, 4> Transform::transform_plane(const std::array<float, 4> &p_plane) const {
	const Vec3 n{ p_plane[0], p_plane[1], p_plane[2] };
	const float len2 = vec::dot(n, n);
	if (len2 <= 0.0f) {
		return p_plane;
	}
	const Vec3 point = transform_point(vec::scale(n, -p_plane[3] / len2));
	const std::array<Vec3, 3> cofactor{ vec::cross(up, forward), vec::cross(forward, right), vec::cross(right, up) };
	Vec3 normal{ 0.0f, 0.0f, 0.0f };
	for (size_t row = 0; row < 3; row++) {
		for (size_t a = 0; a < 3; a++) {
			normal[a] += cofactor[row][a] * n[row];
		}
	}
	if (determinant() < 0.0f) {
		normal = vec::scale(normal, -1.0f);
	}
	normal = vec::normalize(normal);
	return { normal[0], normal[1], normal[2], -vec::dot(normal, point) };
}

}
