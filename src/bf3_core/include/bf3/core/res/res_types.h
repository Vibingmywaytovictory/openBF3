#pragma once

#include "bf3/core/math.h"
#include "bf3/core/res_types.h"

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
using Vec3 = core::Vec3;
using Vec4 = std::array<float, 4>;

inline constexpr uint32_t kResTypeMeshSet = res_type::kMeshSet;
inline constexpr uint32_t kResTypeDxTexture = res_type::kDxTexture;
inline constexpr uint32_t kResTypeHavokPhysicsData = res_type::kHavokPhysicsData;
inline constexpr uint32_t kResTypeHavokDestructionPhysicsData = res_type::kHavokDestructionPhysicsData;

struct ChunkId {
	std::array<uint8_t, 16> bytes{};

	bool is_zero() const;
	std::string to_string() const;
	std::string to_hex() const;
	friend bool operator==(const ChunkId &, const ChunkId &) = default;
};

struct ResError {
	enum class Kind : uint8_t {
		None,
		Truncated,
		Malformed,
		Unsupported,
	};

	Kind kind = Kind::None;
	const char *what = "";
	uint64_t at = 0;
	uint64_t needed = 0;
	uint64_t len = 0;
	uint32_t value = 0;

	bool ok() const { return kind == Kind::None; }

	std::string message() const;

	static ResError truncated(uint64_t p_at, uint64_t p_needed, uint64_t p_len);
	static ResError malformed(const char *p_what, uint64_t p_at);
	static ResError unsupported(const char *p_what, uint32_t p_value);
};

uint32_t hash_quick(std::string_view p_text);

float half_to_f32(uint16_t p_bits);

}
