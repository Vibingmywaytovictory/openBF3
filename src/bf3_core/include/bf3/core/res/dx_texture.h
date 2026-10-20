#pragma once

#include "bf3/core/res/res_types.h"

#include <string>
#include <vector>

namespace bf3::core::res {

enum class TextureKind : uint8_t {
	Texture2D = 0,
	Cube = 1,
	Volume = 2,
	Array2D = 3,
	Array1D = 4,
	Texture1D = 5,
	CubeArray = 6,
};

enum class TextureFormat : uint8_t {
	Dxt1 = 0,
	Dxt3 = 1,
	Dxt5 = 2,
	Dxt5A = 3,
	Dxn = 4,
	Rgb565 = 5,
	Rgb888 = 6,
	Argb1555 = 7,
	Argb4444 = 8,
	Argb8888 = 9,
	L8 = 10,
	L16 = 11,
	Abgr16 = 12,
	Abgr16F = 13,
	Abgr32F = 14,
	R16F = 15,
	R32F = 16,
	NormalDxn = 17,
	NormalDxt1 = 18,
	NormalDxt5 = 19,
	NormalDxt5Rga = 20,
	Rg8 = 21,
	Gr16 = 22,
	Gr16F = 23,
	D16 = 24,
	D24S8 = 25,
	D24FS8 = 26,
	D32F = 27,
	Abgr32 = 28,
	Gr32F = 29,
	A2R10G10B10 = 30,
};

enum class PixelFormat : uint8_t {
	None,
	Bc1,
	Bc2,
	Bc3,
	Bc4,
	Bc5,
	Rgba8,
	L8,
	L16,
};

PixelFormat texture_pixel_format(TextureFormat p_format);
bool texture_format_is_normal_map(TextureFormat p_format);
int texture_block_bytes(TextureFormat p_format);
int texture_pixel_bytes(TextureFormat p_format);

int64_t texture_row_pitch(TextureFormat p_format, uint32_t p_width);
int64_t texture_image_bytes(TextureFormat p_format, uint32_t p_width, uint32_t p_height);

struct MipSlice {
	uint32_t face = 0;
	uint8_t mip = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t depth = 1;
	uint32_t row_pitch = 0;
	Bytes data;

	bool texels(TextureFormat p_format, std::vector<std::array<float, 4>> &r_texels) const;
};

struct DxTexture {
	static constexpr uint32_t kFlagStreaming = 0x1;
	static constexpr uint32_t kFlagSrgb = 0x2;

	TextureKind kind = TextureKind::Texture2D;
	TextureFormat format = TextureFormat::Dxt1;
	uint32_t flags = 0;
	uint16_t width = 0;
	uint16_t height = 0;
	uint16_t depth = 0;
	uint16_t slice_count = 0;
	uint8_t mip_count = 0;
	uint8_t mip_base = 0;
	ChunkId chunk_id;
	std::vector<uint32_t> mip_sizes;
	uint32_t mip_chain_size = 0;
	uint32_t name_hash = 0;
	std::string texture_group;

	static ResError parse(Bytes p_res, DxTexture &r_texture);

	bool is_srgb() const { return (flags & kFlagSrgb) != 0; }

	uint32_t faces() const;
	void mip_dimensions(uint8_t p_mip, uint32_t &r_width, uint32_t &r_height, uint32_t &r_depth) const;

	uint64_t logical_offset(uint8_t p_first_mip) const;

	int first_mip_at(uint64_t p_logical_offset) const;

	ResError split_mips(Bytes p_chunk, uint64_t p_logical_offset, std::vector<MipSlice> &r_mips) const;
};

}
