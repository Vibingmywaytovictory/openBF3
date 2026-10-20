#pragma once

#include "bf3/core/res/res_types.h"

#include <bit>
#include <string>

namespace bf3::core::res::detail {

inline uint64_t align16(uint64_t p_value) {
	return (p_value + 15) / 16 * 16;
}

inline uint64_t off(uint64_t p_a, uint64_t p_b) {
	const uint64_t s = p_a + p_b;
	return s < p_a ? UINT64_MAX : s;
}

inline uint64_t mul(uint64_t p_a, uint64_t p_b) {
	if (p_a != 0 && p_b > UINT64_MAX / p_a) {
		return UINT64_MAX;
	}
	return p_a * p_b;
}

bool valid_utf8(const uint8_t *p_data, size_t p_len);

class Reader {
public:
	explicit Reader(Bytes p_data) :
			data_(p_data) {}

	Bytes data() const { return data_; }
	uint64_t size() const { return data_.size(); }
	bool ok() const { return error_.ok(); }
	const ResError &error() const { return error_; }

	bool fail(const ResError &p_error) {
		if (error_.ok()) {
			error_ = p_error;
		}
		return false;
	}

	bool has(uint64_t p_at, uint64_t p_len) {
		if (!ok()) {
			return false;
		}
		if (p_at > data_.size() || p_len > data_.size() - p_at) {
			return fail(ResError::truncated(p_at, p_len, data_.size()));
		}
		return true;
	}
	Bytes slice(uint64_t p_at, uint64_t p_len) { return has(p_at, p_len) ? data_.subspan(size_t(p_at), size_t(p_len)) : Bytes(); }

	uint8_t u8(uint64_t p_at) { return has(p_at, 1) ? data_[size_t(p_at)] : 0; }
	uint16_t u16(uint64_t p_at) {
		if (!has(p_at, 2)) {
			return 0;
		}
		const uint8_t *b = data_.data() + p_at;
		return uint16_t(b[0] | (b[1] << 8));
	}
	uint32_t u32(uint64_t p_at) {
		if (!has(p_at, 4)) {
			return 0;
		}
		const uint8_t *b = data_.data() + p_at;
		return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
	}
	int32_t i32(uint64_t p_at) { return int32_t(u32(p_at)); }
	uint64_t u64(uint64_t p_at) {
		if (!has(p_at, 8)) {
			return 0;
		}
		const uint64_t lo = u32(p_at);
		const uint64_t hi = u32(p_at + 4);
		return lo | (hi << 32);
	}
	float f32(uint64_t p_at) {
		return std::bit_cast<float>(u32(p_at));
	}

	Vec3 vec3(uint64_t p_at) { return { f32(p_at), f32(off(p_at, 4)), f32(off(p_at, 8)) }; }
	Vec4 vec4(uint64_t p_at) { return { f32(p_at), f32(off(p_at, 4)), f32(off(p_at, 8)), f32(off(p_at, 12)) }; }
	ChunkId chunk_id(uint64_t p_at) {
		ChunkId id;
		const Bytes b = slice(p_at, 16);
		if (!b.empty()) {
			for (size_t i = 0; i < 16; i++) {
				id.bytes[i] = b[i];
			}
		}
		return id;
	}

	bool cstr(uint64_t p_at, std::string &r_out) {
		if (!ok()) {
			return false;
		}
		if (p_at > data_.size()) {
			return fail(ResError::truncated(p_at, 1, data_.size()));
		}
		const uint8_t *begin = data_.data() + p_at;
		const size_t avail = size_t(data_.size() - p_at);
		size_t end = 0;
		while (end < avail && begin[end] != 0) {
			end++;
		}
		if (end == avail) {
			return fail(ResError::malformed("unterminated string", p_at));
		}
		if (!valid_utf8(begin, end)) {
			return fail(ResError::malformed("string encoding", p_at));
		}
		r_out.assign(reinterpret_cast<const char *>(begin), end);
		return true;
	}

	std::string optional_cstr(uint64_t p_pointer) {
		std::string out;
		if (p_pointer != 0) {
			cstr(p_pointer, out);
		}
		return out;
	}

private:
	Bytes data_;
	ResError error_;
};

}
