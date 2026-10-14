#pragma once
// GUIDs, SHA1s and name hashes of the BF3 containers. Ported from IW4L superbundle_bf3 (ids.rs).
//
// Guid bytes are kept in the canonical (DbObject / Microsoft) layout: Data1 little-endian u32,
// Data2/Data3 little-endian u16, then 8 bytes. Text form 8-4-4-4-12 with the first three fields
// read little-endian. Non-CAS bundles store chunk ids with big-endian fields (from_big_endian_fields).

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
	// Reverses Data1, Data2 and Data3 (byte 15, the chunk compression flag, is untouched).
	static Guid from_big_endian_fields(const uint8_t *p_bytes);
	// "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" with or without braces, any hex case.
	static std::optional<Guid> parse(std::string_view p_text);

	bool is_zero() const;
	std::string to_string() const;

	auto operator<=>(const Guid &) const = default;
};

struct Sha1 {
	std::array<uint8_t, 20> bytes{};

	static Sha1 from_bytes(const uint8_t *p_bytes);
	static std::optional<Sha1> parse(std::string_view p_text); // 40 hex digits
	std::string to_string() const; // lowercase hex

	auto operator<=>(const Sha1 &) const = default;
};

struct GuidHash {
	size_t operator()(const Guid &p_guid) const noexcept {
		uint64_t a, b;
		std::memcpy(&a, p_guid.bytes.data(), 8);
		std::memcpy(&b, p_guid.bytes.data() + 8, 8);
		return static_cast<size_t>(a ^ (b * 0x9E3779B97F4A7C15ull));
	}
};

struct Sha1Hash {
	size_t operator()(const Sha1 &p_sha1) const noexcept {
		uint64_t a; // a SHA1 is already uniformly distributed
		std::memcpy(&a, p_sha1.bytes.data(), 8);
		return static_cast<size_t>(a);
	}
};

// Frostbite hashQuick (djb2-xor, NOT FNV): h = 5381; h = (h * 33) ^ c, over the lowercased name.
// Used for bundle chunkMeta `h32`, MeshSet/DxTexture name hashes.
uint32_t asset_name_hash(std::string_view p_name);

// ASCII helpers shared by the container readers.
std::string to_lower_ascii(std::string_view p_text);
bool iequals_ascii(std::string_view p_a, std::string_view p_b);
std::string hex_lower(const uint8_t *p_bytes, size_t p_count);

} // namespace bf3::core
