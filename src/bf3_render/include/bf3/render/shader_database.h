#pragma once
// L1 look: the level's compiled shader database (res type IShaderDatabase 0x36F3F2C0, `levels/<id>/<id>/shaderdb`),
// read for what the EBX does not say about a surface shader: the textures its graph samples as constants (the
// look of every material that uses the graph without exposing them as parameters), the texture parameters it
// exposes with their UV set and tiling, the named vector values its pixel shaders read with the graph's
// defaults, and its solutions' surface type, blend mode and double-sided flag. Godot-free.
//
// Port of IW4L resource_bf3 shader.rs (the user's own Apache-2.0 code), without its DXBC terrain-albedo pass. Layout
// of one render path (version 182), little endian:
//   u32 version, u32 ?, u32 constant block count, then each block (u32 size including itself, then the block);
//   u32 n x (u16 constants, u16, 8 x constants); u32 n x (u32 textures, 4 x textures);
//   u32 n vertex shaders (16 + u32 size + size, 12, u32 signature + bytes, u32 elements x 24, u32 names = elements
//   NUL-terminated strings, u32); u32 n pixel shaders (16, u32 size + size, 16); u32 n x (16 + u32 size + size, u32);
//   u32 solutions x 64 bytes (u64 state hash, u8 flags (1 = double-sided), u8 surface type, u8 blend mode, ...,
//   i64 pixel shader index at 32, i64 pixel constant block index at 56); u32 solution states (= solutions) x 32;
//   u32 declarations x 80; u32 shaders, each: u32 name hash (ebx_name_hash of the lowercase ShaderGraph partition
//   name), u32 surface type, u8 flags, 35 bytes, u32 textures x (cstr name, u32 coord type, u32 vertex usage,
//   f32 factor), u32 texture parameters x (cstr name, u32 id, u32 coord type, u32 vertex usage, f32 factor),
//   u32 used solutions x u16 solution index.
// A constant block: u64 offset of its texture table at 0x10 (0x98-byte entries, name in 0x80 bytes at +8), u64 offset
// of its value table at 0x18 (0x3C-byte entries, name in 0x20 bytes, default vec4 at +0x2C), u8 texture count at
// 0x35, u8 value count at 0x36. The res starts with u32 render path count, then per path u32 id, u32 size, body.
//
// Hostile input fails with an error, never reads out of range or allocates beyond the input: every count is checked
// against the bytes left before anything is reserved, and the work of collecting each shader's sampled textures and
// values (solutions x constant entries) is charged to a budget of kWorkPerByte units per input byte + kWorkBase.

#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::render {

inline constexpr uint32_t kShaderDatabaseResType = 0x36F3F2C0u;
inline constexpr uint32_t kShaderDatabaseVersion = 182;

enum class SurfaceShaderType : uint8_t {
	Opaque = 0,
	OpaqueAlphaTest = 1,
	OpaqueAlphaTestSimple = 2,
	Transparent = 3,
	TransparentDecal = 4,
	Other = 255,
};
SurfaceShaderType surface_shader_type(uint32_t p_value);
inline bool is_alpha_tested(SurfaceShaderType p_type) {
	return p_type == SurfaceShaderType::OpaqueAlphaTest || p_type == SurfaceShaderType::OpaqueAlphaTestSimple;
}
inline bool is_transparent(SurfaceShaderType p_type) {
	return p_type == SurfaceShaderType::Transparent || p_type == SurfaceShaderType::TransparentDecal;
}

// ShaderTextureCoordType and vertex usage values the graphs use (IW4L materials.rs).
inline constexpr uint32_t kCoordWorld = 2; // projected along world position
inline constexpr uint32_t kUsageTexCoord0 = 33;
inline constexpr uint32_t kUsageTexCoord1 = 34;
inline constexpr uint32_t kUsageTexCoord2 = 35;

struct ShaderTexture { // a texture the graph samples as a constant (its TextureAsset partition = DxTexture res name)
	std::string name;
	uint32_t coord_type = 0;
	uint32_t vertex_usage = 0;
	float factor = 1.0f;
};

struct ShaderTextureParameter { // a texture parameter the graph exposes; materials bind it by name
	std::string name;
	uint32_t id = 0;
	uint32_t coord_type = 0;
	uint32_t vertex_usage = 0;
	float factor = 1.0f;
};

struct ShaderValue { // a vector a pixel shader reads from the material by name, with the graph's default
	std::string name;
	std::array<float, 4> value{};
};

struct ShaderSolution {
	uint64_t state_hash = 0;
	uint8_t flags = 0; // bit 0: double-sided
	SurfaceShaderType surface_type = SurfaceShaderType::Opaque;
	uint8_t blend_mode = 0; // 0 lerp, 1 additive, 2 multiply, 3 lerp premultiplied, 4..8 decal modes
};

struct SurfaceShader {
	uint32_t name_hash = 0;
	SurfaceShaderType surface_type = SurfaceShaderType::Opaque;
	uint8_t flags = 0;
	std::vector<ShaderTexture> textures;
	std::vector<ShaderTextureParameter> texture_parameters;
	std::vector<std::string> sampled_textures; // every texture its solutions' pixel constants bind by name, first-seen order
	std::vector<ShaderValue> values; // first-seen order, unique names
	std::vector<ShaderSolution> solutions;

	bool double_sided() const;
	const ShaderTextureParameter *parameter(std::string_view p_name) const; // case-insensitive
	const ShaderValue *value(std::string_view p_name) const; // case-insensitive
};

struct ShaderDatabase {
	uint32_t render_path = 0;
	std::vector<SurfaceShader> shaders;
};

inline constexpr uint64_t kShaderWorkPerByte = 4;
inline constexpr uint64_t kShaderWorkBase = uint64_t(1) << 20;

// Every render path of a res payload, in stored order. p_work (optional) receives the work units spent.
core::Result<std::vector<ShaderDatabase>> parse_shader_database(std::span<const uint8_t> p_res, uint64_t *r_work = nullptr);

// One database of the shaders of several lists (each a res's first render path), in order, keeping the first shader
// of each name hash. O(n log n) in the shader count (hostile databases may repeat a hash many times).
ShaderDatabase merge_first_wins(std::vector<std::vector<SurfaceShader>> p_lists);

// The surface shaders of the first render path, by name hash (the first entry of a hash wins).
class ShaderLookup {
public:
	ShaderLookup() = default;
	explicit ShaderLookup(ShaderDatabase p_database);
	// ShaderGraph partition name (any case) -> the shader, or nullptr.
	const SurfaceShader *by_name(std::string_view p_partition) const;
	size_t size() const { return database_.shaders.size(); }
	bool empty() const { return database_.shaders.empty(); }

private:
	ShaderDatabase database_;
	std::vector<std::pair<uint32_t, size_t>> by_hash_; // sorted by hash, then stored order (no data-keyed hash map)
};

} // namespace bf3::render
