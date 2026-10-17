#pragma once
// Bounds-checked byte and bit reader for the SWF readers (swf_movie.cpp, swf_font.cpp; docs/design/ui-swf-fonts.md
// §9). Little-endian bytes, MSB-first bit fields as the SWF specification defines them (UB / SB / FB, byte-aligned
// again by align()). A read past the end never touches memory outside the span: it returns zero and sets a sticky
// failure flag that the caller checks after each record (every loop over data also charges a WorkBudget, so a loop
// cannot spin on a failed reader).

#include "bf3/core/ui/swf_movie.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace bf3::core::ui::detail {

class SwfReader {
public:
	SwfReader() = default;
	explicit SwfReader(std::span<const uint8_t> p_data, size_t p_base = 0) :
			data_(p_data), base_(p_base) {}

	bool failed() const { return failed_; }
	void fail() { failed_ = true; }
	size_t pos() const { return pos_; }
	// Offset of the cursor in the enclosing buffer (for error messages).
	size_t abs_pos() const { return base_ + pos_; }
	size_t size() const { return data_.size(); }
	size_t left() const { return pos_ <= data_.size() ? data_.size() - pos_ : 0; }
	bool at_end() const { return pos_ >= data_.size(); }
	std::span<const uint8_t> data() const { return data_; }

	// Byte reads align first (as the SWF types do after a bit field).
	uint8_t u8() {
		align();
		if (!need(1)) {
			return 0;
		}
		return data_[pos_++];
	}
	uint16_t u16() {
		align();
		if (!need(2)) {
			return 0;
		}
		const uint16_t v = uint16_t(data_[pos_] | (data_[pos_ + 1] << 8));
		pos_ += 2;
		return v;
	}
	int16_t s16() { return int16_t(u16()); }
	uint32_t u32() {
		align();
		if (!need(4)) {
			return 0;
		}
		uint32_t v = 0;
		std::memcpy(&v, data_.data() + pos_, 4);
		pos_ += 4;
		return v;
	}
	float f32() {
		const uint32_t bits = u32();
		float f = 0.0f;
		std::memcpy(&f, &bits, 4);
		return f;
	}
	void skip(size_t p_count) {
		align();
		if (!need(p_count)) {
			pos_ = data_.size();
			return;
		}
		pos_ += p_count;
	}
	void seek(size_t p_pos) {
		bit_ = 0;
		if (p_pos > data_.size()) {
			failed_ = true;
			pos_ = data_.size();
			return;
		}
		pos_ = p_pos;
	}
	// NUL-terminated string inside the span (the NUL is consumed); empty + failure when no NUL follows.
	std::string_view cstr() {
		align();
		if (failed_) {
			return {};
		}
		const uint8_t *begin = data_.data() + pos_;
		const void *nul = std::memchr(begin, 0, data_.size() - pos_);
		if (nul == nullptr) {
			failed_ = true;
			pos_ = data_.size();
			return {};
		}
		const size_t len = size_t(static_cast<const uint8_t *>(nul) - begin);
		pos_ += len + 1;
		return std::string_view(reinterpret_cast<const char *>(begin), len);
	}
	// u8 length, then that many bytes (a trailing NUL inside the count is dropped).
	std::string_view pstr8() {
		const uint8_t len = u8();
		if (!need(len)) {
			return {};
		}
		std::string_view s(reinterpret_cast<const char *>(data_.data() + pos_), len);
		pos_ += len;
		while (!s.empty() && s.back() == '\0') {
			s.remove_suffix(1);
		}
		return s;
	}

	// Bit fields (MSB first).
	uint32_t ub(unsigned p_bits) {
		uint32_t v = 0;
		for (unsigned i = 0; i < p_bits; ++i) {
			if (pos_ >= data_.size()) {
				failed_ = true;
				return 0;
			}
			v = (v << 1) | ((data_[pos_] >> (7 - bit_)) & 1u);
			if (++bit_ == 8) {
				bit_ = 0;
				++pos_;
			}
		}
		return v;
	}
	int32_t sb(unsigned p_bits) {
		if (p_bits == 0) {
			return 0;
		}
		const uint32_t v = ub(p_bits);
		if (p_bits < 32 && (v >> (p_bits - 1)) != 0) {
			return int32_t(v | (~uint32_t(0) << p_bits));
		}
		return int32_t(v);
	}
	// 16.16 fixed point in a bit field.
	float fb(unsigned p_bits) { return float(double(sb(p_bits)) / 65536.0); }
	void align() {
		if (bit_ != 0) {
			bit_ = 0;
			++pos_;
		}
	}

	// RECT (twips) -> pixels.
	SwfRect rect() {
		align();
		const unsigned n = ub(5);
		SwfRect r;
		r.x_min = float(sb(n)) / 20.0f;
		r.x_max = float(sb(n)) / 20.0f;
		r.y_min = float(sb(n)) / 20.0f;
		r.y_max = float(sb(n)) / 20.0f;
		align();
		return r;
	}
	// MATRIX; translation in pixels.
	SwfMatrix matrix() {
		align();
		SwfMatrix m;
		if (ub(1) != 0) {
			const unsigned n = ub(5);
			m.a = fb(n);
			m.d = fb(n);
		}
		if (ub(1) != 0) {
			const unsigned n = ub(5);
			m.b = fb(n);
			m.c = fb(n);
		}
		const unsigned n = ub(5);
		m.tx = float(sb(n)) / 20.0f;
		m.ty = float(sb(n)) / 20.0f;
		align();
		return m;
	}
	// CXFORM (p_alpha false, PlaceObject) or CXFORMWITHALPHA.
	SwfColorTransform cxform(bool p_alpha) {
		align();
		SwfColorTransform c;
		const bool has_add = ub(1) != 0;
		const bool has_mul = ub(1) != 0;
		const unsigned n = ub(4);
		const int channels = p_alpha ? 4 : 3;
		if (has_mul) {
			for (int i = 0; i < channels; ++i) {
				c.mul[size_t(i)] = int16_t(sb(n));
			}
		}
		if (has_add) {
			for (int i = 0; i < channels; ++i) {
				c.add[size_t(i)] = int16_t(sb(n));
			}
		}
		align();
		return c;
	}

private:
	bool need(size_t p_count) {
		if (failed_ || pos_ > data_.size() || data_.size() - pos_ < p_count) {
			failed_ = true;
			return false;
		}
		return true;
	}

	std::span<const uint8_t> data_;
	size_t base_ = 0;
	size_t pos_ = 0;
	unsigned bit_ = 0;
	bool failed_ = false;
};

} // namespace bf3::core::ui::detail
