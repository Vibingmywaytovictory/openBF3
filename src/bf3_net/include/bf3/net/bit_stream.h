#pragma once
// Bit packing for the snapshot and command datagrams (plan §2.2 bf3_net "bit packing", §3.5 "Quantisation";
// topics/05 §7.2; docs/design/m1.md §7). Godot-free.
//
// Bits are written least significant first into little-endian bytes. A writer has a byte cap (the datagram
// budget); writing past it sets overflowed() and drops the bits, so an encoder checks once at the end. A reader
// never reads past its span: a read that would sets failed() and yields 0, and every decoder returns an error
// when failed() is set at its end or when a decoded value is out of its range (input from the network is
// hostile).

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bf3::net {

class BitWriter {
public:
	explicit BitWriter(size_t p_max_bytes = 1200);

	void write_bits(uint64_t p_value, unsigned p_bits); // p_bits 0..64; higher bits of p_value are ignored
	void write_bool(bool p_value) { write_bits(p_value ? 1u : 0u, 1); }
	void write_signed(int64_t p_value, unsigned p_bits); // two's complement in p_bits
	void write_varint(uint64_t p_value); // 7 bits per group, continuation bit first
	void write_f32(float p_value); // the bit pattern, 32 bits (full-precision own state)
	void align_byte();

	size_t bit_count() const { return bits_; }
	size_t byte_count() const { return (bits_ + 7) / 8; }
	bool overflowed() const { return overflowed_; }
	const std::vector<uint8_t> &bytes() const { return bytes_; } // byte_count() bytes, last one zero padded
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
	uint64_t read_varint(); // at most 10 groups, else failed()
	float read_f32();
	void align_byte();

	size_t bits_left() const;
	bool failed() const { return failed_; }
	// Every bit up to the last whole byte consumed (trailing pad bits of the last byte are allowed).
	bool at_end() const;

private:
	std::span<const uint8_t> bytes_;
	size_t pos_ = 0; // bits
	bool failed_ = false;
};

// A uniform quantiser: value -> integer in [0, 2^bits), clamped; decode(encode(v)) is within step / 2 of v in
// range. The ranges of topics/05 §7.2.
struct Quantizer {
	float min = 0;
	float step = 1;
	unsigned bits = 0;

	constexpr uint32_t max_code() const { return bits >= 32 ? 0xFFFFFFFFu : ((1u << bits) - 1u); }
	constexpr uint32_t encode(float p_value) const {
		const float t = (p_value - min) / step + 0.5f;
		if (!(t > 0.0f)) {
			return 0; // also NaN
		}
		const float top = float(max_code());
		return t >= top ? max_code() : uint32_t(t);
	}
	constexpr float decode(uint32_t p_code) const { return min + float(p_code > max_code() ? max_code() : p_code) * step; }
};

namespace quant {
inline constexpr Quantizer kPositionXZ{ -2048.0f, 1.0f / 256.0f, 20 }; // +-2,048 m at 3.9 mm
inline constexpr Quantizer kPositionY{ -256.0f, 1.0f / 128.0f, 18 }; // -256..+1,792 m at 7.8 mm
inline constexpr Quantizer kSoldierVelocity{ -32.0f, 1.0f / 32.0f, 11 }; // +-32 m/s at 3.1 cm/s
inline constexpr unsigned kSoldierYawBits = 14; // of the 16-bit Command yaw
inline constexpr unsigned kSoldierPitchBits = 12; // of the 14-bit Command pitch range
inline constexpr unsigned kCommandPitchBits = 14;
} // namespace quant

} // namespace bf3::net
