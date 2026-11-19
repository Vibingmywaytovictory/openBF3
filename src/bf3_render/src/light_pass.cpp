#include "bf3/render/light_pass.h"

#include <bit>
#include <optional>
#include <vector>

namespace bf3::render {

namespace {

using dxbc::Instruction;
using dxbc::Op;
using dxbc::Operand;
using dxbc::OperandType;

constexpr std::string_view kNormalTarget = "g_gbufferTexture0";
constexpr uint8_t kSmoothnessLane = 3;
constexpr uint16_t kLoadMultisampled = 46;

struct Lane {
	uint32_t reg = 0;
	uint8_t lane = 0;
};

std::optional<uint8_t> single_lane(const Operand &p_dst) {
	const uint8_t mask = p_dst.write_mask();
	if (std::popcount(unsigned(mask)) != 1) {
		return std::nullopt;
	}
	return uint8_t(std::countr_zero(unsigned(mask)));
}

std::optional<Lane> destination(const Instruction &p_ins) {
	if (p_ins.operands.empty() || !p_ins.operands[0].is(OperandType::Temp)) {
		return std::nullopt;
	}
	const std::optional<uint8_t> lane = single_lane(p_ins.operands[0]);
	if (!lane) {
		return std::nullopt;
	}
	return Lane{ p_ins.operands[0].index[0], *lane };
}

bool reads(const Operand &p_src, const Lane &p_lane, uint8_t p_dst_lane) {
	return p_src.is(OperandType::Temp) && p_src.index[0] == p_lane.reg && p_src.lane(p_dst_lane) == p_lane.lane;
}

std::optional<float> immediate(const Operand &p_src, uint8_t p_dst_lane) {
	if (!p_src.is(OperandType::Immediate32)) {
		return std::nullopt;
	}
	return std::bit_cast<float>(p_src.imm[p_src.components == 1 ? 0 : p_dst_lane]);
}

std::optional<Lane> smoothness_lane(const dxbc::Program &p_program) {
	const dxbc::Binding *target = p_program.binding(dxbc::BindingType::Texture, kNormalTarget);
	if (target == nullptr) {
		return std::nullopt;
	}
	for (const Instruction &ins : p_program.code) {
		if ((!ins.is(Op::Ld) && ins.opcode != kLoadMultisampled) || ins.operands.size() < 3 ||
				!ins.operands[0].is(OperandType::Temp) || !ins.operands[2].is(OperandType::Resource) ||
				ins.operands[2].index[0] != target->bind_point) {
			continue;
		}
		const uint8_t mask = ins.operands[0].write_mask();
		for (uint8_t c = 0; c < 4; ++c) {
			if (((mask >> c) & 1u) != 0 && ins.operands[2].lane(c) == kSmoothnessLane) {
				return Lane{ ins.operands[0].index[0], c };
			}
		}
	}
	return std::nullopt;
}

std::vector<Lane> smoothness_lanes(const dxbc::Program &p_program, const Lane &p_loaded) {
	std::vector<Lane> lanes{ p_loaded };
	for (const Instruction &ins : p_program.code) {
		const std::optional<Lane> dst = destination(ins);
		if (!ins.is(Op::And) || !dst || ins.operands.size() != 3) {
			continue;
		}
		for (const Lane &l : lanes) {
			if (reads(ins.operands[1], l, dst->lane) || reads(ins.operands[2], l, dst->lane)) {
				lanes.push_back(*dst);
				break;
			}
		}
	}
	return lanes;
}

bool reads_any(const Operand &p_src, const std::vector<Lane> &p_lanes, uint8_t p_dst_lane) {
	for (const Lane &l : p_lanes) {
		if (reads(p_src, l, p_dst_lane)) {
			return true;
		}
	}
	return false;
}

std::optional<float> scaled(const Instruction &p_mul, const Lane &p_src) {
	if (!p_mul.is(Op::Mul) || p_mul.operands.size() != 3 || !p_mul.operands[0].is(OperandType::Temp)) {
		return std::nullopt;
	}
	for (uint8_t c = 0; c < 4; ++c) {
		if (((p_mul.operands[0].write_mask() >> c) & 1u) != 0 && reads(p_mul.operands[1], p_src, c)) {
			return immediate(p_mul.operands[2], c);
		}
	}
	return std::nullopt;
}

std::string where(size_t p_at) {
	return "instruction " + std::to_string(p_at);
}

}

core::Result<LightPassLobe> measure_light_pass(const dxbc::Program &p_program) {
	const std::optional<Lane> loaded = smoothness_lane(p_program);
	if (!loaded) {
		return core::make_error("no load of the G-buffer normal target's smoothness lane");
	}
	const std::vector<Lane> smooth = smoothness_lanes(p_program, *loaded);
	LightPassLobe lobe;
	const std::vector<Instruction> &code = p_program.code;
	std::optional<Lane> power;
	size_t at = 0;
	for (; at < code.size() && !power; ++at) {
		const Instruction &ins = code[at];
		const std::optional<Lane> dst = destination(ins);
		if (!ins.is(Op::Mad) || !dst || ins.operands.size() != 4 || !reads_any(ins.operands[1], smooth, dst->lane)) {
			continue;
		}
		const std::optional<float> scale = immediate(ins.operands[2], dst->lane);
		const std::optional<float> bias = immediate(ins.operands[3], dst->lane);
		if (!scale || !bias) {
			continue;
		}
		for (size_t e = at + 1; e < code.size(); ++e) {
			const std::optional<Lane> exp_dst = destination(code[e]);
			if (code[e].is(Op::Exp) && exp_dst && reads(code[e].operands[1], *dst, exp_dst->lane)) {
				lobe.power_scale = *scale;
				lobe.power_bias = *bias;
				power = exp_dst;
				at = e;
				break;
			}
		}
	}
	if (!power) {
		return core::make_error("no specular power exp2(a * smoothness + b)");
	}
	std::optional<Lane> norm;
	for (; at < code.size() && !norm; ++at) {
		const Instruction &ins = code[at];
		const std::optional<Lane> dst = destination(ins);
		if (ins.is(Op::Add) && dst && ins.operands.size() == 3 && reads(ins.operands[1], *power, dst->lane)) {
			if (const std::optional<float> add = immediate(ins.operands[2], dst->lane)) {
				lobe.norm_add = *add;
				norm = dst;
			}
		}
	}
	if (!norm) {
		return core::make_error("no normalisation power + c after the power");
	}
	std::optional<Lane> normalised;
	for (; at < code.size() && !normalised; ++at) {
		const Instruction &ins = code[at];
		const std::optional<Lane> dst = destination(ins);
		if (ins.is(Op::Mul) && dst && ins.operands.size() == 3 &&
				(reads(ins.operands[1], *norm, dst->lane) || reads(ins.operands[2], *norm, dst->lane))) {
			normalised = dst;
		}
	}
	for (; normalised && at < code.size() && lobe.norm_scale == 0.0f; ++at) {
		lobe.norm_scale = scaled(code[at], *normalised).value_or(0.0f);
	}
	if (lobe.norm_scale == 0.0f) {
		return core::make_error("no normalisation scale after " + where(at));
	}
	for (size_t i = 0; i < code.size() && lobe.envmap_mip_scale == 0.0f; ++i) {
		const std::optional<Lane> rough = destination(code[i]);
		if (!code[i].is(Op::Add) || !rough || code[i].operands.size() != 3 || !code[i].operands[1].negate ||
				!reads_any(code[i].operands[1], smooth, rough->lane) ||
				immediate(code[i].operands[2], rough->lane) != 1.0f) {
			continue;
		}
		for (size_t m = i + 1; m < code.size() && lobe.envmap_mip_scale == 0.0f; ++m) {
			lobe.envmap_mip_scale = scaled(code[m], *rough).value_or(0.0f);
		}
	}
	if (lobe.envmap_mip_scale == 0.0f) {
		return core::make_error("no envmap mip (1 - smoothness) * c");
	}
	return lobe;
}

}
