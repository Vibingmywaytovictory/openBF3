#include "bf3/net/bit_stream.h"

#include <bit>
#include <utility>

namespace bf3::net {

namespace {
constexpr uint64_t low_mask(unsigned p_bits) {
	return p_bits >= 64 ? ~uint64_t(0) : ((uint64_t(1) << p_bits) - 1);
}
}

BitWriter::BitWriter(size_t p_max_bytes) :
		max_bytes_(p_max_bytes) {
	bytes_.reserve(p_max_bytes < 4096 ? p_max_bytes : 4096);
}

void BitWriter::write_bits(uint64_t p_value, unsigned p_bits) {
	if (overflowed_) {
		return;
	}
	if (p_bits > 64) {
		overflowed_ = true;
		return;
	}
	if (p_bits == 0) {
		return;
	}
	if (bits_ + p_bits > max_bytes_ * 8) {
		overflowed_ = true;
		return;
	}
	uint64_t value = p_value & low_mask(p_bits);
	unsigned left = p_bits;
	while (left > 0) {
		const size_t index = bits_ / 8;
		const unsigned offset = unsigned(bits_ % 8);
		if (offset == 0) {
			bytes_.push_back(0);
		}
		const unsigned n = (8 - offset) < left ? (8 - offset) : left;
		bytes_[index] = uint8_t(bytes_[index] | uint8_t((value & low_mask(n)) << offset));
		value >>= n;
		left -= n;
		bits_ += n;
	}
}

void BitWriter::write_signed(int64_t p_value, unsigned p_bits) {
	write_bits(uint64_t(p_value), p_bits);
}

void BitWriter::write_varint(uint64_t p_value) {
	uint64_t v = p_value;
	for (;;) {
		const bool more = v > 0x7F;
		write_bool(more);
		write_bits(v & 0x7F, 7);
		v >>= 7;
		if (!more) {
			return;
		}
	}
}

void BitWriter::write_f32(float p_value) {
	write_bits(std::bit_cast<uint32_t>(p_value), 32);
}

void BitWriter::align_byte() {

	if (!overflowed_ && bits_ % 8 != 0) {
		bits_ += 8 - bits_ % 8;
	}
}

std::vector<uint8_t> BitWriter::take() {
	std::vector<uint8_t> out = std::move(bytes_);
	bytes_ = {};
	bits_ = 0;
	overflowed_ = false;
	return out;
}

BitReader::BitReader(std::span<const uint8_t> p_bytes) :
		bytes_(p_bytes) {}

size_t BitReader::bits_left() const {
	const size_t total = bytes_.size() * 8;
	return pos_ >= total ? 0 : total - pos_;
}

uint64_t BitReader::read_bits(unsigned p_bits) {
	if (failed_) {
		return 0;
	}
	if (p_bits > 64 || p_bits > bits_left()) {
		failed_ = true;
		return 0;
	}
	uint64_t value = 0;
	unsigned got = 0;
	while (got < p_bits) {
		const size_t index = pos_ / 8;
		const unsigned offset = unsigned(pos_ % 8);
		const unsigned n = (8 - offset) < (p_bits - got) ? (8 - offset) : (p_bits - got);
		const uint64_t chunk = (uint64_t(bytes_[index]) >> offset) & low_mask(n);
		value |= chunk << got;
		got += n;
		pos_ += n;
	}
	return value;
}

int64_t BitReader::read_signed(unsigned p_bits) {
	const uint64_t raw = read_bits(p_bits);
	if (p_bits == 0 || p_bits >= 64 || failed_) {
		return int64_t(raw);
	}
	const uint64_t sign = uint64_t(1) << (p_bits - 1);
	return int64_t((raw ^ sign) - sign);
}

uint64_t BitReader::read_varint() {
	uint64_t value = 0;
	for (unsigned group = 0; group < 10; ++group) {
		const bool more = read_bool();
		const uint64_t bits = read_bits(7);
		if (failed_) {
			return 0;
		}
		if (group == 9 && (more || bits > 1)) {
			failed_ = true;
			return 0;
		}
		value |= bits << (7 * group);
		if (!more) {
			return value;
		}
	}
	failed_ = true;
	return 0;
}

float BitReader::read_f32() {
	return std::bit_cast<float>(uint32_t(read_bits(32)));
}

void BitReader::align_byte() {
	if (failed_ || pos_ % 8 == 0) {
		return;
	}
	const size_t next = pos_ + (8 - pos_ % 8);
	if (next > bytes_.size() * 8) {
		failed_ = true;
		return;
	}
	pos_ = next;
}

bool BitReader::at_end() const {
	return !failed_ && (pos_ + 7) / 8 == bytes_.size();
}

}
