// The emitter atlases' pixels prepared for upload (docs/design/m2a.md §12.1; docs/design/m2a-fx2.md). See
// atlas_pixels.h.
#include "bf3/fx/atlas_pixels.h"

#include "bf3/core/cache/blob_io.h"
#include "bf3/core/cache/cache_key.h"
#include "bf3/core/cache/cached_convert.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <optional>

namespace bf3::fx {

using core::res::LoadedTexture;
using core::res::MipSlice;
using core::res::PixelFormat;
using core::res::TextureFormat;

namespace {

constexpr const char *kBlobMagic = "FXATLASP";
constexpr uint64_t kBlockBytes = 16;
constexpr uint32_t kMaxSide = 16384; // EmitterAtlas::texture_info bounds the atlases far below this

// Red of a BC1-style colour block (BC3 always decodes it with four colours), as BC4 input values.
std::array<uint8_t, 16> colour_block_red(const uint8_t *p_block) {
	const uint16_t c0 = uint16_t(p_block[0] | (p_block[1] << 8));
	const uint16_t c1 = uint16_t(p_block[2] | (p_block[3] << 8));
	const int r0 = (c0 >> 11) & 31, r1 = (c1 >> 11) & 31;
	const int e0 = (r0 << 3) | (r0 >> 2), e1 = (r1 << 3) | (r1 >> 2);
	const int palette[4] = { e0, e1, (2 * e0 + e1 + 1) / 3, (e0 + 2 * e1 + 1) / 3 };
	const uint32_t bits = uint32_t(p_block[4]) | (uint32_t(p_block[5]) << 8) | (uint32_t(p_block[6]) << 16) | (uint32_t(p_block[7]) << 24);
	std::array<uint8_t, 16> out{};
	for (int i = 0; i < 16; ++i) {
		out[size_t(i)] = uint8_t(palette[(bits >> (2 * i)) & 3]);
	}
	return out;
}

// A BC4 block for 16 values: endpoints max / min, 8-value mode, nearest index.
std::array<uint8_t, 8> bc4_encode_values(const std::array<uint8_t, 16> &p_values) {
	const uint8_t lo = *std::min_element(p_values.begin(), p_values.end());
	const uint8_t hi = *std::max_element(p_values.begin(), p_values.end());
	std::array<uint8_t, 8> out{};
	out[0] = hi;
	out[1] = lo;
	if (hi == lo) {
		return out;
	}
	uint64_t bits = 0;
	for (int i = 0; i < 16; ++i) {
		const int step = std::clamp(int((float(hi - p_values[size_t(i)]) * 7.0f / float(hi - lo)) + 0.5f), 0, 7);
		const uint64_t index = step == 0 ? 0u : (step == 7 ? 1u : uint64_t(step + 1));
		bits |= index << (3 * i);
	}
	for (int i = 0; i < 6; ++i) {
		out[size_t(2 + i)] = uint8_t(bits >> (8 * i));
	}
	return out;
}

bool valid_chain(const AtlasImage &p_image) {
	if (p_image.width == 0 || p_image.height == 0 || p_image.width > kMaxSide || p_image.height > kMaxSide) {
		return false;
	}
	if (p_image.mips.empty() || p_image.mips.size() > kAtlasPixelsMaxMips) {
		return false;
	}
	uint64_t at = 0;
	for (size_t i = 0; i < p_image.mips.size(); ++i) {
		const AtlasMip &m = p_image.mips[i];
		const uint32_t w = std::max(1u, p_image.width >> i), h = std::max(1u, p_image.height >> i);
		if (m.width != w || m.height != h || m.offset != at || m.size != atlas_level_bytes(w, h)) {
			return false;
		}
		at += m.size;
	}
	return at == p_image.bytes.size();
}

void encode_image(core::BlobWriter &w, const AtlasImage &p_image) {
	w.str(p_image.partition);
	w.u8(uint8_t(p_image.format));
	w.boolean(p_image.srgb);
	w.u32(p_image.width);
	w.u32(p_image.height);
	w.u32(uint32_t(p_image.mips.size()));
	for (const AtlasMip &m : p_image.mips) {
		w.u32(m.width);
		w.u32(m.height);
		w.u64(m.offset);
		w.u64(m.size);
	}
	w.u64(p_image.bytes.size());
	w.raw(p_image.bytes.data(), p_image.bytes.size());
}

core::Status decode_image(core::BlobReader &r, AtlasImage &r_image, PixelFormat p_expected) {
	r_image = AtlasImage{};
	r_image.partition = r.str(1024);
	r_image.format = PixelFormat(r.u8());
	r_image.srgb = r.boolean();
	r_image.width = r.u32();
	r_image.height = r.u32();
	const uint32_t mips = r.u32();
	if (!r.ok()) {
		return r.error("atlas pixels blob");
	}
	if (r_image.format != p_expected) {
		return core::make_error("atlas pixels blob: image format " + std::to_string(int(r_image.format)));
	}
	if (mips == 0 || mips > kAtlasPixelsMaxMips) {
		return core::make_error("atlas pixels blob: mip count " + std::to_string(mips));
	}
	r_image.mips.resize(mips);
	for (AtlasMip &m : r_image.mips) {
		m.width = r.u32();
		m.height = r.u32();
		m.offset = r.u64();
		m.size = r.u64();
	}
	const uint64_t bytes = r.u64();
	if (!r.ok()) {
		return r.error("atlas pixels blob");
	}
	if (bytes > kAtlasPixelsMaxBytes || bytes > r.remaining()) {
		return core::make_error("atlas pixels blob: " + std::to_string(bytes) + " pixel bytes past the cap or the blob");
	}
	if (!r.array(size_t(bytes), r_image.bytes)) {
		return r.error("atlas pixels blob");
	}
	if (!valid_chain(r_image)) {
		return core::make_error("atlas pixels blob: mip chain does not match the image size");
	}
	return core::Status::success();
}

std::optional<core::CacheKey> atlas_pixels_key(const EmitterAtlas &p_atlas, const core::LevelMount &p_mount) {
	core::CacheKeyInputs in;
	in.kind = kAtlasPixelsCacheKind;
	for (const AtlasTextureInfo *info : { &p_atlas.base(), &p_atlas.normal() }) {
		const std::optional<core::Sha1> res = p_mount.entry_sha1(core::EntryRef::res(info->partition));
		if (!res) {
			return std::nullopt;
		}
		in.sources.push_back(core::source_digest(*res));
		// The chunk id is in the res header; read the 128 bytes once more (cheap) for the chunk's own SHA1s.
		core::Result<core::res::RawRes> raw = core::res::read_raw_res(p_mount, info->partition);
		if (!raw) {
			return std::nullopt;
		}
		core::res::DxTexture tex;
		if (!core::res::DxTexture::parse(raw->bytes(), tex).ok()) {
			return std::nullopt;
		}
		const std::vector<core::Sha1> chunk = p_mount.chunk_source_sha1s(core::Guid::from_bytes(tex.chunk_id.bytes.data()));
		if (chunk.empty()) {
			return std::nullopt;
		}
		for (const core::Sha1 &s : chunk) {
			in.sources.push_back(core::source_digest(s));
		}
	}
	in.converter_version = kAtlasPixelsVersion;
	in.engine = std::string(core::cache_engine_id());
	in.format_version = kAtlasPixelsVersion;
	in.params = "base=" + p_atlas.base().partition + ";normal=" + p_atlas.normal().partition;
	return core::make_cache_key(in);
}

} // namespace

uint64_t atlas_level_bytes(uint32_t p_width, uint32_t p_height) {
	return uint64_t((std::max(p_width, 1u) + 3) / 4) * ((std::max(p_height, 1u) + 3) / 4) * kBlockBytes;
}

core::Status convert_normal_dxt5rga_to_bc5(const uint8_t *p_blocks, uint64_t p_size, uint32_t p_width, uint32_t p_height, std::vector<uint8_t> &r_out,
		core::gameplay::WorkBudget &r_budget) {
	if (p_width == 0 || p_height == 0 || p_width > kMaxSide || p_height > kMaxSide) {
		return core::make_error("normal atlas: mip size " + std::to_string(p_width) + " x " + std::to_string(p_height));
	}
	const uint64_t bw = (uint64_t(p_width) + 3) / 4, bh = (uint64_t(p_height) + 3) / 4;
	const uint64_t need = bw * bh * kBlockBytes;
	if (p_blocks == nullptr || p_size < need) {
		return core::make_error("normal atlas: mip of " + std::to_string(p_size) + " bytes, needs " + std::to_string(need));
	}
	const size_t base = r_out.size();
	r_out.resize(base + size_t(need));
	uint8_t *out = r_out.data() + base;
	for (uint64_t y = 0; y < bh; ++y) {
		if (core::Status s = r_budget.charge(1 + size_t(bw / 16)); !s.ok()) {
			r_out.resize(base);
			return s;
		}
		for (uint64_t x = 0; x < bw; ++x) {
			const uint64_t at = (y * bw + x) * kBlockBytes;
			const uint8_t *in = p_blocks + at;
			// NormalDXT5RGA [inferred]: tangent x in the colour block's red, y in the alpha block. BC5: red block = x,
			// green block = y (the alpha block is a BC4 block as stored).
			const std::array<uint8_t, 8> red = bc4_encode_values(colour_block_red(in + 8));
			std::memcpy(out + at, red.data(), 8);
			std::memcpy(out + at + 8, in, 8);
		}
	}
	return core::Status::success();
}

core::Result<AtlasImage> prepare_atlas_image(const LoadedTexture &p_texture, const AtlasTextureInfo &p_info, core::gameplay::WorkBudget &r_budget) {
	const core::res::DxTexture &t = p_texture.texture;
	const std::string where = "atlas " + p_info.partition;
	if (t.format != p_info.format || t.width != p_info.width || t.height != p_info.height) {
		return core::make_error(where + ": texture header differs from the atlas header");
	}
	const bool normal = t.format == TextureFormat::NormalDxt5Rga;
	if (t.format != TextureFormat::Dxt5 && !normal) {
		return core::make_error(where + ": format " + std::to_string(int(t.format)) + " (DXT5 or NormalDXT5RGA expected)");
	}
	if (p_info.mips == 0 || p_info.mips > kAtlasPixelsMaxMips || p_info.width == 0 || p_info.height == 0 || p_info.width > kMaxSide ||
			p_info.height > kMaxSide) {
		return core::make_error(where + ": size or mip count out of range");
	}
	std::vector<MipSlice> slices;
	if (const core::res::ResError e = p_texture.mips(slices); !e.ok()) {
		return core::make_error(where + ": " + e.message());
	}
	AtlasImage img;
	img.partition = p_info.partition;
	img.format = normal ? PixelFormat::Bc5 : PixelFormat::Bc3;
	img.srgb = !normal && p_info.srgb;
	img.width = p_info.width;
	img.height = p_info.height;
	uint64_t total = 0;
	for (uint32_t i = 0; i < p_info.mips; ++i) {
		total += atlas_level_bytes(std::max(1u, img.width >> i), std::max(1u, img.height >> i));
	}
	if (total > kAtlasPixelsMaxBytes) {
		return core::make_error(where + ": " + std::to_string(total) + " bytes past the cap");
	}
	img.bytes.reserve(size_t(total));
	for (uint32_t i = 0; i < p_info.mips; ++i) {
		const uint32_t w = std::max(1u, img.width >> i), h = std::max(1u, img.height >> i);
		const MipSlice *slice = nullptr;
		for (const MipSlice &s : slices) {
			if (s.face == 0 && s.mip == i) {
				slice = &s;
				break;
			}
		}
		if (slice == nullptr || slice->width != w || slice->height != h) {
			return core::make_error(where + ": mip " + std::to_string(i) + " missing from the chunk");
		}
		const uint64_t need = atlas_level_bytes(w, h);
		if (slice->data.size() < need) {
			return core::make_error(where + ": mip " + std::to_string(i) + " has " + std::to_string(slice->data.size()) + " bytes, needs " +
					std::to_string(need));
		}
		AtlasMip m;
		m.width = w;
		m.height = h;
		m.offset = img.bytes.size();
		m.size = need;
		if (normal) {
			if (core::Status s = convert_normal_dxt5rga_to_bc5(slice->data.data(), slice->data.size(), w, h, img.bytes, r_budget); !s.ok()) {
				return core::make_error(where + ": " + s.error().message);
			}
		} else {
			if (core::Status s = r_budget.charge(1 + size_t(need / (64 * 1024))); !s.ok()) {
				return s.error();
			}
			img.bytes.insert(img.bytes.end(), slice->data.begin(), slice->data.begin() + ptrdiff_t(need));
		}
		img.mips.push_back(m);
	}
	return img;
}

std::vector<uint8_t> encode_atlas_pixels(const AtlasPixels &p_pixels) {
	core::BlobWriter w;
	w.magic(kBlobMagic, kAtlasPixelsVersion);
	encode_image(w, p_pixels.base);
	encode_image(w, p_pixels.normal);
	return w.take();
}

core::Status decode_atlas_pixels(std::span<const uint8_t> p_blob, AtlasPixels &r_pixels) {
	core::BlobReader r(p_blob);
	const uint32_t version = r.magic(kBlobMagic);
	if (!r.ok()) {
		return r.error("atlas pixels blob");
	}
	if (version != kAtlasPixelsVersion) {
		return core::make_error("atlas pixels blob: format version " + std::to_string(version));
	}
	AtlasPixels out;
	if (core::Status s = decode_image(r, out.base, PixelFormat::Bc3); !s.ok()) {
		return s;
	}
	if (core::Status s = decode_image(r, out.normal, PixelFormat::Bc5); !s.ok()) {
		return s;
	}
	if (!r.at_end()) {
		return core::make_error("atlas pixels blob: trailing bytes");
	}
	if (out.normal.srgb) {
		return core::make_error("atlas pixels blob: sRGB normal atlas");
	}
	r_pixels = std::move(out);
	return core::Status::success();
}

core::Result<AtlasPixels> prepare_atlas_pixels(const EmitterAtlas &p_atlas, const core::LevelMount &p_mount, core::Cache *p_cache,
		core::gameplay::WorkBudget &r_budget) {
	const auto t0 = std::chrono::steady_clock::now();
	AtlasPixels pixels;
	bool produced = false;
	auto produce = [&]() -> core::Result<std::vector<uint8_t>> {
		for (int k = 0; k < 2; ++k) {
			const AtlasTextureInfo &info = k == 0 ? p_atlas.base() : p_atlas.normal();
			core::Result<LoadedTexture> tex = core::res::load_texture(p_mount, info.partition);
			if (!tex) {
				return core::make_error("atlas " + info.partition + ": " + tex.error().message);
			}
			core::Result<AtlasImage> img = prepare_atlas_image(*tex, info, r_budget);
			if (!img) {
				return img.error();
			}
			(k == 0 ? pixels.base : pixels.normal) = std::move(*img);
		}
		produced = true;
		return encode_atlas_pixels(pixels);
	};
	const std::optional<core::CacheKey> key = p_cache != nullptr ? atlas_pixels_key(p_atlas, p_mount) : std::nullopt;
	if (!key) {
		core::Result<std::vector<uint8_t>> blob = produce();
		if (!blob) {
			return blob.error();
		}
	} else {
		auto accept = [&](std::span<const uint8_t> p_blob) -> core::Status {
			AtlasPixels decoded;
			if (core::Status s = decode_atlas_pixels(p_blob, decoded); !s.ok()) {
				return s;
			}
			if (decoded.base.partition != p_atlas.base().partition || decoded.normal.partition != p_atlas.normal().partition) {
				return core::make_error("atlas pixels blob: partitions differ");
			}
			pixels = std::move(decoded);
			return core::Status::success();
		};
		core::Result<core::CachedItem> item = core::cached_convert(p_cache, *key, accept, produce);
		if (!item) {
			return item.error();
		}
		pixels.from_cache = item->outcome == core::CacheOutcome::Hit && !produced;
	}
	pixels.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	return pixels;
}

} // namespace bf3::fx
