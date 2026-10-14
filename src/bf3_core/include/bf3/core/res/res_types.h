#pragma once
// Common ground of the BF3 resource decoders (handoff sections/02-resources.md §1). Every decoder
// takes the decompressed `res` payload, the 16-byte `resMeta` of its bundle entry and, where the
// format needs one, decompressed chunk bytes, all as spans: nothing in bf3::core::res opens a file,
// so the container layer (install mount, superbundles, chunks) only has to hand over bytes.
//
// Every length and offset read from a payload is bounds-checked; a decoder never reads outside its
// spans and reports the first problem as a ResError (no exceptions). Port of IW4L
// crates/resource_bf3 (Apache-2.0, the user's own code); error texts match it word for word.
// Everything stays in Frostbite space: metres, Y up (handoff §13).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace bf3::core::res {

using Bytes = std::span<const uint8_t>;
using ResMeta = std::array<uint8_t, 16>;
using Vec2 = std::array<float, 2>;
using Vec3 = std::array<float, 3>;
using Vec4 = std::array<float, 4>;

// Bundle `resType` ids of the payloads decoded here.
inline constexpr uint32_t kResTypeMeshSet = 0x49B156D4u;
inline constexpr uint32_t kResTypeDxTexture = 0x5C4954A6u;
inline constexpr uint32_t kResTypeHavokPhysicsData = 0x91043F65u;
inline constexpr uint32_t kResTypeHavokDestructionPhysicsData = 0x4864737Bu;

// A chunk GUID as its 16 raw bytes; compare these, never a formatted form.
struct ChunkId {
	std::array<uint8_t, 16> bytes{};

	bool is_zero() const;
	std::string to_string() const; // Microsoft layout, as tools print GUIDs
	std::string to_hex() const; // the raw bytes as 32 lowercase hex digits
	friend bool operator==(const ChunkId &, const ChunkId &) = default;
};

struct ResError {
	enum class Kind : uint8_t {
		None,
		Truncated, // a read past the end of a span
		Malformed, // the bytes contradict the format
		Unsupported, // a valid but unhandled value
	};

	Kind kind = Kind::None;
	const char *what = ""; // Malformed / Unsupported: static text
	uint64_t at = 0; // Truncated / Malformed: offset
	uint64_t needed = 0; // Truncated: bytes wanted
	uint64_t len = 0; // Truncated: span length
	uint32_t value = 0; // Unsupported: the value

	bool ok() const { return kind == Kind::None; }
	// "needed N bytes at A of a L-byte payload" | "malformed W at A" | "unsupported W 0x.."; "" when ok.
	std::string message() const;

	static ResError truncated(uint64_t p_at, uint64_t p_needed, uint64_t p_len);
	static ResError malformed(const char *p_what, uint64_t p_at);
	static ResError unsupported(const char *p_what, uint32_t p_value);
};

// Frostbite's name hash (handoff §1 hashQuick): h = 5381; h = h * 33 ^ c, wrapping u32. MeshSet,
// MeshAsset and texture name hashes apply it to the lowercased asset name.
uint32_t hash_quick(std::string_view p_text);

// IEEE half to float, as the vertex decoders read it.
float half_to_f32(uint16_t p_bits);

} // namespace bf3::core::res
