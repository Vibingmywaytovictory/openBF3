#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace bf3::core {

template <class T> T nearest_rank(std::vector<T> p_values, double p_q) {
	if (p_values.empty()) {
		return T(0);
	}
	std::sort(p_values.begin(), p_values.end());
	const double rank = std::ceil(p_q * double(p_values.size()));
	const size_t at = rank < 1.0 ? 0 : std::min(p_values.size() - 1, size_t(rank) - 1);
	return p_values[at];
}

template <class T> T rank_above(std::vector<T> p_values, double p_q) {
	if (p_values.empty()) {
		return T(0);
	}
	std::sort(p_values.begin(), p_values.end());
	return p_values[std::min(p_values.size() - 1, size_t(double(p_values.size()) * p_q))];
}

template <class T> T rounded_rank(std::vector<T> p_values, double p_q) {
	if (p_values.empty()) {
		return T(0);
	}
	std::sort(p_values.begin(), p_values.end());
	return p_values[std::min(p_values.size() - 1, size_t(std::floor(p_q * double(p_values.size() - 1) + 0.5)))];
}

inline double interpolated_percentile(std::vector<double> p_values, double p_q) {
	if (p_values.empty()) {
		return 0.0;
	}
	std::sort(p_values.begin(), p_values.end());
	const double at = p_q * double(p_values.size() - 1);
	const size_t i = size_t(at);
	const double f = at - double(i);
	return i + 1 < p_values.size() ? p_values[i] * (1.0 - f) + p_values[i + 1] * f : p_values.back();
}

}
