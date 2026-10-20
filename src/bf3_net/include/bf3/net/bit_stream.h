#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bf3::net {

class BitWriter {
public:
	explicit BitWriter(size_t p_max_bytes = 1200);

	void write_bits(uint64_t p_value, unsigned p_bits);
	void write_bool(bool p_value) { write_bits(p_value ? 1u : 0u, 1); }
	void write_signed(int64_t p_value, unsigned p_bits);
	void write_varint(uint64_t p_value);
	void write_f32(float p_value);
	void align_byte();

	size_t bit_count() const { return bits_; }
	size_t byte_count() const { return (bits_ + 7) / 8; }
	bool overflowed() const { return overflowed_; }
	const std::vector<uint8_t> &bytes() const { return bytes_; }
	std::vector<uint8_t> take();

private:
	std::vector<uint8_t> bytes_;
	size_t bits_ = 0;
	size_t max_bytes_ = 0;
	bool overflowed_ = false;
};

class BitReader {
public:
	explicit BitReader(std::span<const uint8_t> p_bytes);

	uint64_t read_bits(unsigned p_bits);
	bool read_bool() { return read_bits(1) != 0; }
	int64_t read_signed(unsigned p_bits);
	uint64_t read_varint();
	float read_f32();
	void align_byte();

	size_t bits_left() const;
	bool failed() const { return failed_; }

	bool at_end() const;

private:
	std::span<const uint8_t> bytes_;
	size_t pos_ = 0;
	bool failed_ = false;
};

struct Quantizer {
	float min = 0;
	float step = 1;
	unsigned bits = 0;

	constexpr uint32_t max_code() const { return bits >= 32 ? 0xFFFFFFFFu : ((1u << bits) - 1u); }
	constexpr uint32_t encode(float p_value) const {
		const double t = (double(p_value) - double(min)) / double(step) + 0.5;
		if (!(t > 0.0)) {
			return 0;
		}
		const double top = double(max_code());
		return t >= top ? max_code() : uint32_t(t);
	}
	constexpr float decode(uint32_t p_code) const { return min + float(p_code > max_code() ? max_code() : p_code) * step; }
};

namespace quant {

inline constexpr Quantizer kPositionXZ{ -4096.0f, 1.0f / 256.0f, 21 };
inline constexpr Quantizer kPositionY{ -256.0f, 1.0f / 128.0f, 18 };
inline constexpr Quantizer kSoldierVelocity{ -32.0f, 1.0f / 32.0f, 11 };
inline constexpr unsigned kSoldierYawBits = 14;
inline constexpr unsigned kSoldierPitchBits = 12;
inline constexpr unsigned kCommandPitchBits = 14;
}

}
