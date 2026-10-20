#pragma once

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

inline constexpr uint32_t kAtlasPixelsVersion = 1;

struct AtlasMip {
	uint32_t width = 0, height = 0;
	uint64_t offset = 0;
	uint64_t size = 0;
};

struct AtlasImage {
	std::string partition;
	core::res::PixelFormat format = core::res::PixelFormat::None;
	bool srgb = false;
	uint32_t width = 0, height = 0;
	std::vector<AtlasMip> mips;
	std::vector<uint8_t> bytes;
};

struct AtlasPixels {
	AtlasImage base;
	AtlasImage normal;
	bool from_cache = false;
	double seconds = 0;
};

core::Result<AtlasPixels> prepare_atlas_pixels(const EmitterAtlas &p_atlas, const core::LevelMount &p_mount, core::Cache *p_cache,
		core::gameplay::WorkBudget &r_budget);

inline constexpr const char *kAtlasPixelsCacheKind = "fx-atlas";
inline constexpr uint64_t kAtlasPixelsMaxBytes = uint64_t(256) << 20;
inline constexpr uint32_t kAtlasPixelsMaxMips = 16;
std::vector<uint8_t> encode_atlas_pixels(const AtlasPixels &p_pixels);
core::Status decode_atlas_pixels(std::span<const uint8_t> p_blob, AtlasPixels &r_pixels);

uint64_t atlas_level_bytes(uint32_t p_width, uint32_t p_height);

core::Result<AtlasImage> prepare_atlas_image(const core::res::LoadedTexture &p_texture, const AtlasTextureInfo &p_info,
		core::gameplay::WorkBudget &r_budget);

core::Status convert_normal_dxt5rga_to_bc5(const uint8_t *p_blocks, uint64_t p_size, uint32_t p_width, uint32_t p_height, std::vector<uint8_t> &r_out,
		core::gameplay::WorkBudget &r_budget);

}
