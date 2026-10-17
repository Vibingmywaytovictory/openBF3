#pragma once
// The emitter atlases' pixels prepared for upload (docs/design/m2a.md §12.1, §13.1; owner fx2). Godot-free; runs on
// the loader thread. EmitterAtlas (emitter_atlas.h) reads only the two texture headers; this reads their pixel
// chunks through the level mount (read-only on the install), keeps the base atlas's DXT5 blocks as stored (BC3,
// sRGB) and converts the normal atlas's NormalDXT5RGA to BC5 (x = 2R - 1, y = 2A - 1 [inferred], plan §3.14), every
// mip. The prepared bytes are cached in the user cache root's texture cache, keyed by the atlas partition hash and
// kAtlasPixelsVersion, never written next to the install. Bf3FxRenderer uploads them with
// render_bridge::make_texture (Metro: ~21 MiB + 3 MiB VRAM [inferred]); E8 (6) gates 52/52 entries uploaded and 0
// default-texture fallbacks.
// Hostile data: sizes and mip counts come from the header and are bounded (EmitterAtlas::texture_info's limits); a
// chunk shorter than its mips needs fails with an error; every block loop charges the WorkBudget.

#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/res/dx_texture.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/core/result.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/fx/emitter_atlas.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bf3::fx {

inline constexpr uint32_t kAtlasPixelsVersion = 1; // part of the cache key: bump when the conversion changes

struct AtlasMip {
	uint32_t width = 0, height = 0;
	uint64_t offset = 0; // into AtlasImage::bytes
	uint64_t size = 0;
};

struct AtlasImage {
	std::string partition; // the TextureAsset partition
	core::res::PixelFormat format = core::res::PixelFormat::None; // Bc3 (base) or Bc5 (normal)
	bool srgb = false;
	uint32_t width = 0, height = 0;
	std::vector<AtlasMip> mips; // largest first
	std::vector<uint8_t> bytes;
};

struct AtlasPixels {
	AtlasImage base;
	AtlasImage normal;
	bool from_cache = false;
	double seconds = 0;
};

// p_cache may be null (convert without a cache).
core::Result<AtlasPixels> prepare_atlas_pixels(const EmitterAtlas &p_atlas, const core::LevelMount &p_mount, core::Cache *p_cache,
		core::gameplay::WorkBudget &r_budget);
// The cache payload ("FXATLASP" magic, kAtlasPixelsVersion): both images with their mips and bytes. The decoder
// checks every field (formats, sizes against the mip chain, offsets, the 256 MiB cap) before it allocates; hostile
// bytes fail with an error (fx.render.unit, asan preset).
inline constexpr const char *kAtlasPixelsCacheKind = "fx-atlas";
inline constexpr uint64_t kAtlasPixelsMaxBytes = uint64_t(256) << 20;
inline constexpr uint32_t kAtlasPixelsMaxMips = 16;
std::vector<uint8_t> encode_atlas_pixels(const AtlasPixels &p_pixels);
core::Status decode_atlas_pixels(std::span<const uint8_t> p_blob, AtlasPixels &r_pixels);
// The bytes of one mip level of a BC3 / BC5 image (4x4 blocks of 16 bytes; a level below 4 px still takes a block).
uint64_t atlas_level_bytes(uint32_t p_width, uint32_t p_height);
// Prepares one image from a loaded texture (the base keeps its blocks, a NormalDXT5RGA converts): fx.render.unit
// feeds synthetic LoadedTextures.
core::Result<AtlasImage> prepare_atlas_image(const core::res::LoadedTexture &p_texture, const AtlasTextureInfo &p_info,
		core::gameplay::WorkBudget &r_budget);
// The conversion alone (fx.render.unit runs it on synthetic blocks): one NormalDXT5RGA mip -> BC5 blocks.
core::Status convert_normal_dxt5rga_to_bc5(const uint8_t *p_blocks, uint64_t p_size, uint32_t p_width, uint32_t p_height, std::vector<uint8_t> &r_out,
		core::gameplay::WorkBudget &r_budget);

} // namespace bf3::fx
