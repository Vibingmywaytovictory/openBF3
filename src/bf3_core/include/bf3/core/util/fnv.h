#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace bf3::core {

inline constexpr uint64_t kFnv1aOffset = 0xCBF29CE484222325ull;
inline constexpr uint64_t kFnv1aPrime = 0x100000001B3ull;

inline void fnv1a(uint64_t &r_hash, const void *p_data, size_t p_size) {
	const unsigned char *b = static_cast<const unsigned char *>(p_data);
	for (size_t i = 0; i < p_size; ++i) {
		r_hash ^= b[i];
		r_hash *= kFnv1aPrime;
	}
}

template <class T> void fnv1a_value(uint64_t &r_hash, const T &p_value) {
	static_assert(std::is_trivially_copyable_v<T>);
	unsigned char bytes[sizeof(T)];
	std::memcpy(bytes, &p_value, sizeof(T));
	fnv1a(r_hash, bytes, sizeof(T));
}

constexpr uint64_t fnv1a_fold(uint64_t p_hash, uint64_t p_value) {
	return (p_hash ^ p_value) * kFnv1aPrime;
}

}
