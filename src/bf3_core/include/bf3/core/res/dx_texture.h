#pragma once
// DxTexture res (resType 0x5C4954A6), handoff 02-resources §3; port of IW4L texture.rs.
//
// The res is the 128-byte header; pixels live in the chunk named by chunk_id. Chunk layout: per
// face/slice a full mip chain, largest mip first (face-major, the DDS order). A level bundle may
// carry the chunk *ranged*: its bundle entry's logicalOffset is where the stored bytes start inside
// the full chunk, == sum(mip_sizes[0..firstMip)). Split by that logicalOffset, never by
// chunkMeta.firstMip (2D arrays keep the whole chunk at logicalOffset 0 while firstMip > 0); for
// mip 0 the whole chunk is needed (the sha1 is the full chunk's).
//
// Upload: BC1/BC2/BC3/BC4/BC5 blocks as stored (pixel_format), sRGB only when FLAG_SRGB is set
// (never on normal maps). ARGB8888 is R,G,B,A in memory. BF3's format table has no BC6H/BC7.

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
	Dxt5A = 3, // BC4
	Dxn = 4, // BC5
	Rgb565 = 5,
	Rgb888 = 6,
	Argb1555 = 7,
	Argb4444 = 8,
	Argb8888 = 9, // bytes in memory are R, G, B, A despite the name
	L8 = 10,
	L16 = 11,
	Abgr16 = 12,
	Abgr16F = 13,
	Abgr32F = 14,
	R16F = 15,
	R32F = 16,
	NormalDxn = 17, // BC5, tangent X in R, Y in G; z = sqrt(1 - x^2 - y^2)
	NormalDxt1 = 18, // BC1 with X, Y, Z in R, G, B
	NormalDxt5 = 19, // BC3, X in alpha, Y in green
	NormalDxt5Rga = 20, // BC3, layout unresolved (particle textures only)
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

// Upload formats; block formats keep their blocks as stored, Rgba8 is R, G, B, A bytes.
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
int texture_block_bytes(TextureFormat p_format); // 8 or 16 for block formats, else 0
int texture_pixel_bytes(TextureFormat p_format); // uncompressed formats, else 0
// Bytes from one row of pixels (or 4x4 blocks) to the next; rows of uncompressed formats are padded
// to 4 bytes. -1 for a format without a known size.
int64_t texture_row_pitch(TextureFormat p_format, uint32_t p_width);
int64_t texture_image_bytes(TextureFormat p_format, uint32_t p_width, uint32_t p_height);

struct MipSlice {
	uint32_t face = 0; // cube face or array slice
	uint8_t mip = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t depth = 1;
	uint32_t row_pitch = 0; // 0 for formats without a known pitch
	Bytes data;

	// Reference CPU decode for L8, L16, ARGB8888, DXT1, DXT5 and DXT5A: every texel as [0, 1]
	// R, G, B, A, row by row (luminance repeats in R, G, B with A = 1). BC1 three-colour mode gives
	// transparent black; a BC3 colour block never punches through. False for other formats or
	// short data.
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
	uint16_t depth = 0; // 6 for cubes, the depth for volumes
	uint16_t slice_count = 0;
	uint8_t mip_count = 0;
	uint8_t mip_base = 0; // first mip kept resident with the level (== chunkMeta.firstMip)
	ChunkId chunk_id;
	std::vector<uint32_t> mip_sizes; // per face/slice, all depth slices for volumes
	uint32_t mip_chain_size = 0;
	uint32_t name_hash = 0;
	std::string texture_group;

	static ResError parse(Bytes p_res, DxTexture &r_texture);

	bool is_srgb() const { return (flags & kFlagSrgb) != 0; }
	// Images stored one after another in the chunk, each a full mip chain.
	uint32_t faces() const;
	void mip_dimensions(uint8_t p_mip, uint32_t &r_width, uint32_t &r_height, uint32_t &r_depth) const;
	// Byte offset of p_first_mip in the full chunk (a ranged bundle entry's logicalOffset).
	uint64_t logical_offset(uint8_t p_first_mip) const;
	// The mip a chunk view with this logicalOffset starts at, or -1.
	int first_mip_at(uint64_t p_logical_offset) const;
	// Splits chunk bytes into images, largest mip first, face-major. p_logical_offset is where p_chunk
	// starts in the full chunk: 0 for a full chunk, the bundle entry's logicalOffset for a ranged one;
	// mips before the first present one are left out for every face.
	ResError split_mips(Bytes p_chunk, uint64_t p_logical_offset, std::vector<MipSlice> &r_mips) const;
};

} // namespace bf3::core::res
