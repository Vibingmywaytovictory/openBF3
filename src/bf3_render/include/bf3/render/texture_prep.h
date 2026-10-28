#pragma once

#include "bf3/core/res/dx_texture.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bf3::render {

enum class GpuFormat : uint8_t {
	Bc1,
	Bc2,
	Bc3,
	Bc4,
	Bc5,
	Rgba8,
	L8,
	Rg8,
};
const char *gpu_format_name(GpuFormat p_format);
bool gpu_format_is_block(GpuFormat p_format);
int gpu_format_block_bytes(GpuFormat p_format);
int gpu_format_pixel_bytes(GpuFormat p_format);

enum class TextureRole : uint8_t {
	Color,
	Normal,
	Data,
	Mask,
};

struct PreparedTexture {
	std::string name;
	GpuFormat format = GpuFormat::Bc1;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t levels = 0;
	uint32_t stored_levels = 0;
	bool srgb = false;
	TextureRole role = TextureRole::Color;
	core::res::TextureFormat source_format = core::res::TextureFormat::Dxt1;
	std::vector<uint8_t> data;
};

uint32_t godot_mip_levels(GpuFormat p_format, uint32_t p_width, uint32_t p_height);

uint64_t level_bytes(GpuFormat p_format, uint32_t p_width, uint32_t p_height);

core::Result<PreparedTexture> prepare_texture(const core::res::LoadedTexture &p_texture, TextureRole p_role);

std::array<uint8_t, 16> bc3_normal_to_bc5(const uint8_t *p_bc3_block);

std::array<uint8_t, 16> bc1_luminance(const uint8_t *p_block);

std::array<uint8_t, 16> bc1_mask_to_bc3(const uint8_t *p_bc1_block);

std::array<uint8_t, 16> bc_color_green(const uint8_t *p_color_block);

std::array<uint8_t, 16> bc4_values(const uint8_t *p_block);

std::array<uint8_t, 8> bc4_encode(const std::array<uint8_t, 16> &p_values);

}
