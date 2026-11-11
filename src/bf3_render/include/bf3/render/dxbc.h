#pragma once

#include "bf3/core/result.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bf3::render {

enum class DxbcBindingType : uint32_t {
	ConstantBuffer = 0,
	TextureBuffer = 1,
	Texture = 2,
	Sampler = 3,
};

struct DxbcBinding {
	std::string name;
	DxbcBindingType type = DxbcBindingType::Texture;
	uint32_t slot = 0;
};

struct DxbcVariable {
	std::string name;
	uint32_t offset = 0;
	uint32_t size = 0;
};

struct DxbcConstantBuffer {
	std::string name;
	uint32_t size = 0;
	std::vector<DxbcVariable> variables;
};

struct DxbcProgram {
	uint32_t program_type = 0;
	uint32_t major = 0;
	uint32_t minor = 0;
	std::vector<DxbcBinding> bindings;
	std::vector<DxbcConstantBuffer> constant_buffers;
	std::vector<uint32_t> tokens;

	const DxbcBinding *binding(DxbcBindingType p_type, std::string_view p_name) const;
	const DxbcVariable *variable(std::string_view p_buffer, std::string_view p_name) const;
};

inline constexpr uint32_t kDxbcPixelProgram = 0;
inline constexpr size_t kMaxDxbcBytes = size_t(1) << 22;

core::Result<DxbcProgram> parse_dxbc(std::span<const uint8_t> p_bytes);

struct PixelTranslation {
	std::string body;
	uint32_t temps = 0;
	uint32_t outputs = 0;
	std::vector<uint32_t> inputs;
	std::vector<uint32_t> textures;
	std::vector<std::pair<uint32_t, uint32_t>> constant_buffers;
	uint64_t instructions = 0;
};

core::Result<PixelTranslation> translate_pixel_program(const DxbcProgram &p_program);

}
