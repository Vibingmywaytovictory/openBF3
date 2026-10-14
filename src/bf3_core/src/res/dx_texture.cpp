// DxTexture decoder: port of IW4L crates/resource_bf3/src/texture.rs.
#include "bf3/core/res/dx_texture.h"

#include "res_reader.h"

namespace bf3::core::res {

using detail::Reader;

namespace {

constexpr uint64_t kHeaderBytes = 128;
constexpr uint8_t kMipSlots = 15;

// Rust String::from_utf8_lossy: each maximal invalid prefix becomes one U+FFFD.
std::string lossy_utf8(Bytes p_bytes) {
	std::string out;
	size_t i = 0;
	const size_t n = p_bytes.size();
	while (i < n) {
		const uint8_t c = p_bytes[i];
		if (c < 0x80) {
			out.push_back(char(c));
			i++;
			continue;
		}
		size_t need = 0;
		uint8_t lo = 0x80, hi = 0xBF;
		if (c >= 0xC2 && c <= 0xDF) {
			need = 1;
		} else if (c >= 0xE0 && c <= 0xEF) {
			need = 2;
			lo = c == 0xE0 ? 0xA0 : 0x80;
			hi = c == 0xED ? 0x9F : 0xBF;
		} else if (c >= 0xF0 && c <= 0xF4) {
			need = 3;
			lo = c == 0xF0 ? 0x90 : 0x80;
			hi = c == 0xF4 ? 0x8F : 0xBF;
		}
		size_t good = 1; // bytes of a valid prefix
		bool complete = need > 0;
		if (need > 0) {
			if (i + 1 < n && p_bytes[i + 1] >= lo && p_bytes[i + 1] <= hi) {
				good = 2;
				for (size_t k = 2; k <= need; k++) {
					if (i + k < n && (p_bytes[i + k] & 0xC0) == 0x80) {
						good = k + 1;
					} else {
						complete = false;
						break;
					}
				}
			} else {
				complete = false;
			}
		}
		if (complete) {
			out.append(reinterpret_cast<const char *>(p_bytes.data() + i), need + 1);
			i += need + 1;
		} else {
			out.append("\xEF\xBF\xBD");
			i += good;
		}
	}
	return out;
}

std::array<float, 4> grey(float p_value) {
	return { p_value, p_value, p_value, 1.0f };
}

std::array<float, 3> rgb565(uint16_t p_value) {
	return { float((p_value >> 11) & 31) / 31.0f, float((p_value >> 5) & 63) / 63.0f, float(p_value & 31) / 31.0f };
}

// A BC1 colour block; p_punch_through lets the three-colour mode make transparent black.
void colour_block(const uint8_t *p_block, bool p_punch_through, std::array<std::array<float, 4>, 16> &r_texels) {
	const uint16_t a = uint16_t(p_block[0] | (p_block[1] << 8));
	const uint16_t b = uint16_t(p_block[2] | (p_block[3] << 8));
	const auto ca = rgb565(a);
	const auto cb = rgb565(b);
	auto mix = [&](float p_wa, float p_wb) {
		return std::array<float, 4>{ ca[0] * p_wa + cb[0] * p_wb, ca[1] * p_wa + cb[1] * p_wb, ca[2] * p_wa + cb[2] * p_wb, 1.0f };
	};
	std::array<std::array<float, 4>, 4> palette;
	palette[0] = { ca[0], ca[1], ca[2], 1.0f };
	palette[1] = { cb[0], cb[1], cb[2], 1.0f };
	if (a > b || !p_punch_through) {
		palette[2] = mix(2.0f / 3.0f, 1.0f / 3.0f);
		palette[3] = mix(1.0f / 3.0f, 2.0f / 3.0f);
	} else {
		palette[2] = mix(0.5f, 0.5f);
		palette[3] = { 0.0f, 0.0f, 0.0f, 0.0f };
	}
	const uint32_t indices = uint32_t(p_block[4]) | (uint32_t(p_block[5]) << 8) | (uint32_t(p_block[6]) << 16) |
			(uint32_t(p_block[7]) << 24);
	for (int at = 0; at < 16; at++) {
		r_texels[at] = palette[(indices >> (2 * at)) & 3];
	}
}

// A BC4 block, or the alpha half of a BC3 one.
void alpha_block(const uint8_t *p_block, std::array<float, 16> &r_alpha) {
	const float a = float(p_block[0]) / 255.0f;
	const float b = float(p_block[1]) / 255.0f;
	std::array<float, 8> palette;
	for (int at = 0; at < 8; at++) {
		if (at == 0) {
			palette[at] = a;
		} else if (at == 1) {
			palette[at] = b;
		} else if (p_block[0] > p_block[1]) {
			palette[at] = (a * float(8 - at) + b * float(at - 1)) / 7.0f;
		} else if (at == 6) {
			palette[at] = 0.0f;
		} else if (at == 7) {
			palette[at] = 1.0f;
		} else {
			palette[at] = (a * float(6 - at) + b * float(at - 1)) / 5.0f;
		}
	}
	uint64_t bits = 0;
	for (int k = 0; k < 6; k++) {
		bits |= uint64_t(p_block[2 + k]) << (8 * k);
	}
	for (int at = 0; at < 16; at++) {
		r_alpha[at] = palette[(bits >> (3 * at)) & 7];
	}
}

} // namespace

PixelFormat texture_pixel_format(TextureFormat p_format) {
	switch (p_format) {
		case TextureFormat::Dxt1:
		case TextureFormat::NormalDxt1:
			return PixelFormat::Bc1;
		case TextureFormat::Dxt3:
			return PixelFormat::Bc2;
		case TextureFormat::Dxt5:
		case TextureFormat::NormalDxt5:
		case TextureFormat::NormalDxt5Rga:
			return PixelFormat::Bc3;
		case TextureFormat::Dxt5A:
			return PixelFormat::Bc4;
		case TextureFormat::Dxn:
		case TextureFormat::NormalDxn:
			return PixelFormat::Bc5;
		case TextureFormat::Argb8888:
			return PixelFormat::Rgba8;
		case TextureFormat::L8:
			return PixelFormat::L8;
		case TextureFormat::L16:
			return PixelFormat::L16;
		default:
			return PixelFormat::None;
	}
}

bool texture_format_is_normal_map(TextureFormat p_format) {
	return p_format == TextureFormat::NormalDxn || p_format == TextureFormat::NormalDxt1 || p_format == TextureFormat::NormalDxt5 ||
			p_format == TextureFormat::NormalDxt5Rga;
}

int texture_block_bytes(TextureFormat p_format) {
	switch (p_format) {
		case TextureFormat::Dxt1:
		case TextureFormat::NormalDxt1:
		case TextureFormat::Dxt5A:
			return 8;
		case TextureFormat::Dxt3:
		case TextureFormat::Dxt5:
		case TextureFormat::Dxn:
		case TextureFormat::NormalDxn:
		case TextureFormat::NormalDxt5:
		case TextureFormat::NormalDxt5Rga:
			return 16;
		default:
			return 0;
	}
}

int texture_pixel_bytes(TextureFormat p_format) {
	switch (p_format) {
		case TextureFormat::L8:
			return 1;
		case TextureFormat::Rgb565:
		case TextureFormat::Argb1555:
		case TextureFormat::Argb4444:
		case TextureFormat::L16:
		case TextureFormat::R16F:
		case TextureFormat::Rg8:
		case TextureFormat::D16:
			return 2;
		case TextureFormat::Rgb888:
			return 3;
		case TextureFormat::Argb8888:
		case TextureFormat::R32F:
		case TextureFormat::Gr16:
		case TextureFormat::Gr16F:
		case TextureFormat::A2R10G10B10:
		case TextureFormat::D24S8:
		case TextureFormat::D24FS8:
		case TextureFormat::D32F:
			return 4;
		case TextureFormat::Abgr16:
		case TextureFormat::Abgr16F:
		case TextureFormat::Gr32F:
			return 8;
		case TextureFormat::Abgr32F:
		case TextureFormat::Abgr32:
			return 16;
		default:
			return 0;
	}
}

int64_t texture_row_pitch(TextureFormat p_format, uint32_t p_width) {
	const int64_t width = p_width < 1 ? 1 : p_width;
	const int block = texture_block_bytes(p_format);
	if (block > 0) {
		return (width + 3) / 4 * block;
	}
	const int pixel = texture_pixel_bytes(p_format);
	if (pixel == 0) {
		return -1;
	}
	return (width * pixel + 3) / 4 * 4;
}

int64_t texture_image_bytes(TextureFormat p_format, uint32_t p_width, uint32_t p_height) {
	const int64_t height = p_height < 1 ? 1 : p_height;
	const int64_t rows = texture_block_bytes(p_format) > 0 ? (height + 3) / 4 : height;
	const int64_t pitch = texture_row_pitch(p_format, p_width);
	return pitch < 0 ? -1 : pitch * rows;
}

ResError DxTexture::parse(Bytes p_res, DxTexture &r_texture) {
	r_texture = DxTexture();
	Reader r(p_res);
	if (!r.has(0, kHeaderBytes)) {
		return r.error();
	}
	const uint32_t version = r.u32(0x00);
	if (version != 10) {
		return ResError::unsupported("texture version", version);
	}
	const uint32_t kind = r.u32(0x04);
	if (kind > 6) {
		return ResError::unsupported("texture type", kind);
	}
	const uint32_t format = r.u32(0x08);
	if (format > 30) {
		return ResError::unsupported("texture format", format);
	}
	const uint8_t mip_count = r.u8(0x1A);
	if (mip_count > kMipSlots) {
		return ResError::unsupported("mip count", mip_count);
	}
	DxTexture &t = r_texture;
	t.kind = TextureKind(kind);
	t.format = TextureFormat(format);
	for (uint64_t mip = 0; mip < mip_count; mip++) {
		t.mip_sizes.push_back(r.u32(0x2C + 4 * mip));
	}
	const Bytes group = r.slice(0x70, 16);
	size_t group_end = 0;
	while (group_end < group.size() && group[group_end] != 0) {
		group_end++;
	}
	t.flags = r.u32(0x0C);
	t.width = r.u16(0x10);
	t.height = r.u16(0x12);
	t.depth = r.u16(0x14);
	t.slice_count = r.u16(0x16);
	t.mip_count = mip_count;
	t.mip_base = r.u8(0x1B);
	t.chunk_id = r.chunk_id(0x1C);
	t.mip_chain_size = r.u32(0x68);
	t.name_hash = r.u32(0x6C);
	t.texture_group = lossy_utf8(group.first(group_end));
	return r.error();
}

uint32_t DxTexture::faces() const {
	switch (kind) {
		case TextureKind::Cube:
			return 6;
		case TextureKind::Array2D:
		case TextureKind::Array1D:
		case TextureKind::CubeArray:
			return slice_count < 1 ? 1u : uint32_t(slice_count);
		default:
			return 1;
	}
}

void DxTexture::mip_dimensions(uint8_t p_mip, uint32_t &r_width, uint32_t &r_height, uint32_t &r_depth) const {
	auto shrink = [p_mip](uint16_t p_size) {
		const uint32_t v = p_mip < 32 ? uint32_t(p_size) >> p_mip : 0;
		return v < 1 ? 1u : v;
	};
	r_width = shrink(width);
	r_height = shrink(height);
	r_depth = kind == TextureKind::Volume ? shrink(depth) : 1;
}

uint64_t DxTexture::logical_offset(uint8_t p_first_mip) const {
	uint64_t sum = 0;
	for (size_t mip = 0; mip < p_first_mip && mip < mip_sizes.size(); mip++) {
		sum += mip_sizes[mip];
	}
	return sum;
}

int DxTexture::first_mip_at(uint64_t p_logical_offset) const {
	for (int mip = 0; mip <= mip_count; mip++) {
		if (logical_offset(uint8_t(mip)) == p_logical_offset) {
			return mip;
		}
	}
	return -1;
}

ResError DxTexture::split_mips(Bytes p_chunk, uint64_t p_logical_offset, std::vector<MipSlice> &r_mips) const {
	r_mips.clear();
	const int first_mip = first_mip_at(p_logical_offset);
	if (first_mip < 0) {
		return ResError::malformed("logical offset between mips", p_logical_offset);
	}
	uint64_t chain = 0;
	for (uint32_t size : mip_sizes) {
		chain += size;
	}
	Reader r(p_chunk);
	for (uint32_t face = 0; face < faces(); face++) {
		uint64_t at = uint64_t(face) * chain;
		for (uint8_t mip = 0; mip < mip_count; mip++) {
			const uint64_t size = mip_sizes[mip];
			if (mip >= first_mip) {
				MipSlice slice;
				slice.face = face;
				slice.mip = mip;
				mip_dimensions(mip, slice.width, slice.height, slice.depth);
				const int64_t pitch = texture_row_pitch(format, slice.width);
				slice.row_pitch = pitch < 0 ? 0 : uint32_t(pitch);
				slice.data = r.slice(at - p_logical_offset, size);
				if (!r.ok()) {
					return r.error();
				}
				r_mips.push_back(slice);
			}
			at += size;
		}
	}
	return ResError();
}

bool MipSlice::texels(TextureFormat p_format, std::vector<std::array<float, 4>> &r_texels) const {
	r_texels.clear();
	const size_t w = width;
	const size_t h = height;
	const int64_t pitch_signed = texture_row_pitch(p_format, width);
	if (pitch_signed < 0) {
		return false;
	}
	const size_t pitch = size_t(pitch_signed);
	auto row = [&](size_t p_y, size_t p_bytes) -> const uint8_t * {
		const size_t at = p_y * pitch;
		return at <= data.size() && p_bytes <= data.size() - at ? data.data() + at : nullptr;
	};
	// Every row the decode reads must be backed by `data` before the output is allocated: the
	// dimensions come from untrusted u16 header fields (65535 x 65535 would be ~68 GB of texels).
	auto backed = [&](size_t p_rows, size_t p_row_bytes) {
		return p_rows == 0 || row(p_rows - 1, p_row_bytes) != nullptr;
	};
	switch (p_format) {
		case TextureFormat::L8:
		case TextureFormat::L16:
		case TextureFormat::Argb8888: {
			const size_t size = size_t(texture_pixel_bytes(p_format));
			if (!backed(h, w * size)) {
				return false;
			}
			std::vector<std::array<float, 4>> out(w * h, std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f });
			for (size_t y = 0; y < h; y++) {
				const uint8_t *bytes = row(y, w * size);
				if (bytes == nullptr) {
					return false;
				}
				for (size_t x = 0; x < w; x++) {
					const uint8_t *t = bytes + x * size;
					if (p_format == TextureFormat::L8) {
						out[y * w + x] = grey(float(t[0]) / 255.0f);
					} else if (p_format == TextureFormat::L16) {
						out[y * w + x] = grey(float(uint16_t(t[0] | (t[1] << 8))) / 65535.0f);
					} else {
						out[y * w + x] = { float(t[0]) / 255.0f, float(t[1]) / 255.0f, float(t[2]) / 255.0f, float(t[3]) / 255.0f };
					}
				}
			}
			r_texels = std::move(out);
			return true;
		}
		case TextureFormat::Dxt1:
		case TextureFormat::Dxt5:
		case TextureFormat::Dxt5A: {
			const size_t block_bytes = size_t(texture_block_bytes(p_format));
			const size_t blocks_x = (w + 3) / 4;
			if (!backed((h + 3) / 4, blocks_x * block_bytes)) {
				return false;
			}
			std::vector<std::array<float, 4>> out(w * h, std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f });
			for (size_t by = 0; by < (h + 3) / 4; by++) {
				const uint8_t *blocks = row(by, blocks_x * block_bytes);
				if (blocks == nullptr) {
					return false;
				}
				for (size_t bx = 0; bx < blocks_x; bx++) {
					const uint8_t *block = blocks + bx * block_bytes;
					std::array<std::array<float, 4>, 16> texels;
					if (p_format == TextureFormat::Dxt1) {
						colour_block(block, true, texels);
					} else if (p_format == TextureFormat::Dxt5) {
						std::array<float, 16> alpha;
						alpha_block(block, alpha);
						colour_block(block + 8, false, texels);
						for (int k = 0; k < 16; k++) {
							texels[k][3] = alpha[k];
						}
					} else {
						std::array<float, 16> alpha;
						alpha_block(block, alpha);
						for (int k = 0; k < 16; k++) {
							texels[k] = grey(alpha[k]);
						}
					}
					for (size_t at = 0; at < 16; at++) {
						const size_t x = bx * 4 + at % 4;
						const size_t y = by * 4 + at / 4;
						if (x < w && y < h) {
							out[y * w + x] = texels[at];
						}
					}
				}
			}
			r_texels = std::move(out);
			return true;
		}
		default:
			return false;
	}
}

} // namespace bf3::core::res
