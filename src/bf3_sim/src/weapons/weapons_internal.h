#pragma once
// Shared by the weapons sources (docs/design/m2a-weapons.md §5): the FNV-1a field hasher, double vector helpers,
// the aim frame, and the polynomial sin/cos of the replicated path (plan R4: no libm call where the server and a
// predicting client must agree to the bit; only +, -, *, / and sqrt, which IEEE 754 rounds identically everywhere).

#include "bf3/sim/sim_types.h"
#include "bf3/sim/weapons/ballistics.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace bf3::sim::weapons::detail {

class Fnv64 {
public:
	template <typename T>
	void add(const T &p_value) {
		static_assert(std::is_trivially_copyable_v<T>);
		unsigned char bytes[sizeof(T)];
		std::memcpy(bytes, &p_value, sizeof(T));
		for (const unsigned char b : bytes) {
			hash_ ^= b;
			hash_ *= 0x100000001B3ull;
		}
	}
	void add3(const Vec3 &p_v) {
		add(p_v[0]);
		add(p_v[1]);
		add(p_v[2]);
	}
	void addd(const DVec3 &p_v) {
		add(p_v[0]);
		add(p_v[1]);
		add(p_v[2]);
	}
	void add_str(std::string_view p_s) {
		add(uint64_t(p_s.size()));
		for (const char c : p_s) {
			add(c);
		}
	}
	uint64_t value() const { return hash_; }

private:
	uint64_t hash_ = 0xCBF29CE484222325ull;
};

inline constexpr double kPiD = 3.14159265358979323846;
inline constexpr double kDegToRad = kPiD / 180.0;

inline DVec3 to_d(const Vec3 &p_v) {
	return { double(p_v[0]), double(p_v[1]), double(p_v[2]) };
}
inline Vec3 to_f(const DVec3 &p_v) {
	return { float(p_v[0]), float(p_v[1]), float(p_v[2]) };
}
inline DVec3 add(const DVec3 &p_a, const DVec3 &p_b) {
	return { p_a[0] + p_b[0], p_a[1] + p_b[1], p_a[2] + p_b[2] };
}
inline DVec3 sub(const DVec3 &p_a, const DVec3 &p_b) {
	return { p_a[0] - p_b[0], p_a[1] - p_b[1], p_a[2] - p_b[2] };
}
inline DVec3 scale(const DVec3 &p_a, double p_s) {
	return { p_a[0] * p_s, p_a[1] * p_s, p_a[2] * p_s };
}
inline double dot(const DVec3 &p_a, const DVec3 &p_b) {
	return p_a[0] * p_b[0] + p_a[1] * p_b[1] + p_a[2] * p_b[2];
}
inline DVec3 cross(const DVec3 &p_a, const DVec3 &p_b) {
	return { p_a[1] * p_b[2] - p_a[2] * p_b[1], p_a[2] * p_b[0] - p_a[0] * p_b[2], p_a[0] * p_b[1] - p_a[1] * p_b[0] };
}
inline double length(const DVec3 &p_a) {
	return std::sqrt(dot(p_a, p_a));
}
inline DVec3 normalize(const DVec3 &p_a, const DVec3 &p_fallback = { 0, 0, 1 }) {
	const double l = length(p_a);
	return l > 1e-300 && std::isfinite(l) ? scale(p_a, 1.0 / l) : p_fallback;
}
inline bool finite3(const DVec3 &p_a) {
	return std::isfinite(p_a[0]) && std::isfinite(p_a[1]) && std::isfinite(p_a[2]);
}
inline bool finite3f(const Vec3 &p_a) {
	return std::isfinite(p_a[0]) && std::isfinite(p_a[1]) && std::isfinite(p_a[2]);
}

// sin and cos of p_x (any finite value) from a reduction to [-pi/4, pi/4] and Taylor polynomials to x^17 / x^16
// (|error| < 2e-16 there): deterministic on every IEEE machine, unlike libm.
inline void poly_sincos(double p_x, double &r_sin, double &r_cos) {
	if (!std::isfinite(p_x)) {
		r_sin = 0;
		r_cos = 1;
		return;
	}
	constexpr double kHalfPi = kPiD * 0.5;
	constexpr double kInvHalfPi = 2.0 / kPiD;
	const double q = p_x * kInvHalfPi;
	const double qr = q >= 0 ? double(int64_t(q + 0.5)) : -double(int64_t(-q + 0.5));
	const double r = p_x - qr * kHalfPi; // [-pi/4, pi/4] (up to rounding)
	const int64_t quadrant = int64_t(qr) & 3;
	const double r2 = r * r;
	// sin r = r - r^3/3! + ... ; cos r = 1 - r^2/2! + ...
	double s = 1.0 / 355687428096000.0; // 1/17!
	s = s * r2 - 1.0 / 1307674368000.0; // 1/15!
	s = s * r2 + 1.0 / 6227020800.0; // 1/13!
	s = s * r2 - 1.0 / 39916800.0; // 1/11!
	s = s * r2 + 1.0 / 362880.0; // 1/9!
	s = s * r2 - 1.0 / 5040.0;
	s = s * r2 + 1.0 / 120.0;
	s = s * r2 - 1.0 / 6.0;
	s = r + r * r2 * s;
	double c = 1.0 / 20922789888000.0; // 1/16!
	c = c * r2 - 1.0 / 87178291200.0; // 1/14!
	c = c * r2 + 1.0 / 479001600.0; // 1/12!
	c = c * r2 - 1.0 / 3628800.0; // 1/10!
	c = c * r2 + 1.0 / 40320.0;
	c = c * r2 - 1.0 / 720.0;
	c = c * r2 + 1.0 / 24.0;
	c = c * r2 - 0.5;
	c = 1.0 + r2 * c;
	switch (quadrant) {
		case 0:
			r_sin = s;
			r_cos = c;
			break;
		case 1:
			r_sin = c;
			r_cos = -s;
			break;
		case 2:
			r_sin = -s;
			r_cos = -c;
			break;
		default:
			r_sin = -c;
			r_cos = s;
			break;
	}
}

// The aim frame of a unit direction (x left, y up, z forward; ShotConfigData's weapon space): left = up x forward,
// up' = forward x left. Straight up or down (no horizontal part) takes left = +X.
struct Frame {
	DVec3 left{};
	DVec3 up{};
	DVec3 forward{};
};
inline Frame frame_of(const DVec3 &p_forward) {
	Frame f;
	f.forward = p_forward;
	const DVec3 l = cross({ 0, 1, 0 }, p_forward);
	f.left = length(l) > 1e-9 ? normalize(l) : DVec3{ 1, 0, 0 };
	f.up = cross(f.forward, f.left);
	return f;
}
inline DVec3 frame_apply(const Frame &p_f, const Vec3 &p_local) {
	return add(add(scale(p_f.left, double(p_local[0])), scale(p_f.up, double(p_local[1]))), scale(p_f.forward, double(p_local[2])));
}

} // namespace bf3::sim::weapons::detail
