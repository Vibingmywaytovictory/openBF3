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
}
