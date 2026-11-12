#pragma once

#include "bf3/core/result.h"
#include "bf3/render/dxbc.h"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bf3::render {

struct PixelTranslation {
	std::string body;
	uint32_t temps = 0;
	uint32_t outputs = 0;
	std::vector<uint32_t> inputs;
	std::vector<uint32_t> textures;
	std::vector<std::pair<uint32_t, uint32_t>> constant_buffers;
	uint64_t instructions = 0;
};

core::Result<PixelTranslation> translate_pixel_program(const dxbc::Program &p_program);

enum class ProgramTarget : uint8_t {
	Deferred,
	Forward,
};

enum class ProgramBlend : uint8_t {
	Mix,
	Add,
	Multiply,
};

struct ProgramConstantLane {
	std::string value;
	uint8_t component = 0;
};

struct ProgramConstant {
	std::string uniform;
	std::array<ProgramConstantLane, 4> lanes;
};

struct ProgramAnalysis {
	bool sliced = false;
	std::vector<uint32_t> textures;
	std::vector<uint32_t> mask_only;
};

struct ProgramRequest {
	const dxbc::Program *pixel = nullptr;
	const dxbc::Program *vertex = nullptr;
	std::span<const uint32_t> vertex_formats;
	std::span<const uint32_t> bound_textures;
	ProgramTarget target = ProgramTarget::Deferred;
	ProgramBlend blend = ProgramBlend::Mix;
	bool double_sided = false;
	std::function<bool(uint32_t)> srgb;
	std::function<uint32_t(uint32_t)> texture_usage;
	ProgramAnalysis *analysis = nullptr;
};

struct TranslatedProgram {
	std::string code;
	std::vector<uint32_t> textures;
	std::vector<ProgramConstant> constants;
	size_t instructions = 0;
};

inline constexpr uint32_t kDxgiR32G32B32A32Float = 2;
inline constexpr uint32_t kDxgiR16G16B16A16Float = 10;
inline constexpr uint32_t kDxgiR32G32Float = 16;
inline constexpr uint32_t kDxgiR8G8B8A8Unorm = 28;
inline constexpr uint32_t kDxgiR16G16Float = 34;
inline constexpr uint32_t kDxgiR16G16Unorm = 35;
inline constexpr uint32_t kDxgiR16G16Snorm = 37;
inline constexpr uint32_t kDxgiB8G8R8A8Unorm = 87;

core::Result<TranslatedProgram> translate_program(const ProgramRequest &p_request);

}
