#include "bf3/render/texture_prep.h"

#include <algorithm>
#include <cstring>

namespace bf3::render {

using core::res::LoadedTexture;
using core::res::MipSlice;
using core::res::PixelFormat;
using core::res::TextureFormat;
using core::res::TextureKind;

const char *gpu_format_name(GpuFormat p_format) {
	switch (p_format) {
		case GpuFormat::Bc1:
			return "BC1";
		case GpuFormat::Bc2:
			return "BC2";
		case GpuFormat::Bc3:
			return "BC3";
		case GpuFormat::Bc4:
			return "BC4";
		case GpuFormat::Bc5:
			return "BC5";
		case GpuFormat::Rgba8:
			return "RGBA8";
		case GpuFormat::L8:
			return "L8";
		case GpuFormat::Rg8:
			return "RG8";
	}
	return "?";
}

bool gpu_format_is_block(GpuFormat p_format) {
	return gpu_format_block_bytes(p_format) != 0;
}

int gpu_format_block_bytes(GpuFormat p_format) {
	switch (p_format) {
		case GpuFormat::Bc1:
		case GpuFormat::Bc4:
			return 8;
		case GpuFormat::Bc2:
		case GpuFormat::Bc3:
		case GpuFormat::Bc5:
			return 16;
		default:
			return 0;
	}
}

int gpu_format_pixel_bytes(GpuFormat p_format) {
	switch (p_format) {
		case GpuFormat::Rgba8:
			return 4;
		case GpuFormat::L8:
			return 1;
		case GpuFormat::Rg8:
			return 2;
		default:
			return 0;
	}
}

uint32_t godot_mip_levels(GpuFormat p_format, uint32_t p_width, uint32_t p_height) {

	(void)p_format;
	const uint32_t min_side = 1u;
	uint32_t w = std::max(p_width, 1u), h = std::max(p_height, 1u), levels = 1;
	while (!(w == min_side && h == min_side) && levels < 32) {
		w = std::max(min_side, w >> 1);
		h = std::max(min_side, h >> 1);
		++levels;
	}
	return levels;
}

uint64_t level_bytes(GpuFormat p_format, uint32_t p_width, uint32_t p_height) {
	if (gpu_format_is_block(p_format)) {
		return uint64_t((std::max(p_width, 1u) + 3) / 4) * ((std::max(p_height, 1u) + 3) / 4) * gpu_format_block_bytes(p_format);
	}
	return uint64_t(std::max(p_width, 1u)) * std::max(p_height, 1u) * gpu_format_pixel_bytes(p_format);
}

std::array<uint8_t, 16> bc_color_green(const uint8_t *p_block) {
	const uint16_t c0 = uint16_t(p_block[0] | (p_block[1] << 8));
	const uint16_t c1 = uint16_t(p_block[2] | (p_block[3] << 8));
	const int g0 = ((c0 >> 5) & 63), g1 = ((c1 >> 5) & 63);
	const int e0 = (g0 << 2) | (g0 >> 4), e1 = (g1 << 2) | (g1 >> 4);
	const int palette[4] = { e0, e1, (2 * e0 + e1 + 1) / 3, (e0 + 2 * e1 + 1) / 3 };
	const uint32_t bits = uint32_t(p_block[4]) | (uint32_t(p_block[5]) << 8) | (uint32_t(p_block[6]) << 16) | (uint32_t(p_block[7]) << 24);
	std::array<uint8_t, 16> out{};
	for (int i = 0; i < 16; ++i) {
		out[size_t(i)] = uint8_t(palette[(bits >> (2 * i)) & 3]);
	}
	return out;
}

std::array<uint8_t, 16> bc4_values(const uint8_t *p_block) {
	const int e0 = p_block[0], e1 = p_block[1];
	int palette[8] = { e0, e1, 0, 0, 0, 0, 0, 0 };
	if (e0 > e1) {
		for (int i = 1; i <= 6; ++i) {
			palette[i + 1] = ((7 - i) * e0 + i * e1 + 3) / 7;
		}
	} else {
		for (int i = 1; i <= 4; ++i) {
			palette[i + 1] = ((5 - i) * e0 + i * e1 + 2) / 5;
		}
		palette[6] = 0;
		palette[7] = 255;
	}
	uint64_t bits = 0;
	for (int i = 0; i < 6; ++i) {
		bits |= uint64_t(p_block[2 + i]) << (8 * i);
	}
	std::array<uint8_t, 16> out{};
	for (int i = 0; i < 16; ++i) {
		out[size_t(i)] = uint8_t(palette[(bits >> (3 * i)) & 7]);
	}
	return out;
}

std::array<uint8_t, 8> bc4_encode(const std::array<uint8_t, 16> &p_values) {
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

std::array<uint8_t, 16> bc1_luminance(const uint8_t *p_block) {
	const uint16_t c0 = uint16_t(p_block[0] | (p_block[1] << 8));
	const uint16_t c1 = uint16_t(p_block[2] | (p_block[3] << 8));
	const auto brightest = [](uint16_t p_c) {
		const int r = (p_c >> 11) & 31, g = (p_c >> 5) & 63, b = p_c & 31;
		return std::max({ (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2) });
	};
	const int e0 = brightest(c0), e1 = brightest(c1);
	const int palette[4] = { e0, e1, c0 > c1 ? (2 * e0 + e1 + 1) / 3 : (e0 + e1) / 2,
		c0 > c1 ? (e0 + 2 * e1 + 1) / 3 : 0 };
	const uint32_t bits = uint32_t(p_block[4]) | (uint32_t(p_block[5]) << 8) | (uint32_t(p_block[6]) << 16) |
			(uint32_t(p_block[7]) << 24);
	std::array<uint8_t, 16> out{};
	for (int i = 0; i < 16; ++i) {
		out[size_t(i)] = uint8_t(palette[(bits >> (2 * i)) & 3]);
	}
	return out;
}

std::array<uint8_t, 16> bc1_mask_to_bc3(const uint8_t *p_bc1_block) {
	std::array<uint8_t, 16> out{};
	const std::array<uint8_t, 8> alpha = bc4_encode(bc1_luminance(p_bc1_block));
	std::memcpy(out.data(), alpha.data(), 8);
	out[8] = out[9] = out[10] = out[11] = 0xFF;
	return out;
}

std::array<uint8_t, 16> bc3_normal_to_bc5(const uint8_t *p_bc3_block) {
	std::array<uint8_t, 16> out{};
	std::memcpy(out.data(), p_bc3_block, 8);
	const std::array<uint8_t, 8> g = bc4_encode(bc_color_green(p_bc3_block + 8));
	std::memcpy(out.data() + 8, g.data(), 8);
	return out;
}

namespace {

bool source_gpu_format(TextureFormat p_format, GpuFormat &r_format) {
	switch (core::res::texture_pixel_format(p_format)) {
		case PixelFormat::Bc1:
			r_format = GpuFormat::Bc1;
			return true;
		case PixelFormat::Bc2:
			r_format = GpuFormat::Bc2;
			return true;
		case PixelFormat::Bc3:
			r_format = GpuFormat::Bc3;
			return true;
		case PixelFormat::Bc4:
			r_format = GpuFormat::Bc4;
			return true;
		case PixelFormat::Bc5:
			r_format = GpuFormat::Bc5;
			return true;
		case PixelFormat::Rgba8:
			r_format = GpuFormat::Rgba8;
			return true;
		case PixelFormat::L8:
			r_format = GpuFormat::L8;
			return true;
		default:
			return false;
	}
}

bool level_data(const MipSlice &p_mip, GpuFormat p_format, std::vector<uint8_t> &r_out) {
	const uint64_t need = level_bytes(p_format, p_mip.width, p_mip.height);
	if (gpu_format_is_block(p_format)) {
		if (p_mip.data.size() < need) {
			return false;
		}
		r_out.assign(p_mip.data.begin(), p_mip.data.begin() + std::ptrdiff_t(need));
		return true;
	}
	const uint64_t row = uint64_t(p_mip.width) * gpu_format_pixel_bytes(p_format);
	const uint64_t pitch = std::max<uint64_t>(p_mip.row_pitch, row);
	if (p_mip.height == 0 || p_mip.data.size() < pitch * (p_mip.height - 1) + row) {
		return false;
	}
	r_out.resize(size_t(need));
	for (uint32_t y = 0; y < p_mip.height; ++y) {
		std::memcpy(r_out.data() + y * row, p_mip.data.data() + y * pitch, size_t(row));
	}
	return true;
}

std::vector<uint8_t> shrink_level(const std::vector<uint8_t> &p_level, GpuFormat p_format, uint32_t p_w, uint32_t p_h, uint32_t p_nw, uint32_t p_nh) {
	std::vector<uint8_t> out(size_t(level_bytes(p_format, p_nw, p_nh)));
	if (gpu_format_is_block(p_format)) {
		const size_t bb = size_t(gpu_format_block_bytes(p_format));
		for (size_t at = 0; at + bb <= out.size(); at += bb) {
			std::memcpy(out.data() + at, p_level.data(), bb);
		}
		return out;
	}
	const size_t px = size_t(gpu_format_pixel_bytes(p_format));
	for (uint32_t y = 0; y < p_nh; ++y) {
		for (uint32_t x = 0; x < p_nw; ++x) {
			const uint32_t sx = std::min(p_w - 1, x * 2), sy = std::min(p_h - 1, y * 2);
			std::memcpy(out.data() + (size_t(y) * p_nw + x) * px, p_level.data() + (size_t(sy) * p_w + sx) * px, px);
		}
	}
	return out;
}

}

core::Result<PreparedTexture> prepare_texture(const LoadedTexture &p_texture, TextureRole p_role) {
	const core::res::DxTexture &tex = p_texture.texture;
	const std::string where = "texture " + p_texture.res.name;
	if (tex.kind != TextureKind::Texture2D) {
		return core::make_error(where + ": not a 2D texture (kind " + std::to_string(int(tex.kind)) + ")");
	}
	PreparedTexture out;
	out.name = p_texture.res.name;
	out.source_format = tex.format;
	out.srgb = tex.is_srgb();
	out.role = p_role;
	out.width = tex.width;
	out.height = tex.height;
	GpuFormat stored;
	if (!source_gpu_format(tex.format, stored)) {
		return core::make_error(where + ": format " + std::to_string(int(tex.format)) + " is not uploaded");
	}

	const bool transcode = p_role == TextureRole::Normal && tex.format == TextureFormat::NormalDxt5;
	const bool mask = p_role == TextureRole::Mask && stored == GpuFormat::Bc1;
	out.format = transcode ? GpuFormat::Bc5 : mask ? GpuFormat::Bc3 : stored;

	std::vector<MipSlice> mips;
	const core::res::ResError e = p_texture.mips(mips);
	if (!e.ok()) {
		return core::make_error(where + ": " + e.message());
	}
	std::vector<std::vector<uint8_t>> levels;
	uint32_t w = 0, h = 0;
	for (const MipSlice &m : mips) {
		if (m.face != 0) {
			continue;
		}
		if (m.mip != levels.size()) {
			return core::make_error(where + ": face 0 mip " + std::to_string(m.mip) + " out of order");
		}
		std::vector<uint8_t> bytes;
		if (!level_data(m, stored, bytes)) {
			return core::make_error(where + ": mip " + std::to_string(m.mip) + " short");
		}
		if (transcode) {
			std::vector<uint8_t> bc5(bytes.size());
			for (size_t at = 0; at + 16 <= bytes.size(); at += 16) {
				const std::array<uint8_t, 16> b = bc3_normal_to_bc5(bytes.data() + at);
				std::memcpy(bc5.data() + at, b.data(), 16);
			}
			bytes = std::move(bc5);
		}
		if (mask) {
			std::vector<uint8_t> bc3(bytes.size() * 2);
			for (size_t at = 0; at + 8 <= bytes.size(); at += 8) {
				const std::array<uint8_t, 16> b = bc1_mask_to_bc3(bytes.data() + at);
				std::memcpy(bc3.data() + at * 2, b.data(), 16);
			}
			bytes = std::move(bc3);
		}
		levels.push_back(std::move(bytes));
		w = m.width;
		h = m.height;
	}
	if (levels.empty()) {
		return core::make_error(where + ": no mips");
	}
	out.stored_levels = uint32_t(levels.size());
	out.levels = levels.size() == 1 ? 1u : godot_mip_levels(out.format, out.width, out.height);

	uint32_t lw = std::max(w, 1u), lh = std::max(h, 1u);
	while (levels.size() < out.levels) {
		const uint32_t nw = std::max(1u, lw >> 1), nh = std::max(1u, lh >> 1);
		levels.push_back(shrink_level(levels.back(), out.format, lw, lh, nw, nh));
		lw = nw;
		lh = nh;
	}
	uint64_t total = 0;
	for (uint32_t i = 0; i < out.levels; ++i) {
		total += levels[i].size();
	}
	out.data.reserve(size_t(total));
	for (uint32_t i = 0; i < out.levels; ++i) {
		out.data.insert(out.data.end(), levels[i].begin(), levels[i].end());
	}

	uint64_t expect = 0;
	uint32_t gw = std::max(out.width, 1u), gh = std::max(out.height, 1u);
	for (uint32_t i = 0; i < out.levels; ++i) {
		expect += level_bytes(out.format, gw, gh);
		gw = std::max(1u, gw >> 1);
		gh = std::max(1u, gh >> 1);
	}
	if (expect != out.data.size()) {
		return core::make_error(where + ": chain of " + std::to_string(out.data.size()) + " bytes, Godot expects " + std::to_string(expect));
	}
	return out;
}

}
