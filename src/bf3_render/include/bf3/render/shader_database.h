#pragma once

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

inline constexpr uint32_t kCoordWorld = 2;
inline constexpr uint32_t kUsageTexCoord0 = 33;
inline constexpr uint32_t kUsageTexCoord1 = 34;
inline constexpr uint32_t kUsageTexCoord2 = 35;

struct ShaderTexture {
	std::string name;
	uint32_t coord_type = 0;
	uint32_t vertex_usage = 0;
	float factor = 1.0f;
};

struct ShaderTextureParameter {
	std::string name;
	uint32_t id = 0;
	uint32_t coord_type = 0;
	uint32_t vertex_usage = 0;
	float factor = 1.0f;
};

struct ShaderValue {
	std::string name;
	std::array<float, 4> value{};
};

struct ShaderSolution {
	uint64_t state_hash = 0;
	uint8_t flags = 0;
	SurfaceShaderType surface_type = SurfaceShaderType::Opaque;
	uint8_t blend_mode = 0;
};

struct SurfaceShader {
	uint32_t name_hash = 0;
	SurfaceShaderType surface_type = SurfaceShaderType::Opaque;
	uint8_t flags = 0;
	std::vector<ShaderTexture> textures;
	std::vector<ShaderTextureParameter> texture_parameters;
	std::vector<std::string> sampled_textures;
	std::vector<ShaderValue> values;
	std::vector<ShaderSolution> solutions;

	bool double_sided() const;
	const ShaderTextureParameter *parameter(std::string_view p_name) const;
	const ShaderValue *value(std::string_view p_name) const;
};

struct ShaderDatabase {
	uint32_t render_path = 0;
	std::vector<SurfaceShader> shaders;
};

inline constexpr uint64_t kShaderWorkPerByte = 4;
inline constexpr uint64_t kShaderWorkBase = uint64_t(1) << 20;

core::Result<std::vector<ShaderDatabase>> parse_shader_database(std::span<const uint8_t> p_res, uint64_t *r_work = nullptr);

ShaderDatabase merge_first_wins(std::vector<std::vector<SurfaceShader>> p_lists);

class ShaderLookup {
public:
	ShaderLookup() = default;
	explicit ShaderLookup(ShaderDatabase p_database);

	const SurfaceShader *by_name(std::string_view p_partition) const;
	size_t size() const { return database_.shaders.size(); }
	bool empty() const { return database_.shaders.empty(); }

private:
	ShaderDatabase database_;
	std::vector<std::pair<uint32_t, size_t>> by_hash_;
};

}
