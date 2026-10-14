#pragma once
// DxTexture -> GPU-ready image bytes for the Godot bridge (plan §2.5 "textures": resident BC from the install).
// Godot-free; runs on loader threads. The bridge turns a PreparedTexture into an Image/ImageTexture on the main
// thread without touching the bytes again.
//
// * Block formats keep their stored blocks (BC1/BC2/BC3/BC4/BC5); uncompressed formats their bytes.
// * Godot wants a full mip chain down to 1x1 (Image::get_image_required_mipmaps; block formats round each level
//   up to whole 4x4 blocks). The chain BF3 stores is cut to that length, or completed by repeating its smallest
//   level (scaled down for uncompressed formats) when it stops early.
// * Normal maps are handed out as two channels, tangent X in R and Y in G, because Godot's material shader
//   reads only .xy of a normal texture and rebuilds Z:
//     NormalDxn / Dxn (BC5): as stored.
//     NormalDxt1 (BC1, X Y Z in R G B): as stored.
//     NormalDxt5 (BC3, X in alpha, Y in green): transcoded to BC5 (the alpha block is already a BC4 block and
//       becomes R unchanged; green is re-encoded as a BC4 block, at most 1/14 of the block's green range off).
//     NormalDxt5Rga (BC3, "layout unresolved"): kept as stored. Measured on the only two in MP_Subway (particle and
//       UI smoke normals; render test "NormalDXT5RGA channel layout census"): over texels with A >= 0.5, G is the
//       reconstructed Z (mean |G - (z+1)/2| 0.015 with X,Y = R,B; 0.025 with B,A), A behaves like coverage, B is
//       far from centred (mean 0.13). Not decided; no mesh material uses the format there.
//   Only the textures used as a normal map are converted (TextureRole::Normal); MP_Subway has no NormalDxt5 at
//   all (the arms and M9 use NormalDXN and NormalDXT1), so the transcode is covered by the unit test only.
// * sRGB: PreparedTexture::srgb repeats DxTexture::is_srgb(); Godot's albedo slot decodes sRGB itself, so the
//   flag is informational for the bridge.

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
int gpu_format_block_bytes(GpuFormat p_format); // 8 / 16 for blocks, else 0
int gpu_format_pixel_bytes(GpuFormat p_format); // uncompressed, else 0

enum class TextureRole : uint8_t {
	Color,
	Normal,
	Data,
};

struct PreparedTexture {
	std::string name;
	GpuFormat format = GpuFormat::Bc1;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t levels = 0; // mip levels in data (Godot's full chain length)
	uint32_t stored_levels = 0; // levels BF3 stored
	bool srgb = false;
	core::res::TextureFormat source_format = core::res::TextureFormat::Dxt1;
	std::vector<uint8_t> data; // level 0 first, tightly packed
};

// Levels Godot's full chain has for this size and format (incl. level 0).
uint32_t godot_mip_levels(GpuFormat p_format, uint32_t p_width, uint32_t p_height);
// Bytes of one level / of the full chain (block formats: whole 4x4 blocks).
uint64_t level_bytes(GpuFormat p_format, uint32_t p_width, uint32_t p_height);

// A 2D texture's face 0 for upload. Fails for cube/volume/array textures, unsupported formats (float, depth,
// 16-bit) and short chunks.
core::Result<PreparedTexture> prepare_texture(const core::res::LoadedTexture &p_texture, TextureRole p_role);

// The BC3 -> BC5 normal transcode of one 16-byte block (exposed for the tests): R = the BC3 alpha block,
// G = the BC3 colour block's green re-encoded.
std::array<uint8_t, 16> bc3_normal_to_bc5(const uint8_t *p_bc3_block);
// Decoded green values of a BC1/BC3 colour block (4-colour mode, as BC3 always uses), 8 bits.
std::array<uint8_t, 16> bc_color_green(const uint8_t *p_color_block);
// Decoded values of a BC4 block.
std::array<uint8_t, 16> bc4_values(const uint8_t *p_block);
// A BC4 block (8-value mode) for 16 values.
std::array<uint8_t, 8> bc4_encode(const std::array<uint8_t, 16> &p_values);

} // namespace bf3::render
