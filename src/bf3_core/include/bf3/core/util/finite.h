#pragma once

#include <cmath>
#include <span>

namespace bf3::core {

inline bool finite(float p_v) {
	return std::isfinite(p_v);
}

inline bool finite(double p_v) {
	return std::isfinite(p_v);
}

inline bool finite(std::span<const float> p_values) {
	for (const float v : p_values) {
		if (!std::isfinite(v)) {
			return false;
		}
	}
	return true;
}

inline bool finite(std::span<const double> p_values) {
	for (const double v : p_values) {
		if (!std::isfinite(v)) {
			return false;
		}
	}
	return true;
}

template <class V>
	requires requires(const V &p_v) {
		std::isfinite(p_v.x);
		std::isfinite(p_v.y);
		std::isfinite(p_v.z);
	}
bool finite(const V &p_v) {
	return std::isfinite(p_v.x) && std::isfinite(p_v.y) && std::isfinite(p_v.z);
}

}
