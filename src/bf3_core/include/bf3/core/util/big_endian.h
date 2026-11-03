#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace bf3::core {

inline uint16_t be16(std::span<const uint8_t> p_bytes, size_t p_at) {
	return uint16_t((uint16_t(p_bytes[p_at]) << 8) | uint16_t(p_bytes[p_at + 1]));
}

inline int16_t be16s(std::span<const uint8_t> p_bytes, size_t p_at) {
	return int16_t(be16(p_bytes, p_at));
}

inline uint32_t be32(std::span<const uint8_t> p_bytes, size_t p_at) {
	return (uint32_t(p_bytes[p_at]) << 24) | (uint32_t(p_bytes[p_at + 1]) << 16) | (uint32_t(p_bytes[p_at + 2]) << 8) |
			uint32_t(p_bytes[p_at + 3]);
}

}
