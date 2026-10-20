#include "bf3/core/res/res_math.h"
#include "bf3/core/res/res_types.h"
#include "res_reader.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace bf3::core::res {

bool ChunkId::is_zero() const {
	for (uint8_t b : bytes) {
		if (b != 0) {
			return false;
		}
	}
	return true;
}

std::string ChunkId::to_string() const {
	const auto &b = bytes;
	char text[40];
	std::snprintf(text, sizeof(text), "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			unsigned(b[0] | (b[1] << 8) | (b[2] << 16) | (unsigned(b[3]) << 24)), unsigned(b[4] | (b[5] << 8)),
			unsigned(b[6] | (b[7] << 8)), b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
	return text;
}

std::string ChunkId::to_hex() const {
	static const char *digits = "0123456789abcdef";
	std::string out;
	out.reserve(32);
	for (uint8_t b : bytes) {
		out.push_back(digits[b >> 4]);
		out.push_back(digits[b & 15]);
	}
	return out;
}

std::string ResError::message() const {
	char text[256];
	switch (kind) {
		case Kind::None:
			return std::string();
		case Kind::Truncated:
			std::snprintf(text, sizeof(text), "needed %llu bytes at %llu of a %llu-byte payload", (unsigned long long)needed,
					(unsigned long long)at, (unsigned long long)len);
			break;
		case Kind::Malformed:
			std::snprintf(text, sizeof(text), "malformed %s at %llu", what, (unsigned long long)at);
			break;
		case Kind::Unsupported:
			std::snprintf(text, sizeof(text), "unsupported %s 0x%x", what, value);
			break;
	}
	return text;
}

ResError ResError::truncated(uint64_t p_at, uint64_t p_needed, uint64_t p_len) {
	ResError e;
	e.kind = Kind::Truncated;
	e.at = p_at;
	e.needed = p_needed;
	e.len = p_len;
	return e;
}

ResError ResError::malformed(const char *p_what, uint64_t p_at) {
	ResError e;
	e.kind = Kind::Malformed;
	e.what = p_what;
	e.at = p_at;
	return e;
}

ResError ResError::unsupported(const char *p_what, uint32_t p_value) {
	ResError e;
	e.kind = Kind::Unsupported;
	e.what = p_what;
	e.value = p_value;
	return e;
}

uint32_t hash_quick(std::string_view p_text) {
	uint32_t h = 5381;
	for (char c : p_text) {
		h = (h * 33u) ^ uint32_t(uint8_t(c));
	}
	return h;
}

float half_to_f32(uint16_t p_bits) {
	const float sign = (p_bits & 0x8000) != 0 ? -1.0f : 1.0f;
	const int exponent = (p_bits >> 10) & 0x1F;
	const float mantissa = float(p_bits & 0x3FF);
	switch (exponent) {
		case 0:
			return sign * mantissa * (1.0f / 16777216.0f);
		case 31:
			return mantissa == 0.0f ? sign * std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
		default:
			return sign * (1.0f + mantissa / 1024.0f) * std::ldexp(1.0f, exponent - 15);
	}
}

namespace detail {

bool valid_utf8(const uint8_t *p_data, size_t p_len) {
	size_t i = 0;
	while (i < p_len) {
		const uint8_t c = p_data[i];
		if (c < 0x80) {
			i++;
			continue;
		}
		size_t n;
		uint8_t lo = 0x80, hi = 0xBF;
		if (c >= 0xC2 && c <= 0xDF) {
			n = 1;
		} else if (c >= 0xE0 && c <= 0xEF) {
			n = 2;
			if (c == 0xE0) {
				lo = 0xA0;
			} else if (c == 0xED) {
				hi = 0x9F;
			}
		} else if (c >= 0xF0 && c <= 0xF4) {
			n = 3;
			if (c == 0xF0) {
				lo = 0x90;
			} else if (c == 0xF4) {
				hi = 0x8F;
			}
		} else {
			return false;
		}
		if (p_len - i - 1 < n) {
			return false;
		}
		if (p_data[i + 1] < lo || p_data[i + 1] > hi) {
			return false;
		}
		for (size_t k = 2; k <= n; k++) {
			if ((p_data[i + k] & 0xC0) != 0x80) {
				return false;
			}
		}
		i += n + 1;
	}
	return true;
}

}

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
Vec3 normalize(const Vec3 &p_a) {
	const float len = length(p_a);
	return len > 0.0f ? scale(p_a, 1.0f / len) : p_a;
}
}

Transform Transform::from_translation(const Vec3 &p_origin) {
	Transform t;
	t.origin = p_origin;
	return t;
}

Transform Transform::from_rotation_translation(const Vec4 &p_rotation, const Vec3 &p_origin) {
	const float x = p_rotation[0], y = p_rotation[1], z = p_rotation[2], w = p_rotation[3];
	Transform t;
	t.axis[0] = { 1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y + z * w), 2.0f * (x * z - y * w) };
	t.axis[1] = { 2.0f * (x * y - z * w), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z + x * w) };
	t.axis[2] = { 2.0f * (x * z + y * w), 2.0f * (y * z - x * w), 1.0f - 2.0f * (x * x + y * y) };
	t.origin = p_origin;
	return t;
}

Transform Transform::from_translation_rotation_scale(const Vec3 &p_translation, const Vec4 &p_rotation, const Vec3 &p_scale) {
	Transform t = from_rotation_translation(p_rotation, p_translation);
	for (int row = 0; row < 3; row++) {
		for (float &component : t.axis[row]) {
			component *= p_scale[row];
		}
	}
	return t;
}

Vec3 Transform::transform_vector(const Vec3 &p_v) const {
	Vec3 out{ 0.0f, 0.0f, 0.0f };
	for (int row = 0; row < 3; row++) {
		for (int a = 0; a < 3; a++) {
			out[a] += axis[row][a] * p_v[row];
		}
	}
	return out;
}

Vec3 Transform::transform_point(const Vec3 &p_p) const {
	const Vec3 v = transform_vector(p_p);
	return { v[0] + origin[0], v[1] + origin[1], v[2] + origin[2] };
}

Transform Transform::then_inner(const Transform &p_inner) const {
	Transform t;
	t.axis = { transform_vector(p_inner.axis[0]), transform_vector(p_inner.axis[1]), transform_vector(p_inner.axis[2]) };
	t.origin = transform_point(p_inner.origin);
	return t;
}

float Transform::determinant() const {
	return vec::dot(axis[0], vec::cross(axis[1], axis[2]));
}

Vec4 Transform::transform_plane(const Vec4 &p_plane) const {
	const Vec3 n{ p_plane[0], p_plane[1], p_plane[2] };
	const float len2 = vec::dot(n, n);
	if (len2 <= 0.0f) {
		return p_plane;
	}
	const Vec3 point = transform_point(vec::scale(n, -p_plane[3] / len2));
	const std::array<Vec3, 3> cofactor{ vec::cross(axis[1], axis[2]), vec::cross(axis[2], axis[0]), vec::cross(axis[0], axis[1]) };
	Vec3 normal{ 0.0f, 0.0f, 0.0f };
	for (int row = 0; row < 3; row++) {
		for (int a = 0; a < 3; a++) {
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
