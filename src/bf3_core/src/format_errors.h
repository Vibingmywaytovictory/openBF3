#pragma once
// Error constructors of the container readers, worded like IW4L superbundle_bf3 (error.rs).

#include "bf3/core/result.h"

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>

namespace bf3::core::detail {

// "0x" + p_digits lowercase hex digits (Rust {:#0Nx} with N = p_digits + 2).
inline std::string hex_prefixed(uint64_t p_value, int p_digits) {
	char text[32];
	std::snprintf(text, sizeof(text), "0x%0*llx", p_digits, static_cast<unsigned long long>(p_value));
	return text;
}

inline Error truncated(const char *p_what, uint64_t p_need, uint64_t p_have) {
	return make_error(std::string(p_what) + " is truncated: need " + std::to_string(p_need) + " bytes, have " + std::to_string(p_have));
}

inline Error bad_magic(const char *p_what, uint32_t p_got) {
	return make_error(std::string(p_what) + " has bad magic " + hex_prefixed(p_got, 8));
}

inline Error bad_db_object(size_t p_at, const char *p_problem) {
	return make_error("malformed DbObject at byte " + std::to_string(p_at) + ": " + p_problem);
}

inline Error bad_utf8(const char *p_what) {
	return make_error(std::string(p_what) + " is not UTF-8");
}

inline Error missing_field(const char *p_what, const char *p_field) {
	return make_error(std::string(p_what) + " has no `" + p_field + "`");
}

inline Error field_type(const char *p_what, const char *p_field) {
	return make_error(std::string(p_what) + " `" + p_field + "` has the wrong type");
}

inline Error field_range(const char *p_what, const char *p_field, int64_t p_value) {
	return make_error(std::string(p_what) + " `" + p_field + "` is out of range (" + std::to_string(p_value) + ")");
}

inline Error layout_error(const char *p_what, const char *p_problem) {
	return make_error(std::string(p_what) + ": " + p_problem);
}

// Upper bound for one buffer whose size comes from metadata alone (a decoded payload, an assembled
// delta bundle). Retail payloads and bundles stay far below it; a corrupt size that would exceed it
// becomes an error instead of an allocation failure, which terminates (no exceptions).
constexpr uint64_t kMaxMetadataSizedAlloc = uint64_t(1) << 30;

inline Error out_of_memory(uint64_t p_bytes) {
	return make_error("cannot allocate " + std::to_string(p_bytes) + " bytes");
}

inline Status need(const char *p_what, size_t p_have, size_t p_need) {
	if (p_have < p_need) {
		return truncated(p_what, p_need, p_have);
	}
	return {};
}

// Strict UTF-8 validation (Rust str::from_utf8 rules: no overlongs, no surrogates, <= U+10FFFF).
inline bool valid_utf8(std::span<const uint8_t> p_bytes) {
	size_t i = 0;
	const size_t n = p_bytes.size();
	while (i < n) {
		const uint8_t c = p_bytes[i];
		if (c < 0x80) {
			++i;
			continue;
		}
		size_t len = 0;
		uint8_t lo = 0x80, hi = 0xBF;
		if (c >= 0xC2 && c <= 0xDF) {
			len = 2;
		} else if (c >= 0xE0 && c <= 0xEF) {
			len = 3;
			if (c == 0xE0) {
				lo = 0xA0;
			} else if (c == 0xED) {
				hi = 0x9F;
			}
		} else if (c >= 0xF0 && c <= 0xF4) {
			len = 4;
			if (c == 0xF0) {
				lo = 0x90;
			} else if (c == 0xF4) {
				hi = 0x8F;
			}
		} else {
			return false;
		}
		if (i + len > n) {
			return false;
		}
		const uint8_t c1 = p_bytes[i + 1];
		if (c1 < lo || c1 > hi) {
			return false;
		}
		for (size_t k = 2; k < len; ++k) {
			if ((p_bytes[i + k] & 0xC0) != 0x80) {
				return false;
			}
		}
		i += len;
	}
	return true;
}

} // namespace bf3::core::detail
