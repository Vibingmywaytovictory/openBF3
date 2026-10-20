#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace bf3::core {

struct Guid {
	std::array<uint8_t, 16> bytes{};

	static Guid from_bytes(const uint8_t *p_bytes);

	static Guid from_big_endian_fields(const uint8_t *p_bytes);

	static std::optional<Guid> parse(std::string_view p_text);

	bool is_zero() const;
	std::string to_string() const;

	auto operator<=>(const Guid &) const = default;
};

struct Sha1 {
	std::array<uint8_t, 20> bytes{};

	static Sha1 from_bytes(const uint8_t *p_bytes);
	static std::optional<Sha1> parse(std::string_view p_text);
	std::string to_string() const;

	auto operator<=>(const Sha1 &) const = default;
};

uint64_t hash_seed();

inline uint64_t hash_mix(uint64_t p_x) {
	p_x ^= p_x >> 33;
	p_x *= 0xFF51AFD7ED558CCDull;
	p_x ^= p_x >> 33;
	p_x *= 0xC4CEB9FE1A85EC53ull;
	p_x ^= p_x >> 33;
	return p_x;
}

inline uint64_t hash_keyed(uint64_t p_a, uint64_t p_b) {
	return hash_mix(hash_mix(p_a ^ hash_seed()) ^ p_b);
}

struct GuidHash {
	size_t operator()(const Guid &p_guid) const noexcept {
		uint64_t a, b;
		std::memcpy(&a, p_guid.bytes.data(), 8);
		std::memcpy(&b, p_guid.bytes.data() + 8, 8);
		return static_cast<size_t>(hash_keyed(a, b));
	}
};

struct Sha1Hash {
	size_t operator()(const Sha1 &p_sha1) const noexcept {

		uint64_t a, b;
		uint32_t c;
		std::memcpy(&a, p_sha1.bytes.data(), 8);
		std::memcpy(&b, p_sha1.bytes.data() + 8, 8);
		std::memcpy(&c, p_sha1.bytes.data() + 16, 4);
		return static_cast<size_t>(hash_mix(hash_keyed(a, b) ^ c));
	}
};

uint32_t asset_name_hash(std::string_view p_name);

std::string to_lower_ascii(std::string_view p_text);
bool iequals_ascii(std::string_view p_a, std::string_view p_b);
std::string hex_lower(const uint8_t *p_bytes, size_t p_count);

}
