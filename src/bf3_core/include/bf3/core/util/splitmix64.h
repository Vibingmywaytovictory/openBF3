#pragma once

#include <cstdint>

namespace bf3::core {

inline constexpr uint64_t kSplitmix64Gamma = 0x9E3779B97F4A7C15ull;

constexpr uint64_t splitmix64(uint64_t p_x) {
	uint64_t z = p_x + kSplitmix64Gamma;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

constexpr uint64_t splitmix64_next(uint64_t &r_state) {
	const uint64_t out = splitmix64(r_state);
	r_state += kSplitmix64Gamma;
	return out;
}

}
