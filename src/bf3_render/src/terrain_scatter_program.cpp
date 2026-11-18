#include "bf3/core/res/res_loader.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/render/terrain_scatter.h"
#include "bf3/render/texture_prep.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>

namespace bf3::render {

namespace {

using dxbc::Op;
using dxbc::OperandType;
using Lanes = std::array<float, 4>;

constexpr std::string_view kConstants = "externalConstants";
constexpr std::string_view kMaskPrefix = "external_worldToMask";
constexpr std::string_view kMaskSuffix = "UvTransform";
constexpr uint32_t kMaxRegisters = 64;
constexpr uint32_t kAllBits = 0xFFFFFFFFu;

uint32_t bits(float p_value) {
	return std::bit_cast<uint32_t>(p_value);
}

float truth(bool p_value) {
	return std::bit_cast<float>(p_value ? kAllBits : 0u);
}

const std::array<float, 256> &srgb_table() {
	static const std::array<float, 256> table = [] {
		std::array<float, 256> t{};
		for (size_t i = 0; i < t.size(); ++i) {
			t[i] = srgb_to_linear(float(i) / 255.0f);
		}
		return t;
	}();
	return table;
}

Lanes sample_image(const ScatterImage &p_image, float p_u, float p_v, float p_lod) {
	if (p_image.mips.empty()) {
		return { 0.0f, 0.0f, 0.0f, 1.0f };
	}
	const size_t mip = size_t(std::clamp(std::lround(p_lod), 0L, long(p_image.mips.size()) - 1));
	const uint32_t w = std::max<uint32_t>(1, p_image.width >> mip);
	const uint32_t h = std::max<uint32_t>(1, p_image.height >> mip);
	const std::vector<uint8_t> &texels = p_image.mips[mip];
	const double x = double(p_u) * double(w) - 0.5;
	const double y = double(p_v) * double(h) - 0.5;
	const double fx0 = std::floor(x);
	const double fy0 = std::floor(y);
	const float tx = float(x - fx0);
	const float ty = float(y - fy0);
	const auto wrap = [](double p_i, uint32_t p_n) {
		const int64_t i = int64_t(p_i) % int64_t(p_n);
		return uint32_t(i < 0 ? i + int64_t(p_n) : i);
	};
	const uint32_t x0 = wrap(fx0, w), x1 = wrap(fx0 + 1.0, w);
	const uint32_t y0 = wrap(fy0, h), y1 = wrap(fy0 + 1.0, h);
	const std::array<float, 256> &linear = srgb_table();
	Lanes out{};
	for (size_t c = 0; c < 4; ++c) {
		const auto at = [&](uint32_t p_x, uint32_t p_y) {
			const uint8_t v = texels[(size_t(p_y) * w + p_x) * 4 + c];
			return p_image.srgb && c < 3 ? linear[v] : float(v) / 255.0f;
		};
		const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
		const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
		out[c] = top + (bottom - top) * ty;
	}
	return out;
}

bool supported(uint16_t p_opcode) {
	switch (Op(p_opcode)) {
		case Op::Add:
		case Op::And:
		case Op::Div:
		case Op::Dp2:
		case Op::Dp3:
		case Op::Dp4:
		case Op::Else:
		case Op::EndIf:
		case Op::Eq:
		case Op::Frc:
		case Op::Ge:
		case Op::If:
		case Op::Lt:
		case Op::Mad:
		case Op::Min:
		case Op::Max:
		case Op::Mov:
		case Op::Movc:
		case Op::Mul:
		case Op::Ne:
		case Op::Ret:
		case Op::Rsq:
		case Op::Sqrt:
		case Op::Sample:
		case Op::SampleL:
			return true;
		default:
			return false;
	}
}

size_t operand_count(uint16_t p_opcode) {
	switch (Op(p_opcode)) {
		case Op::Else:
		case Op::EndIf:
		case Op::Ret:
			return 0;
		case Op::If:
			return 1;
		case Op::Mov:
		case Op::Frc:
		case Op::Rsq:
		case Op::Sqrt:
			return 2;
		case Op::Mad:
		case Op::Movc:
		case Op::Sample:
			return 4;
		case Op::SampleL:
			return 5;
		default:
			return 3;
	}
}

}

core::Result<ScatterImage> load_scatter_image(const core::LevelMount &p_mount, const std::string &p_name) {
	core::Result<core::res::LoadedTexture> texture = core::res::load_texture(p_mount, p_name);
	if (!texture) {
		return texture.error();
	}
	if (texture->texture.kind != core::res::TextureKind::Texture2D) {
		return core::make_error("texture " + p_name + " is not a 2D texture");
	}
	std::vector<core::res::MipSlice> mips;
	const core::res::ResError e = texture->mips(mips);
	if (!e.ok()) {
		return core::make_error("texture " + p_name + ": " + e.message());
	}
	ScatterImage image;
	image.srgb = texture->texture.is_srgb();
	for (const core::res::MipSlice &mip : mips) {
		if (mip.face != 0 || mip.width > kScatterImageMaxSide || mip.height > kScatterImageMaxSide) {
			continue;
		}
		if (!image.mips.empty() &&
				(mip.width != std::max<uint32_t>(1, image.width >> image.mips.size()) ||
						mip.height != std::max<uint32_t>(1, image.height >> image.mips.size()))) {
			break;
		}
		std::vector<std::array<float, 4>> texels;
		if (!mip.texels(texture->texture.format, texels) || texels.size() != size_t(mip.width) * mip.height) {
			return core::make_error("texture " + p_name + " does not decode");
		}
		if (image.mips.empty()) {
			image.width = mip.width;
			image.height = mip.height;
		}
		std::vector<uint8_t> rgba(texels.size() * 4);
		for (size_t i = 0; i < texels.size(); ++i) {
			for (size_t c = 0; c < 4; ++c) {
				rgba[4 * i + c] = uint8_t(std::clamp(texels[i][c] * 255.0f + 0.5f, 0.0f, 255.0f));
			}
		}
		image.mips.push_back(std::move(rgba));
	}
	if (image.mips.empty()) {
		return core::make_error("texture " + p_name + " has no level of at most " +
				std::to_string(kScatterImageMaxSide) + " texels a side");
	}
	return image;
}

ScatterHeights ScatterHeights::from_grid(
		const core::res::HeightGrid &p_grid, const std::optional<std::array<float, 4>> &p_crop) {
	ScatterHeights out;
	out.origin = p_grid.origin_xz;
	out.spacing = p_grid.sample_spacing;
	out.width = uint32_t(p_grid.width);
	out.height = uint32_t(p_grid.height);
	out.drawn = { 0, 0, out.width > 0 ? out.width - 1 : 0, out.height > 0 ? out.height - 1 : 0 };
	if (p_crop && out.width > 1 && out.height > 1) {
		const auto index = [](float p_v, float p_o, float p_s, uint32_t p_n, bool p_up) {
			const double f = (double(p_v) - double(p_o)) / double(p_s);
			const double r = p_up ? std::ceil(f) : std::floor(f);
			if (!(r > 0.0)) {
				return uint32_t(0);
			}
			return r >= double(p_n - 1) ? p_n - 1 : uint32_t(r);
		};
		out.drawn = { index((*p_crop)[0], out.origin[0], out.spacing[0], out.width, false),
			index((*p_crop)[1], out.origin[1], out.spacing[1], out.height, false),
			index((*p_crop)[2], out.origin[0], out.spacing[0], out.width, true),
			index((*p_crop)[3], out.origin[1], out.spacing[1], out.height, true) };
	}
	float low = INFINITY, high = -INFINITY;
	for (float h : p_grid.heights) {
		if (std::isfinite(h)) {
			low = std::fmin(low, h);
			high = std::fmax(high, h);
		}
	}
	if (!(high >= low)) {
		low = high = 0.0f;
	}
	out.base = low;
	out.step = (high - low) / 65535.0f;
	out.samples.resize(p_grid.heights.size());
	for (size_t i = 0; i < p_grid.heights.size(); ++i) {
		const float h = p_grid.heights[i];
		out.samples[i] = out.step > 0.0f && std::isfinite(h)
				? uint16_t(std::clamp((h - low) / out.step + 0.5f, 0.0f, 65535.0f))
				: uint16_t(0);
	}
	out.holes = p_grid.holes;
	return out;
}

std::optional<float> ScatterHeights::surface(float p_x, float p_z) const {
	if (width < 2 || height < 2) {
		return std::nullopt;
	}
	const double gx = (double(p_x) - double(origin[0])) / double(spacing[0]);
	const double gz = (double(p_z) - double(origin[1])) / double(spacing[1]);
	if (!(gx >= double(drawn[0])) || !(gz >= double(drawn[1])) || !(gx < double(drawn[2])) ||
			!(gz < double(drawn[3]))) {
		return std::nullopt;
	}
	const uint32_t x = uint32_t(gx);
	const uint32_t z = uint32_t(gz);
	const size_t a = size_t(z) * width + x;
	const size_t b = a + 1;
	const size_t c = a + width;
	const size_t d = c + 1;
	if (holes[a] != 0 || holes[b] != 0 || holes[c] != 0 || holes[d] != 0) {
		return std::nullopt;
	}
	const float fx = float(gx - double(x));
	const float fz = float(gz - double(z));
	const float ha = metres(x, z), hb = metres(x + 1, z), hc = metres(x, z + 1), hd = metres(x + 1, z + 1);
	if (fx + fz <= 1.0f) {
		return ha + (hb - ha) * fx + (hc - ha) * fz;
	}
	return hd + (hc - hd) * (1.0f - fx) + (hb - hd) * (1.0f - fz);
}

float ScatterHeights::bilinear(float p_u, float p_v) const {
	if (width < 2 || height < 2) {
		return 0.0f;
	}
	const double gx = std::clamp(double(p_u) * double(width) - 0.5, 0.0, double(width - 1));
	const double gz = std::clamp(double(p_v) * double(height) - 0.5, 0.0, double(height - 1));
	const uint32_t x = std::min(uint32_t(gx), width - 2);
	const uint32_t z = std::min(uint32_t(gz), height - 2);
	const float fx = float(gx - double(x));
	const float fz = float(gz - double(z));
	const float top = metres(x, z) + (metres(x + 1, z) - metres(x, z)) * fx;
	const float bottom = metres(x, z + 1) + (metres(x + 1, z + 1) - metres(x, z + 1)) * fx;
	return top + (bottom - top) * fz;
}

core::Result<MaskScaleProgram> MaskScaleProgram::build(std::span<const uint8_t> p_dxbc,
		std::span<const uint8_t> p_layers, const core::res::TerrainMaskField &p_masks, const ScatterHeights &p_heights,
		const std::function<std::optional<int32_t>(uint32_t)> &p_image_of_slot) {
	core::Result<dxbc::Program> parsed = dxbc::parse(p_dxbc);
	if (!parsed) {
		return parsed.error();
	}
	MaskScaleProgram out;
	out.program_ = std::move(*parsed);
	const dxbc::Program &program = out.program_;
	if (program.kind != dxbc::kPixelProgram || program.temps > kMaxRegisters) {
		return core::make_error("not a pixel program of at most " + std::to_string(kMaxRegisters) + " temps");
	}
	for (const auto &[id, size] : program.indexable_temps) {
		if (id >= kMaxRegisters || size > kMaxRegisters) {
			return core::make_error("indexable temp x" + std::to_string(id) + " out of range");
		}
	}
	const dxbc::Binding *constants = program.binding(dxbc::BindingType::ConstantBuffer, kConstants);
	std::vector<int32_t> open;
	out.skip_.assign(program.code.size(), -1);
	for (size_t i = 0; i < program.code.size(); ++i) {
		const dxbc::Instruction &ins = program.code[i];
		if (!supported(ins.opcode)) {
			return core::make_error("uses " + dxbc::opcode_name(ins.opcode));
		}
		if (ins.operands.size() != operand_count(ins.opcode)) {
			return core::make_error(
					dxbc::opcode_name(ins.opcode) + " with " + std::to_string(ins.operands.size()) + " operands");
		}
		if (ins.is(Op::If)) {
			open.push_back(int32_t(i));
		} else if (ins.is(Op::Else)) {
			if (open.empty()) {
				return core::make_error("else without if");
			}
			out.skip_[size_t(open.back())] = int32_t(i);
			open.back() = int32_t(i);
		} else if (ins.is(Op::EndIf)) {
			if (open.empty()) {
				return core::make_error("endif without if");
			}
			out.skip_[size_t(open.back())] = int32_t(i);
			open.pop_back();
		}
		for (size_t o = 0; o < ins.operands.size(); ++o) {
			const dxbc::Operand &op = ins.operands[o];
			if (op.relative) {
				return core::make_error("relative addressing");
			}
			const bool destination = o == 0 && !ins.is(Op::If);
			if (op.is(OperandType::Temp)) {
				if (op.index[0] >= program.temps) {
					return core::make_error("temp out of range");
				}
			} else if (op.is(OperandType::IndexableTemp)) {
				const auto found = std::find_if(program.indexable_temps.begin(), program.indexable_temps.end(),
						[&](const std::pair<uint32_t, uint32_t> &p_x) { return p_x.first == op.index[0]; });
				if (found == program.indexable_temps.end() || op.index[1] >= found->second) {
					return core::make_error("indexable temp out of range");
				}
			} else if (op.is(OperandType::Output)) {
				if (op.index[0] >= 4) {
					return core::make_error("output o" + std::to_string(op.index[0]));
				}
			} else if (op.is(OperandType::Input)) {
				if (destination || op.index[0] != 1) {
					return core::make_error("input v" + std::to_string(op.index[0]));
				}
			} else if (op.is(OperandType::ConstantBuffer)) {
				if (destination || constants == nullptr || op.index[0] != constants->bind_point) {
					return core::make_error("constant buffer cb" + std::to_string(op.index[0]));
				}
			} else if (op.is(OperandType::Immediate32)) {
				if (destination) {
					return core::make_error("immediate destination");
				}
			} else if (op.is(OperandType::Resource) || op.is(OperandType::Sampler)) {
				if (!(ins.is(Op::Sample) || ins.is(Op::SampleL)) || o < 2) {
					return core::make_error("texture operand outside a sample");
				}
			} else {
				return core::make_error("operand type " + std::to_string(op.type));
			}
		}
	}
	if (!open.empty()) {
		return core::make_error("if without endif");
	}

	std::vector<uint8_t> masked;
	for (uint8_t l : p_layers) {
		if (l != 0) {
			masked.push_back(l);
		}
	}
	const std::array<float, 4> &bounds = p_masks.bounds();
	const float span_x = bounds[2] - bounds[0];
	const float span_z = bounds[3] - bounds[1];
	if (constants != nullptr) {
		out.constant_slot_ = constants->bind_point;
		for (const dxbc::ConstantBuffer &cb : program.constant_buffers) {
			if (cb.name != kConstants) {
				continue;
			}
			out.constants_.assign((cb.size + 15) / 16, std::array<float, 4>{});
			for (const dxbc::Variable &v : cb.variables) {
				std::array<float, 4> value{};
				if (v.name == "external_clipToWorldSpaceXzTransform" || v.name == "external_dynamicMaskUvScale") {
					value = { 1.0f, 1.0f, 0.0f, 0.0f };
				} else if (v.name == "external_worldToHeightfieldUvTransform") {
					value = { 1.0f / (p_heights.spacing[0] * float(p_heights.width)),
						1.0f / (p_heights.spacing[1] * float(p_heights.height)),
						(0.5f - p_heights.origin[0] / p_heights.spacing[0]) / float(p_heights.width),
						(0.5f - p_heights.origin[1] / p_heights.spacing[1]) / float(p_heights.height) };
				} else if (v.name == "external_heightfieldPixelSizeAndAspect") {
					value = { 1.0f / float(p_heights.width), 1.0f / float(p_heights.height),
						1.0f / p_heights.spacing[1], 1.0f / p_heights.spacing[0] };
				} else if (v.name == "external_heightmapWorldSizeY") {
					value = { 1.0f, 0.0f, 0.0f, 0.0f };
				} else if (v.name.starts_with(kMaskPrefix) && v.name.ends_with(kMaskSuffix)) {
					const std::string_view digits = std::string_view(v.name).substr(
							kMaskPrefix.size(), v.name.size() - kMaskPrefix.size() - kMaskSuffix.size());
					unsigned n = 0;
					if (std::from_chars(digits.data(), digits.data() + digits.size(), n).ec != std::errc() ||
							n >= masked.size()) {
						return core::make_error(v.name + " has no masked layer");
					}
					value = { 1.0f / span_x, 1.0f / span_z, float(masked[n]) - bounds[0] / span_x,
						-bounds[1] / span_z };
				} else {
					return core::make_error("unknown constant " + v.name);
				}
				const uint32_t reg = v.offset / 16;
				const uint32_t first = (v.offset % 16) / 4;
				for (uint32_t k = 0; k < v.size / 4 && first + k < 4 && reg < out.constants_.size(); ++k) {
					out.constants_[reg][first + k] = value[k];
				}
			}
		}
	}
	for (const dxbc::Binding &b : program.bindings) {
		if (b.type != uint32_t(dxbc::BindingType::Texture)) {
			continue;
		}
		Texture t;
		t.slot = b.bind_point;
		if (b.name == "texture_maskMap") {
			t.source = ScatterSource::Mask;
		} else if (b.name == "texture_dynamicMask") {
			t.source = ScatterSource::Zero;
		} else if (b.name == "texture_heightfieldAtlas") {
			t.source = ScatterSource::Height;
		} else if (const std::optional<int32_t> image = p_image_of_slot(b.bind_point); image) {
			if (*image < 0) {
				return core::make_error("texture t" + std::to_string(b.bind_point) + " (" + b.name + ") has no image");
			}
			t.source = ScatterSource::Image;
			t.image = uint32_t(*image);
		}
		out.textures_.push_back(t);
	}
	for (const dxbc::Instruction &ins : program.code) {
		if (!(ins.is(Op::Sample) || ins.is(Op::SampleL))) {
			continue;
		}
		const uint32_t slot = ins.operands[2].index[0];
		if (std::none_of(
					out.textures_.begin(), out.textures_.end(), [&](const Texture &p_t) { return p_t.slot == slot; })) {
			return core::make_error("samples unbound texture t" + std::to_string(slot));
		}
	}
	out.samples_.assign(program.code.size(), Sample{});
	return out;
}

void MaskScaleProgram::measure(const ScatterSamplers &p_samplers) {
	const std::array<float, 4> &bounds = p_samplers.masks->bounds();
	const float x = 0.5f * (bounds[0] + bounds[2]);
	const float z = 0.5f * (bounds[1] + bounds[3]);
	std::vector<std::array<float, 2>> here(program_.code.size()), east(program_.code.size()),
			south(program_.code.size());
	std::array<std::array<float, 4>, 4> outputs{};
	execute(p_samplers, x, z, 0.0f, true, &here, outputs);
	execute(p_samplers, x + 1.0f, z, 0.0f, true, &east, outputs);
	execute(p_samplers, x, z + 1.0f, 0.0f, true, &south, outputs);
	for (size_t i = 0; i < program_.code.size(); ++i) {
		const dxbc::Instruction &ins = program_.code[i];
		if (!ins.is(Op::Sample)) {
			continue;
		}
		const uint32_t slot = ins.operands[2].index[0];
		const auto bound =
				std::find_if(textures_.begin(), textures_.end(), [&](const Texture &p_t) { return p_t.slot == slot; });
		if (bound == textures_.end() || bound->source != ScatterSource::Image ||
				bound->image >= p_samplers.images.size()) {
			continue;
		}
		const ScatterImage &image = p_samplers.images[bound->image];
		const float w = float(image.width), h = float(image.height);
		const float along_x = std::hypot((east[i][0] - here[i][0]) * w, (east[i][1] - here[i][1]) * h);
		const float along_z = std::hypot((south[i][0] - here[i][0]) * w, (south[i][1] - here[i][1]) * h);
		const float rate = std::fmax(along_x, along_z);
		samples_[i].texels_per_metre = std::isfinite(rate) ? rate : 0.0f;
	}
}

MaskScaleOutput MaskScaleProgram::run(
		const ScatterSamplers &p_samplers, float p_x, float p_z, float p_footprint) const {
	std::array<std::array<float, 4>, 4> outputs{};
	execute(p_samplers, p_x, p_z, p_footprint, false, nullptr, outputs);
	MaskScaleOutput out;
	for (size_t i = 0; i < 8; ++i) {
		out.mask[i] = outputs[i / 4][i % 4];
		out.scale[i] = outputs[2 + i / 4][i % 4];
	}
	return out;
}

void MaskScaleProgram::execute(const ScatterSamplers &p_samplers, float p_x, float p_z, float p_footprint,
		bool p_every_branch, std::vector<std::array<float, 2>> *r_uvs,
		std::array<std::array<float, 4>, 4> &r_outputs) const {
	std::array<Lanes, kMaxRegisters> temps{};
	std::array<std::vector<Lanes>, kMaxRegisters> indexed;
	for (const auto &[id, size] : program_.indexable_temps) {
		indexed[id].assign(size, Lanes{});
	}
	const Lanes input = { p_x, p_z, 0.9f, 0.0f };
	const auto raw = [&](const dxbc::Operand &p_op) -> Lanes {
		if (p_op.is(OperandType::Temp)) {
			return temps[p_op.index[0]];
		}
		if (p_op.is(OperandType::IndexableTemp)) {
			return indexed[p_op.index[0]][p_op.index[1]];
		}
		if (p_op.is(OperandType::Input)) {
			return input;
		}
		if (p_op.is(OperandType::Output)) {
			return r_outputs[p_op.index[0]];
		}
		if (p_op.is(OperandType::ConstantBuffer)) {
			return p_op.index[1] < constants_.size() ? constants_[p_op.index[1]] : Lanes{};
		}
		return { std::bit_cast<float>(p_op.imm[0]), std::bit_cast<float>(p_op.imm[1]),
			std::bit_cast<float>(p_op.imm[2]), std::bit_cast<float>(p_op.imm[3]) };
	};
	const auto read = [&](const dxbc::Operand &p_op) -> Lanes {
		const Lanes r = raw(p_op);
		Lanes v{};
		for (uint8_t c = 0; c < 4; ++c) {
			float f = r[p_op.lane(c)];
			if (p_op.absolute) {
				f = std::fabs(f);
			}
			if (p_op.negate) {
				f = -f;
			}
			v[c] = f;
		}
		return v;
	};
	const auto write = [&](const dxbc::Instruction &p_ins, const Lanes &p_value) {
		const dxbc::Operand &d = p_ins.operands[0];
		Lanes *target = nullptr;
		if (d.is(OperandType::Temp)) {
			target = &temps[d.index[0]];
		} else if (d.is(OperandType::IndexableTemp)) {
			target = &indexed[d.index[0]][d.index[1]];
		} else {
			target = &r_outputs[d.index[0]];
		}
		const uint8_t mask = d.write_mask();
		for (uint8_t c = 0; c < 4; ++c) {
			if ((mask & (1u << c)) == 0) {
				continue;
			}
			float f = p_value[c];
			if (p_ins.saturate) {
				f = f > 0.0f ? std::fmin(f, 1.0f) : 0.0f;
			}
			(*target)[c] = f;
		}
	};
	const auto fetch = [&](size_t p_index, const dxbc::Instruction &p_ins, float p_u, float p_v) -> Lanes {
		const uint32_t slot = p_ins.operands[2].index[0];
		const auto bound =
				std::find_if(textures_.begin(), textures_.end(), [&](const Texture &p_t) { return p_t.slot == slot; });
		if (bound == textures_.end()) {
			return { 0.0f, 0.0f, 0.0f, 1.0f };
		}
		if (bound->source == ScatterSource::Mask) {
			const float layer = std::floor(p_u);
			if (!(layer >= 0.0f) || !(layer < float(p_samplers.masks->layers()))) {
				return { 0.0f, 0.0f, 0.0f, 1.0f };
			}
			const std::array<float, 4> &b = p_samplers.masks->bounds();
			const float m = p_samplers.masks->coverage(
									uint8_t(layer), b[0] + (p_u - layer) * (b[2] - b[0]), b[1] + p_v * (b[3] - b[1])) /
					255.0f;
			return { m, 0.0f, 0.0f, 1.0f };
		}
		if (bound->source == ScatterSource::Height) {
			return { p_samplers.heights->bilinear(p_u, p_v), 0.0f, 0.0f, 1.0f };
		}
		if (bound->source == ScatterSource::Image && bound->image < p_samplers.images.size()) {
			float lod = 0.0f;
			if (p_ins.is(Op::SampleL)) {
				lod = read(p_ins.operands[4])[0];
			} else {
				const float texels = samples_[p_index].texels_per_metre * p_footprint;
				lod = texels > 1.0f ? std::log2(texels) : 0.0f;
			}
			return sample_image(p_samplers.images[bound->image], p_u, p_v, lod);
		}
		return { 0.0f, 0.0f, 0.0f, 1.0f };
	};

	for (size_t i = 0; i < program_.code.size(); ++i) {
		const dxbc::Instruction &ins = program_.code[i];
		switch (Op(ins.opcode)) {
			case Op::Ret:
				return;
			case Op::If: {
				const bool taken = (bits(read(ins.operands[0])[0]) != 0) == ins.nonzero;
				if (!taken && !p_every_branch) {
					i = size_t(skip_[i]);
				}
				break;
			}
			case Op::Else:
				if (!p_every_branch) {
					i = size_t(skip_[i]);
				}
				break;
			case Op::EndIf:
				break;
			case Op::Mov:
				write(ins, read(ins.operands[1]));
				break;
			case Op::Frc:
			case Op::Rsq:
			case Op::Sqrt: {
				const Lanes a = read(ins.operands[1]);
				Lanes v{};
				for (size_t c = 0; c < 4; ++c) {
					v[c] = ins.is(Op::Frc)	  ? a[c] - std::floor(a[c])
							: ins.is(Op::Rsq) ? 1.0f / std::sqrt(a[c])
											  : std::sqrt(a[c]);
				}
				write(ins, v);
				break;
			}
			case Op::Add:
			case Op::Mul:
			case Op::Div:
			case Op::Min:
			case Op::Max:
			case Op::Lt:
			case Op::Ge:
			case Op::Eq:
			case Op::Ne:
			case Op::And: {
				const Lanes a = read(ins.operands[1]);
				const Lanes b = read(ins.operands[2]);
				Lanes v{};
				for (size_t c = 0; c < 4; ++c) {
					switch (Op(ins.opcode)) {
						case Op::Add:
							v[c] = a[c] + b[c];
							break;
						case Op::Mul:
							v[c] = a[c] * b[c];
							break;
						case Op::Div:
							v[c] = a[c] / b[c];
							break;
						case Op::Min:
							v[c] = std::fmin(a[c], b[c]);
							break;
						case Op::Max:
							v[c] = std::fmax(a[c], b[c]);
							break;
						case Op::Lt:
							v[c] = truth(a[c] < b[c]);
							break;
						case Op::Ge:
							v[c] = truth(a[c] >= b[c]);
							break;
						case Op::Eq:
							v[c] = truth(a[c] == b[c]);
							break;
						case Op::Ne:
							v[c] = truth(a[c] != b[c]);
							break;
						default:
							v[c] = std::bit_cast<float>(bits(a[c]) & bits(b[c]));
							break;
					}
				}
				write(ins, v);
				break;
			}
			case Op::Dp2:
			case Op::Dp3:
			case Op::Dp4: {
				const Lanes a = read(ins.operands[1]);
				const Lanes b = read(ins.operands[2]);
				const size_t n = ins.is(Op::Dp2) ? 2 : ins.is(Op::Dp3) ? 3 : 4;
				float sum = 0.0f;
				for (size_t c = 0; c < n; ++c) {
					sum += a[c] * b[c];
				}
				write(ins, { sum, sum, sum, sum });
				break;
			}
			case Op::Mad: {
				const Lanes a = read(ins.operands[1]);
				const Lanes b = read(ins.operands[2]);
				const Lanes c3 = read(ins.operands[3]);
				write(ins, { a[0] * b[0] + c3[0], a[1] * b[1] + c3[1], a[2] * b[2] + c3[2], a[3] * b[3] + c3[3] });
				break;
			}
			case Op::Movc: {
				const Lanes test = read(ins.operands[1]);
				const Lanes a = read(ins.operands[2]);
				const Lanes b = read(ins.operands[3]);
				Lanes v{};
				for (size_t c = 0; c < 4; ++c) {
					v[c] = bits(test[c]) != 0 ? a[c] : b[c];
				}
				write(ins, v);
				break;
			}
			case Op::Sample:
			case Op::SampleL: {
				const Lanes uv = read(ins.operands[1]);
				if (r_uvs != nullptr) {
					(*r_uvs)[i] = { uv[0], uv[1] };
				}
				const Lanes texel = fetch(i, ins, uv[0], uv[1]);
				const dxbc::Operand &resource = ins.operands[2];
				write(ins,
						{ texel[resource.lane(0)], texel[resource.lane(1)], texel[resource.lane(2)],
								texel[resource.lane(3)] });
				break;
			}
			default:
				break;
		}
	}
}

}
