#include "bf3/render/shader_translate.h"

#include "bf3/core/res/mesh_set.h"
#include "bf3/render/shader_database.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <set>

namespace bf3::render {

namespace {

using dxbc::Instruction;
using dxbc::Op;
using dxbc::Operand;
using dxbc::OperandType;

constexpr char kLanes[] = "xyzw";
constexpr uint32_t kSystemPosition = 1;
constexpr uint32_t kGbufferAlbedo = 1;
constexpr const char *kViewConstants = "viewConstants";
constexpr const char *kExternalPrefix = "external_";
constexpr const char *kDepthTexture = "texture_depthBufferTexture";

core::Error refuse(const std::string &p_why) {
	return core::make_error(p_why);
}

uint64_t lane_key(uint8_t p_type, uint32_t p_index, uint8_t p_lane) {
	return uint64_t(p_type) << 40 | uint64_t(p_index) << 8 | p_lane;
}

float as_float(uint32_t p_bits) {
	float f;
	std::memcpy(&f, &p_bits, 4);
	return f;
}

std::optional<std::string> float_text(float p_value) {
	if (!std::isfinite(p_value)) {
		return std::nullopt;
	}
	char buf[48];
	const auto r = std::to_chars(buf, buf + sizeof(buf), p_value);
	std::string s(buf, r.ptr);
	const size_t e = s.find('e');
	if (s.find('.') == std::string::npos) {
		s.insert(e == std::string::npos ? s.size() : e, ".0");
	}
	return p_value < 0.0f ? "(" + s + ")" : s;
}

bool componentwise(uint16_t p_opcode) {
	switch (Op(p_opcode)) {
		case Op::Add:
		case Op::And:
		case Op::DerivRtx:
		case Op::DerivRty:
		case Op::Div:
		case Op::Eq:
		case Op::Exp:
		case Op::Frc:
		case Op::Ge:
		case Op::Ieq:
		case Op::Log:
		case Op::Lt:
		case Op::Mad:
		case Op::Min:
		case Op::Max:
		case Op::Mov:
		case Op::Movc:
		case Op::Mul:
		case Op::Ne:
		case Op::RoundNe:
		case Op::RoundNi:
		case Op::RoundPi:
		case Op::RoundZ:
		case Op::Rsq:
		case Op::Sqrt:
			return true;
		default:
			return false;
	}
}

uint8_t dot_width(uint16_t p_opcode) {
	return p_opcode == uint16_t(Op::Dp2)	? 2
			: p_opcode == uint16_t(Op::Dp3) ? 3
			: p_opcode == uint16_t(Op::Dp4) ? 4
											: 0;
}

bool sampling(uint16_t p_opcode) {
	return p_opcode == uint16_t(Op::Sample) || p_opcode == uint16_t(Op::SampleL) || p_opcode == uint16_t(Op::SampleB) ||
			p_opcode == uint16_t(Op::SampleD);
}

bool comparison(uint16_t p_opcode) {
	return p_opcode == uint16_t(Op::Lt) || p_opcode == uint16_t(Op::Ge) || p_opcode == uint16_t(Op::Eq) ||
			p_opcode == uint16_t(Op::Ne);
}

enum class ConstantNames : uint8_t {
	Arrays,
	Locals,
};

class Emitter {
public:
	explicit Emitter(ConstantNames p_names) : names_(p_names) {}

	std::optional<uint32_t> depth_register;
	std::set<uint32_t> cubes;
	std::set<uint32_t> inputs;
	std::set<uint32_t> textures;
	std::map<uint32_t, uint32_t> buffers;

	core::Result<std::string> operand(const Operand &p_op) {
		std::string base;
		if (p_op.is(OperandType::Immediate32)) {
			base = "vec4(";
			for (uint8_t c = 0; c < 4; ++c) {
				const std::optional<std::string> f = float_text(as_float(p_op.imm[c]));
				if (!f) {
					return refuse("non-finite literal");
				}
				base += (c == 0 ? "" : ", ") + *f;
			}
			base += ")";
		} else {
			core::Result<std::string> r = reg(p_op);
			if (!r) {
				return r.error();
			}
			base = std::move(*r);
			if (p_op.components == 1) {
				base += ".xxxx";
			} else if (p_op.components != 4) {
				return refuse("operand without components");
			} else if (p_op.mode != 0) {
				base += ".";
				for (uint8_t c = 0; c < 4; ++c) {
					base += kLanes[p_op.swizzle[c]];
				}
			}
		}
		if (p_op.absolute) {
			base = "abs(" + base + ")";
		}
		if (p_op.negate) {
			base = "(-" + base + ")";
		}
		return base;
	}

	core::Result<std::string> statement(const Instruction &p_ins) {
		if (p_ins.offset != std::array<int8_t, 3>{}) {
			return refuse("sample offsets in " + dxbc::format_instruction(p_ins));
		}
		if (p_ins.is(Op::Else) || p_ins.is(Op::EndIf) || p_ins.is(Op::If)) {
			mask_lanes_.clear();
		}
		if (p_ins.is(Op::Else)) {
			return std::string("} else {");
		}
		if (p_ins.is(Op::EndIf)) {
			return std::string("}");
		}
		if (p_ins.is(Op::If) || p_ins.is(Op::Discard)) {
			if (p_ins.operands.size() != 1) {
				return refuse("instruction " + dxbc::format_instruction(p_ins));
			}
			core::Result<std::string> e = operand(p_ins.operands[0]);
			if (!e) {
				return e.error();
			}
			const std::string test = "((" + *e + ").x " + (p_ins.nonzero ? "!=" : "==") + " 0.0)";
			return p_ins.is(Op::If) ? "if " + test + " {" : "if " + test + " { discard; }";
		}
		if (p_ins.operands.empty()) {
			return refuse("instruction " + dxbc::format_instruction(p_ins));
		}
		std::vector<std::string> src;
		for (size_t s = 1; s < p_ins.operands.size(); ++s) {
			if (sampling(p_ins.opcode) && (s == 2 || s == 3)) {
				src.emplace_back();
				continue;
			}
			core::Result<std::string> e = operand(p_ins.operands[s]);
			if (!e) {
				return e.error();
			}
			src.push_back(std::move(*e));
		}
		const auto need = [&](size_t p_count) { return src.size() == p_count; };
		std::string value;
		const uint16_t op = p_ins.opcode;
		const uint8_t dot = dot_width(op);
		if (dot > 0 && need(2)) {
			const std::string part = dot == 2 ? ".xy" : dot == 3 ? ".xyz" : "";
			value = "vec4(dot((" + src[0] + ")" + part + ", (" + src[1] + ")" + part + "))";
		} else if (sampling(op) && p_ins.operands.size() > 2) {
			const Operand &res = p_ins.operands[2];
			if (!res.is(OperandType::Resource)) {
				return refuse("sample without a resource");
			}
			std::string swizzle = ".";
			for (uint8_t c = 0; c < 4; ++c) {
				swizzle += kLanes[res.lane(c)];
			}
			std::string tex = "t" + std::to_string(res.index[0]);
			if (depth_register && res.index[0] == *depth_register) {
				tex = "depth_tex";
			} else {
				textures.insert(res.index[0]);
			}
			const std::string at = cubes.count(res.index[0]) != 0 ? ").xyz" : ").xy";
			if (p_ins.is(Op::Sample) && need(3)) {
				value = "texture(" + tex + ", (" + src[0] + at + ")" + swizzle;
			} else if (p_ins.is(Op::SampleL) && need(4)) {
				value = "textureLod(" + tex + ", (" + src[0] + at + ", (" + src[3] + ").x)" + swizzle;
			} else if (p_ins.is(Op::SampleB) && need(4)) {
				value = "texture(" + tex + ", (" + src[0] + at + ", (" + src[3] + ").x)" + swizzle;
			} else if (p_ins.is(Op::SampleD) && need(5)) {
				value = "textureGrad(" + tex + ", (" + src[0] + at + ", (" + src[3] + at + ", (" + src[4] + at + ")" +
						swizzle;
			}
		} else if (p_ins.is(Op::Ieq) && need(2)) {
			const Operand &a = p_ins.operands[1], &b = p_ins.operands[2];
			const bool zero_b = b.is(OperandType::Immediate32) && b.imm == std::array<uint32_t, 4>{};
			const Operand &tested = zero_b ? a : b;
			const Operand &zero = zero_b ? b : a;
			if (!zero.is(OperandType::Immediate32) || zero.imm != std::array<uint32_t, 4>{} ||
					!holds_masks(tested, p_ins)) {
				return refuse("ieq of what is not a comparison mask and zero: " + dxbc::format_instruction(p_ins));
			}
			value = "vec4(equal(" + src[zero_b ? 0 : 1] + ", vec4(0.0)))";
		} else if (comparison(op) && need(2)) {
			const char *fn = p_ins.is(Op::Lt) ? "lessThan"
					: p_ins.is(Op::Ge)		  ? "greaterThanEqual"
					: p_ins.is(Op::Eq)		  ? "equal"
											  : "notEqual";
			value = std::string("vec4(") + fn + "(" + src[0] + ", " + src[1] + "))";
		} else if (p_ins.is(Op::And) && need(2)) {
			for (size_t s = 1; s <= 2; ++s) {
				if (!p_ins.operands[s].is(OperandType::Immediate32)) {
					continue;
				}
				for (uint32_t bits : p_ins.operands[s].imm) {
					const float f = as_float(bits);
					if (f != 0.0f && !std::isnormal(f)) {
						return refuse("and with the integer mask " + std::to_string(bits));
					}
				}
			}
			const bool first = p_ins.operands[1].is(OperandType::Immediate32);
			value = "mix(vec4(0.0), " + src[first ? 0 : 1] + ", notEqual(" + src[first ? 1 : 0] + ", vec4(0.0)))";
		} else if (p_ins.is(Op::Movc) && need(3)) {
			value = "mix(" + src[2] + ", " + src[1] + ", notEqual(" + src[0] + ", vec4(0.0)))";
		} else if (p_ins.is(Op::Mad) && need(3)) {
			value = "(" + src[0] + " * " + src[1] + " + " + src[2] + ")";
		} else if (need(2)) {
			static const std::map<uint16_t, const char *> kBinary = { { uint16_t(Op::Add), "+" },
				{ uint16_t(Op::Mul), "*" }, { uint16_t(Op::Div), "/" } };
			static const std::map<uint16_t, const char *> kCall = { { uint16_t(Op::Min), "min" },
				{ uint16_t(Op::Max), "max" } };
			if (const auto b = kBinary.find(op); b != kBinary.end()) {
				value = "(" + src[0] + " " + b->second + " " + src[1] + ")";
			} else if (const auto f = kCall.find(op); f != kCall.end()) {
				value = std::string(f->second) + "(" + src[0] + ", " + src[1] + ")";
			}
		} else if (need(1)) {
			static const std::map<uint16_t, const char *> kUnary = { { uint16_t(Op::Mov), "" },
				{ uint16_t(Op::Sqrt), "sqrt" }, { uint16_t(Op::Rsq), "inversesqrt" }, { uint16_t(Op::Exp), "exp2" },
				{ uint16_t(Op::Log), "log2" }, { uint16_t(Op::Frc), "fract" }, { uint16_t(Op::RoundNi), "floor" },
				{ uint16_t(Op::RoundPi), "ceil" }, { uint16_t(Op::RoundZ), "trunc" },
				{ uint16_t(Op::RoundNe), "roundEven" }, { uint16_t(Op::DerivRtx), "dFdx" },
				{ uint16_t(Op::DerivRty), "dFdy" } };
			if (const auto u = kUnary.find(op); u != kUnary.end()) {
				value = std::string(u->second) + "(" + src[0] + ")";
			}
		}
		if (value.empty()) {
			return refuse("instruction " + dxbc::format_instruction(p_ins));
		}
		if (p_ins.saturate) {
			value = "clamp(" + value + ", 0.0, 1.0)";
		}
		const Operand &d = p_ins.operands[0];
		if (!d.is(OperandType::Temp) && !d.is(OperandType::Output) && !d.is(OperandType::IndexableTemp)) {
			return refuse("destination " + dxbc::format_instruction(p_ins));
		}
		std::string mask;
		for (uint8_t c = 0; c < 4; ++c) {
			if ((d.write_mask() >> c) & 1u) {
				mask += kLanes[c];
			}
		}
		if (mask.empty()) {
			return std::string();
		}
		core::Result<std::string> r = reg(d);
		if (!r) {
			return r.error();
		}
		if (d.is(OperandType::Temp)) {
			const bool masks = comparison(op) || p_ins.is(Op::Ieq);
			for (uint8_t c = 0; c < 4; ++c) {
				if (((d.write_mask() >> c) & 1u) == 0) {
					continue;
				}
				if (masks) {
					mask_lanes_.insert(lane(d.index[0], c));
				} else {
					mask_lanes_.erase(lane(d.index[0], c));
				}
			}
		}
		return *r + "." + mask + " = (" + value + ")." + mask + ";";
	}

private:
	static uint64_t lane(uint32_t p_reg, uint8_t p_lane) { return uint64_t(p_reg) << 8 | p_lane; }

	bool holds_masks(const Operand &p_op, const Instruction &p_ins) const {
		if (!p_op.is(OperandType::Temp) || p_op.negate || p_op.absolute) {
			return false;
		}
		for (uint8_t c = 0; c < 4; ++c) {
			if (((p_ins.operands[0].write_mask() >> c) & 1u) != 0 &&
					mask_lanes_.count(lane(p_op.index[0], p_op.lane(c))) == 0) {
				return false;
			}
		}
		return true;
	}

	std::set<uint64_t> mask_lanes_;

	core::Result<std::string> reg(const Operand &p_op) {
		if (p_op.relative) {
			return refuse("relative addressing");
		}
		if (p_op.is(OperandType::Temp)) {
			return "r" + std::to_string(p_op.index[0]);
		}
		if (p_op.is(OperandType::Input)) {
			inputs.insert(p_op.index[0]);
			return "v" + std::to_string(p_op.index[0]);
		}
		if (p_op.is(OperandType::Output)) {
			return "o" + std::to_string(p_op.index[0]);
		}
		if (p_op.is(OperandType::IndexableTemp) && p_op.dims == 2) {
			return "x" + std::to_string(p_op.index[0]) + "[" + std::to_string(p_op.index[1]) + "]";
		}
		if (p_op.is(OperandType::ConstantBuffer) && p_op.dims == 2) {
			uint32_t &size = buffers[p_op.index[0]];
			size = std::max(size, p_op.index[1] + 1);
			return "cb" + std::to_string(p_op.index[0]) +
					(names_ == ConstantNames::Arrays ? "[" + std::to_string(p_op.index[1]) + "]"
													 : "_" + std::to_string(p_op.index[1]));
		}
		return refuse("operand type " + std::to_string(p_op.type));
	}

	ConstantNames names_;
};

struct Read {
	size_t operand = 0;
	uint8_t lane = 0;
};

std::vector<Read> reads_for(const Instruction &p_ins, uint8_t p_dest_lane, uint8_t p_coordinate_lanes = 2) {
	std::vector<Read> out;
	const uint8_t dot = dot_width(p_ins.opcode);
	if (componentwise(p_ins.opcode)) {
		for (size_t s = 1; s < p_ins.operands.size(); ++s) {
			out.push_back({ s, p_ins.operands[s].lane(p_dest_lane) });
		}
	} else if (dot > 0) {
		for (size_t s = 1; s < p_ins.operands.size(); ++s) {
			for (uint8_t l = 0; l < dot; ++l) {
				out.push_back({ s, p_ins.operands[s].lane(l) });
			}
		}
	} else if (sampling(p_ins.opcode)) {
		for (uint8_t l = 0; l < p_coordinate_lanes; ++l) {
			out.push_back({ 1, p_ins.operands[1].lane(l) });
		}
		if (p_ins.is(Op::SampleD)) {
			for (size_t s = 4; s < p_ins.operands.size(); ++s) {
				for (uint8_t l = 0; l < p_coordinate_lanes; ++l) {
					out.push_back({ s, p_ins.operands[s].lane(l) });
				}
			}
		} else if (p_ins.operands.size() > 4) {
			out.push_back({ 4, p_ins.operands[4].lane(0) });
		}
	}
	return out;
}

struct Origin {
	enum class Kind : uint8_t {
		Unknown,
		Element,
		Clip,
	};
	Kind kind = Kind::Unknown;
	uint32_t element = 0;
	uint8_t lane = 0;
	float scale = 1.0f;
	float offset = 0.0f;
	uint64_t id = 0;
};

class VertexOrigins {
public:
	VertexOrigins(const dxbc::Program &p_vs) {
		uint64_t next_id = 1;
		for (const Instruction &ins : p_vs.code) {
			if (ins.is(Op::Ret)) {
				break;
			}
			if (ins.operands.empty()) {
				continue;
			}
			const Operand &d = ins.operands[0];
			if (!d.is(OperandType::Temp) && !d.is(OperandType::Output)) {
				continue;
			}
			for (uint8_t c = 0; c < 4; ++c) {
				if (((d.mask >> c) & 1u) == 0) {
					continue;
				}
				Origin o = evaluate(p_vs, ins, c);
				if (o.id == 0) {
					o.id = next_id++;
				}
				values_[lane_key(d.type, d.index[0], c)] = o;
			}
		}
		for (const dxbc::SignatureElement &e : p_vs.outputs) {
			if (e.system_value != kSystemPosition) {
				continue;
			}
			for (uint8_t c = 0; c < 4; ++c) {
				const auto found = values_.find(lane_key(uint8_t(OperandType::Output), e.reg, c));
				if (found != values_.end()) {
					clip_ids_[found->second.id] = c;
				}
			}
		}
	}

	Origin output(uint32_t p_reg, uint8_t p_lane) const {
		const auto found = values_.find(lane_key(uint8_t(OperandType::Output), p_reg, p_lane));
		if (found == values_.end()) {
			return {};
		}
		Origin o = found->second;
		const auto clip = clip_ids_.find(o.id);
		if (o.kind == Origin::Kind::Unknown && clip != clip_ids_.end()) {
			o.kind = Origin::Kind::Clip;
			o.lane = clip->second;
		}
		return o;
	}

private:
	Origin source(const dxbc::Program &p_vs, const Operand &p_op, uint8_t p_lane) const {
		if (p_op.absolute) {
			return {};
		}
		Origin o;
		const uint8_t l = p_op.lane(p_lane);
		if (p_op.is(OperandType::Input)) {
			for (const dxbc::SignatureElement &e : p_vs.inputs) {
				if (e.reg == p_op.index[0] && ((e.mask >> l) & 1u) != 0 && e.name == "TEXCOORD" &&
						e.system_value == 0) {
					o.kind = Origin::Kind::Element;
					o.element = e.semantic_index;
					o.lane = l;
					o.id = lane_key(uint8_t(OperandType::Input), p_op.index[0], l) | (uint64_t(1) << 62);
				}
			}
		} else if (p_op.is(OperandType::Temp)) {
			const auto found = values_.find(lane_key(uint8_t(OperandType::Temp), p_op.index[0], l));
			if (found != values_.end()) {
				o = found->second;
			}
		} else {
			return {};
		}
		if (p_op.negate) {
			o.scale = -o.scale;
			o.offset = -o.offset;
		}
		return o;
	}

	static std::optional<float> literal(const Operand &p_op, uint8_t p_lane) {
		if (!p_op.is(OperandType::Immediate32)) {
			return std::nullopt;
		}
		float v = as_float(p_op.imm[p_op.lane(p_lane)]);
		if (p_op.absolute) {
			v = std::fabs(v);
		}
		return p_op.negate ? -v : v;
	}

	Origin evaluate(const dxbc::Program &p_vs, const Instruction &p_ins, uint8_t p_lane) const {
		const auto affine = [](Origin p_o, float p_scale, float p_offset) {
			if (p_o.kind != Origin::Kind::Element) {
				return Origin{};
			}
			p_o.scale *= p_scale;
			p_o.offset = p_o.offset * p_scale + p_offset;
			p_o.id = 0;
			return p_o;
		};
		const std::vector<Operand> &ops = p_ins.operands;
		if (p_ins.saturate) {
			return {};
		}
		if (p_ins.is(Op::Mov) && ops.size() == 2) {
			return source(p_vs, ops[1], p_lane);
		}
		if (p_ins.is(Op::Mul) && ops.size() == 3) {
			if (const std::optional<float> k = literal(ops[2], p_lane)) {
				return affine(source(p_vs, ops[1], p_lane), *k, 0.0f);
			}
			if (const std::optional<float> k = literal(ops[1], p_lane)) {
				return affine(source(p_vs, ops[2], p_lane), *k, 0.0f);
			}
		}
		if (p_ins.is(Op::Add) && ops.size() == 3) {
			if (const std::optional<float> k = literal(ops[2], p_lane)) {
				return affine(source(p_vs, ops[1], p_lane), 1.0f, *k);
			}
			if (const std::optional<float> k = literal(ops[1], p_lane)) {
				return affine(source(p_vs, ops[2], p_lane), 1.0f, *k);
			}
		}
		if (p_ins.is(Op::Mad) && ops.size() == 4) {
			const std::optional<float> add = literal(ops[3], p_lane);
			if (add) {
				if (const std::optional<float> k = literal(ops[2], p_lane)) {
					return affine(source(p_vs, ops[1], p_lane), *k, *add);
				}
				if (const std::optional<float> k = literal(ops[1], p_lane)) {
					return affine(source(p_vs, ops[2], p_lane), *k, *add);
				}
			}
		}
		return {};
	}

	std::map<uint64_t, Origin> values_;
	std::map<uint64_t, uint8_t> clip_ids_;
};

constexpr uint32_t kSystemInstanceId = 8;
constexpr const char *kGlobals = "$Globals";
constexpr const char *kWorldMatrix = "worldMatrix";
constexpr const char *kInstanceOffsets = "dxVectorBufferOffsets";
constexpr const char *kInstanceVectors = "instanceVectorBuffer";
constexpr const char *kPartVectors = "boneVectors";
constexpr uint32_t kWorldRows = 3;

std::string model_row(uint32_t p_row) {
	const std::string k = std::to_string(p_row);
	return "vec4(MODEL_MATRIX[0][" + k + "], MODEL_MATRIX[1][" + k + "], MODEL_MATRIX[2][" + k + "], MODEL_MATRIX[3][" +
			k + "])";
}

std::optional<std::string> attribute_lane(VertexInputLane p_lane) {
	using core::res::VertexUsage;
	struct Attribute {
		VertexUsage usage;
		const char *name;
		uint8_t lanes;
	};
	static constexpr Attribute kAttributes[] = { { VertexUsage::Position, "VERTEX", 3 },
		{ VertexUsage::Normal, "NORMAL", 3 }, { VertexUsage::Tangent, "TANGENT", 3 },
		{ VertexUsage::Binormal, "BINORMAL", 3 }, { VertexUsage::Color0, "COLOR", 4 },
		{ VertexUsage::TexCoord0, "UV", 2 }, { VertexUsage(uint8_t(VertexUsage::TexCoord0) + 1), "UV2", 2 } };
	if (p_lane.usage == uint8_t(VertexUsage::BinormalSign) && p_lane.lane == 0) {
		return std::string("(dot(BINORMAL, cross(NORMAL, TANGENT)) < 0.0 ? -1.0 : 1.0)");
	}
	for (const Attribute &a : kAttributes) {
		if (uint8_t(a.usage) == p_lane.usage && p_lane.lane < a.lanes) {
			return std::string(a.name) + "." + kLanes[p_lane.lane];
		}
	}
	return std::nullopt;
}

class VertexStage {
public:
	VertexStage(const dxbc::Program &p_vs, std::span<const VertexInputLanes> p_lanes) : vs_(p_vs), lanes_(p_lanes) {}

	std::string lane(uint32_t p_reg, uint8_t p_lane) {
		wanted_.insert(lane_key(uint8_t(OperandType::Output), p_reg, p_lane));
		return "vso" + std::to_string(p_reg) + "." + kLanes[p_lane];
	}

	bool empty() const { return wanted_.empty(); }

	std::string varyings() const {
		std::string out;
		for (uint32_t reg : outputs()) {
			out += "varying vec4 vso" + std::to_string(reg) + ";\n";
		}
		return out;
	}

	core::Result<std::string> body() {
		BF3_TRY(flatten());
		BF3_TRY(trace_instance_rows());
		BF3_TRY(slice());
		Emitter emitter{ ConstantNames::Locals };
		std::string code;
		for (size_t i = 0; i < vs_.code.size(); ++i) {
			if (!needed_[i]) {
				continue;
			}
			const Instruction &ins = vs_.code[i];
			core::Result<std::string> line = ins.is(Op::Ld) ? load_row(i) : emitter.statement(ins);
			if (!line) {
				return refuse("vertex program: " + line.error().message);
			}
			code += "\t" + *line + "\n";
		}
		std::string locals;
		for (const auto &[reg, lanes] : inputs_) {
			core::Result<std::string> l = input_local(reg, lanes);
			if (!l) {
				return l.error();
			}
			locals += *l;
		}
		for (const auto &[key, lanes] : constants_) {
			core::Result<std::string> l = constant_local(uint32_t(key >> 32), uint32_t(key & 0xFFFFFFFFu), lanes);
			if (!l) {
				return l.error();
			}
			locals += *l;
		}
		for (uint32_t r = 0; r < vs_.temps; ++r) {
			locals += "\tvec4 r" + std::to_string(r) + " = vec4(0.0);\n";
		}
		std::string tail;
		for (uint32_t reg : outputs()) {
			locals += "\tvec4 o" + std::to_string(reg) + " = vec4(0.0);\n";
			tail += "\tvso" + std::to_string(reg) + " = o" + std::to_string(reg) + ";\n";
		}
		return locals + code + tail;
	}

private:
	bool part_shown(size_t p_if) const {
		const Instruction &branch = vs_.code[p_if];
		if (!branch.nonzero || branch.operands.size() != 1 || !branch.operands[0].is(OperandType::Temp)) {
			return false;
		}
		const Operand &test = branch.operands[0];
		const uint8_t lane = test.lane(0);
		for (size_t j = p_if; j-- > 0;) {
			const Instruction &ins = vs_.code[j];
			if (ins.operands.empty() || !ins.operands[0].is(OperandType::Temp) ||
					ins.operands[0].index[0] != test.index[0] || ((ins.operands[0].write_mask() >> lane) & 1u) == 0) {
				continue;
			}
			if (!ins.is(Op::Lt) || ins.saturate || ins.operands.size() != 3 ||
					!ins.operands[1].is(OperandType::Immediate32) ||
					ins.operands[1].imm[ins.operands[1].lane(lane)] != 0) {
				return false;
			}
			const Operand &vector = ins.operands[2];
			uint32_t component = 0;
			const dxbc::ConstantBuffer *cb = vector.is(OperandType::ConstantBuffer) && vector.dims == 2 &&
							vector.relative && !vector.negate && !vector.absolute
					? vs_.constant_buffer(vector.index[0])
					: nullptr;
			const dxbc::Variable *v = cb != nullptr
					? variable_at(vector.index[0], vector.index[1], vector.lane(lane), component)
					: nullptr;
			return cb != nullptr && cb->name == kGlobals && v != nullptr && v->name == kPartVectors;
		}
		return false;
	}

	core::Status flatten() {
		removed_.assign(vs_.code.size(), false);
		struct Block {
			size_t at_if = 0;
			std::optional<size_t> at_else;
		};
		std::vector<Block> open;
		for (size_t i = 0; i < vs_.code.size(); ++i) {
			const Instruction &ins = vs_.code[i];
			if (ins.is(Op::If)) {
				if (!part_shown(i)) {
					return refuse("vertex program branches on " + dxbc::format_instruction(ins));
				}
				open.push_back(Block{ i, std::nullopt });
				removed_[i] = true;
			} else if (ins.is(Op::Else)) {
				if (open.empty() || open.back().at_else) {
					return refuse("vertex program else outside if");
				}
				open.back().at_else = i;
			} else if (ins.is(Op::EndIf)) {
				if (open.empty()) {
					return refuse("vertex program endif outside if");
				}
				const Block b = open.back();
				open.pop_back();
				for (size_t k = b.at_else.value_or(i); k <= i; ++k) {
					removed_[k] = true;
				}
			}
		}
		if (!open.empty()) {
			return refuse("vertex program if without endif");
		}
		return core::Status::success();
	}

	std::set<uint32_t> outputs() const {
		std::set<uint32_t> out;
		for (uint64_t key : wanted_) {
			out.insert(uint32_t((key >> 8) & 0xFFFFFFFFu));
		}
		return out;
	}

	const dxbc::Variable *variable_at(uint32_t p_cb, uint32_t p_reg, uint8_t p_lane, uint32_t &r_component) const {
		const dxbc::ConstantBuffer *cb = vs_.constant_buffer(p_cb);
		if (cb == nullptr) {
			return nullptr;
		}
		const uint32_t at = p_reg * 16 + p_lane * 4;
		for (const dxbc::Variable &v : cb->variables) {
			if (at >= v.offset && at < v.offset + v.size) {
				r_component = (at - v.offset) / 4;
				return &v;
			}
		}
		return nullptr;
	}

	bool instance_offset(const Operand &p_op, uint8_t p_lane, uint32_t p_component) const {
		uint32_t component = 0;
		const dxbc::ConstantBuffer *cb = p_op.is(OperandType::ConstantBuffer) && p_op.dims == 2 && !p_op.relative &&
						!p_op.negate && !p_op.absolute
				? vs_.constant_buffer(p_op.index[0])
				: nullptr;
		if (cb == nullptr || cb->name != kGlobals) {
			return false;
		}
		const dxbc::Variable *v = variable_at(p_op.index[0], p_op.index[1], p_op.lane(p_lane), component);
		return v != nullptr && v->name == kInstanceOffsets && component == p_component;
	}

	bool instance_id(const Operand &p_op, uint8_t p_lane) const {
		if (!p_op.is(OperandType::Input) || p_op.negate || p_op.absolute) {
			return false;
		}
		for (const dxbc::SignatureElement &e : vs_.inputs) {
			if (e.reg == p_op.index[0] && e.system_value == kSystemInstanceId && p_op.lane(p_lane) == 0) {
				return true;
			}
		}
		return false;
	}

	std::optional<int64_t> symbol(const Operand &p_op, uint8_t p_lane) const {
		if (!p_op.is(OperandType::Temp) || p_op.negate || p_op.absolute) {
			return std::nullopt;
		}
		const auto found = symbols_.find(lane_key(p_op.type, p_op.index[0], p_op.lane(p_lane)));
		return found == symbols_.end() ? std::nullopt : std::optional<int64_t>(found->second);
	}

	static constexpr int64_t kInstanceIndex = -1;

	core::Status trace_instance_rows() {
		for (size_t i = 0; i < vs_.code.size(); ++i) {
			const Instruction &ins = vs_.code[i];
			if (removed_[i] || ins.operands.empty() || !ins.operands[0].is(OperandType::Temp)) {
				continue;
			}
			const Operand &d = ins.operands[0];
			for (uint8_t c = 0; c < 4; ++c) {
				if (((d.write_mask() >> c) & 1u) == 0) {
					continue;
				}
				std::optional<int64_t> value;
				const std::vector<Operand> &ops = ins.operands;
				if (ins.is(Op::Utof) && ops.size() == 2 && instance_id(ops[1], c)) {
					value = kInstanceIndex;
				} else if (ins.is(Op::Mad) && ops.size() == 4 && symbol(ops[1], c) == kInstanceIndex &&
						instance_offset(ops[2], c, 1) && instance_offset(ops[3], c, 0) && !ins.saturate) {
					value = 0;
				} else if (ins.is(Op::Ftou) && ops.size() == 2) {
					const std::optional<int64_t> row = symbol(ops[1], c);
					if (row && *row >= 0) {
						value = row;
					}
				} else if (ins.is(Op::Iadd) && ops.size() == 3) {
					const std::optional<int64_t> row = symbol(ops[1], c);
					if (row && *row >= 0 && ops[2].is(OperandType::Immediate32) && !ops[2].negate) {
						value = *row + int64_t(int32_t(ops[2].imm[ops[2].lane(c)]));
					}
				}
				const uint64_t key = lane_key(d.type, d.index[0], c);
				if (value) {
					symbols_[key] = *value;
				} else {
					symbols_.erase(key);
				}
			}
			if (ins.is(Op::Ld)) {
				rows_[i] = ins.operands.size() == 3 ? symbol(ins.operands[1], 0) : std::nullopt;
			}
		}
		return core::Status::success();
	}

	core::Status slice() {
		std::set<uint64_t> live = wanted_;
		needed_.assign(vs_.code.size(), false);
		for (size_t i = vs_.code.size(); i-- > 0;) {
			const Instruction &ins = vs_.code[i];
			if (removed_[i]) {
				continue;
			}
			if (ins.is(Op::Ret)) {
				if (i + 1 != vs_.code.size()) {
					return refuse("vertex program returns early");
				}
				continue;
			}
			if (ins.is(Op::If) || ins.is(Op::Else) || ins.is(Op::EndIf) || ins.is(Op::Discard)) {
				return refuse("vertex program branches: " + dxbc::format_instruction(ins));
			}
			if (ins.operands.empty()) {
				continue;
			}
			const Operand &d = ins.operands[0];
			if (!d.is(OperandType::Temp) && !d.is(OperandType::Output)) {
				continue;
			}
			std::vector<uint8_t> written;
			for (uint8_t c = 0; c < 4; ++c) {
				if (((d.write_mask() >> c) & 1u) == 0) {
					continue;
				}
				const auto it = live.find(lane_key(d.type, d.index[0], c));
				if (it != live.end()) {
					live.erase(it);
					written.push_back(c);
				}
			}
			if (written.empty()) {
				continue;
			}
			needed_[i] = true;
			if (ins.is(Op::Ld)) {
				const auto row = rows_.find(i);
				const dxbc::Binding *b = ins.operands.size() == 3 && ins.operands[2].is(OperandType::Resource)
						? vs_.binding(dxbc::BindingType::Texture, ins.operands[2].index[0])
						: nullptr;
				if (b == nullptr || b->name != kInstanceVectors || row == rows_.end() || !row->second ||
						*row->second >= int64_t(kWorldRows)) {
					return refuse(
							"vertex program loads what is not a world matrix row: " + dxbc::format_instruction(ins));
				}
				continue;
			}
			if (!componentwise(ins.opcode) && dot_width(ins.opcode) == 0) {
				return refuse("vertex program instruction " + dxbc::format_instruction(ins));
			}
			for (uint8_t c : written) {
				for (const Read &r : reads_for(ins, c)) {
					const Operand &op = ins.operands[r.operand];
					if (op.is(OperandType::Temp)) {
						live.insert(lane_key(op.type, op.index[0], r.lane));
					} else if (op.is(OperandType::Input)) {
						inputs_[op.index[0]] |= uint8_t(1u << r.lane);
					} else if (op.is(OperandType::ConstantBuffer) && op.dims == 2 && !op.relative) {
						constants_[uint64_t(op.index[0]) << 32 | op.index[1]] |= uint8_t(1u << r.lane);
					} else if (!op.is(OperandType::Immediate32)) {
						return refuse("vertex program operand " + dxbc::format_instruction(ins));
					}
				}
			}
		}
		if (!live.empty()) {
			return refuse("vertex program reads or outputs a value no instruction writes");
		}
		return core::Status::success();
	}

	core::Result<std::string> load_row(size_t p_index) const {
		const Instruction &ins = vs_.code[p_index];
		const Operand &res = ins.operands[2];
		std::string swizzle;
		for (uint8_t c = 0; c < 4; ++c) {
			swizzle += kLanes[res.lane(c)];
		}
		const Operand &d = ins.operands[0];
		std::string mask;
		for (uint8_t c = 0; c < 4; ++c) {
			if ((d.write_mask() >> c) & 1u) {
				mask += kLanes[c];
			}
		}
		return "r" + std::to_string(d.index[0]) + "." + mask + " = (" + model_row(uint32_t(*rows_.at(p_index))) + "." +
				swizzle + ")." + mask + ";";
	}

	core::Result<std::string> input_local(uint32_t p_reg, uint8_t p_lanes) const {
		const dxbc::SignatureElement *element = nullptr;
		for (const dxbc::SignatureElement &e : vs_.inputs) {
			if (e.reg == p_reg) {
				element = &e;
			}
		}
		const std::string where = "vertex input v" + std::to_string(p_reg);
		if (element == nullptr || element->system_value != 0 || element->name != "TEXCOORD") {
			return refuse(where + " is not a vertex element");
		}
		const uint32_t index = element->semantic_index;
		if (index >= lanes_.size()) {
			return refuse(where + " has no element usage");
		}
		std::string l = "\tvec4 v" + std::to_string(p_reg) + " = vec4(";
		for (uint8_t c = 0; c < 4; ++c) {
			std::string lane = "0.0";
			if ((p_lanes >> c) & 1u) {
				const VertexInputLane in = lanes_[index][c];
				const std::optional<std::string> a = attribute_lane(in);
				if (!a) {
					return refuse(where + "." + kLanes[c] + " reads usage " + std::to_string(in.usage) + " lane " +
							std::to_string(in.lane));
				}
				lane = *a;
			}
			l += (c == 0 ? "" : ", ") + lane;
		}
		return l + ");\n";
	}

	core::Result<std::string> constant_local(uint32_t p_cb, uint32_t p_reg, uint8_t p_lanes) const {
		const dxbc::ConstantBuffer *cb = vs_.constant_buffer(p_cb);
		if (cb == nullptr) {
			return refuse("vertex constant buffer cb" + std::to_string(p_cb) + " is not described");
		}
		std::array<std::string, 4> lanes{ "0.0", "0.0", "0.0", "0.0" };
		for (uint8_t c = 0; c < 4; ++c) {
			if (((p_lanes >> c) & 1u) == 0) {
				continue;
			}
			uint32_t component = 0;
			const dxbc::Variable *var = variable_at(p_cb, p_reg, c, component);
			if (var == nullptr) {
				return refuse(
						"vertex " + cb->name + " offset " + std::to_string(p_reg * 16 + c * 4) + " is no variable");
			}
			if (cb->name == kGlobals && var->name == kWorldMatrix && component < 16) {
				lanes[c] = "MODEL_MATRIX[" + std::to_string(component % 4) + "][" + std::to_string(component / 4) + "]";
			} else if (cb->name == kViewConstants && var->name == "cameraPos" && component < 3) {
				lanes[c] = std::string("CAMERA_POSITION_WORLD.") + kLanes[component];
			} else if (cb->name == kViewConstants && var->name == "time" && component == 0) {
				lanes[c] = "TIME";
			} else {
				return refuse("vertex program reads " + cb->name + "." + var->name);
			}
		}
		return "\tvec4 cb" + std::to_string(p_cb) + "_" + std::to_string(p_reg) + " = vec4(" + lanes[0] + ", " +
				lanes[1] + ", " + lanes[2] + ", " + lanes[3] + ");\n";
	}

	const dxbc::Program &vs_;
	std::span<const VertexInputLanes> lanes_;
	std::set<uint64_t> wanted_;
	std::map<uint64_t, int64_t> symbols_;
	std::map<size_t, std::optional<int64_t>> rows_;
	std::vector<bool> needed_;
	std::vector<bool> removed_;
	std::map<uint32_t, uint8_t> inputs_;
	std::map<uint64_t, uint8_t> constants_;
};

bool two_channel(uint32_t p_format) {
	return p_format == kDxgiR32G32Float || p_format == kDxgiR16G16Float || p_format == kDxgiR16G16Unorm ||
			p_format == kDxgiR16G16Snorm;
}

bool colour_channel(uint32_t p_format) {
	return p_format == kDxgiR8G8B8A8Unorm || p_format == kDxgiB8G8R8A8Unorm;
}

class Translator {
public:
	explicit Translator(const ProgramRequest &p_request) : request_(p_request), ps_(*p_request.pixel) {
		for (const dxbc::Binding &b : ps_.bindings) {
			if (b.type == uint32_t(dxbc::BindingType::Texture) && b.dimension == dxbc::kDimensionTextureCube) {
				emitter_.cubes.insert(b.bind_point);
			}
		}
	}

	core::Result<TranslatedProgram> run() {
		const core::Status sliced = slice();
		if (walked_ && request_.analysis != nullptr) {
			analyse(*request_.analysis);
		}
		BF3_TRY(sliced);
		trace_coordinates();
		std::string body;
		for (size_t i = 0; i < ps_.code.size(); ++i) {
			if (!needed_[i]) {
				continue;
			}
			core::Result<std::string> line = emitter_.statement(ps_.code[i]);
			if (!line) {
				return line.error();
			}
			body += "\t" + *line + "\n";
			++out_.instructions;
		}
		std::string head;
		for (uint32_t reg : textures_) {
			const bool srgb = request_.srgb && request_.srgb(reg);
			const bool cube = emitter_.cubes.count(reg) != 0;
			head += std::string(cube ? "uniform samplerCube t" : "uniform sampler2D t") + std::to_string(reg) +
					(srgb ? " : source_color," : " :") +
					(cube ? " filter_linear_mipmap;\n" : " filter_linear_mipmap_anisotropic, repeat_enable;\n");
			out_.textures.push_back(reg);
			if (cube) {
				out_.cubes.push_back(reg);
			}
		}
		if (depth_) {
			head += "uniform sampler2D depth_tex : hint_depth_texture, filter_nearest;\n";
		}
		std::string locals;
		for (const auto &[reg, lanes] : inputs_) {
			std::string l = "\tvec4 v" + std::to_string(reg) + " = vec4(";
			for (uint8_t c = 0; c < 4; ++c) {
				core::Result<std::string> e =
						((lanes >> c) & 1u) ? input_lane(reg, c) : core::Result<std::string>(std::string("0.0"));
				if (!e) {
					return e.error();
				}
				l += (c == 0 ? "" : ", ") + *e;
			}
			locals += l + ");\n";
		}
		for (const auto &[key, lanes] : constants_) {
			core::Result<std::string> l = constant_local(uint32_t(key >> 32), uint32_t(key & 0xFFFFFFFFu), lanes, head);
			if (!l) {
				return l.error();
			}
			locals += *l;
		}
		for (uint32_t r = 0; r < ps_.temps; ++r) {
			locals += "\tvec4 r" + std::to_string(r) + " = vec4(0.0);\n";
		}
		for (uint32_t o : outputs_used_) {
			locals += "\tvec4 o" + std::to_string(o) + " = vec4(0.0);\n";
		}
		core::Result<std::string> vertex = vertex_stage();
		if (!vertex) {
			return vertex.error();
		}
		out_.code =
				header() + head + common_uniforms() + *vertex + "void fragment() {\n" + locals + body + tail() + "}\n";
		return out_;
	}

private:
	core::Status slice() {
		std::set<uint64_t> live;
		if (request_.target == ProgramTarget::Deferred) {
			bool albedo = false;
			for (const dxbc::SignatureElement &e : ps_.outputs) {
				albedo = albedo || (e.reg == kGbufferAlbedo && e.name == "SV_Target");
			}
			if (!albedo) {
				return refuse("no albedo target o1");
			}
			for (uint8_t c = 0; c < 3; ++c) {
				live.insert(lane_key(uint8_t(OperandType::Output), kGbufferAlbedo, c));
			}
			outputs_used_.insert(kGbufferAlbedo);
		} else {
			for (uint8_t c = 0; c < 4; ++c) {
				live.insert(lane_key(uint8_t(OperandType::Output), 0, c));
			}
			outputs_used_.insert(0);
		}
		needed_.assign(ps_.code.size(), false);
		for (size_t i = ps_.code.size(); i-- > 0;) {
			const Instruction &ins = ps_.code[i];
			if (ins.is(Op::Ret)) {
				if (i + 1 != ps_.code.size()) {
					return refuse("early ret");
				}
				continue;
			}
			if (ins.is(Op::Discard)) {
				needed_[i] = true;
				note_read(ins.operands[0], ins.operands[0].lane(0), live);
				continue;
			}
			const bool known = componentwise(ins.opcode) || dot_width(ins.opcode) > 0 || sampling(ins.opcode);
			if (!known) {
				defer("instruction " + dxbc::format_instruction(ins));
			}
			if (ins.operands.empty()) {
				if (!known) {
					continue;
				}
				return refuse("instruction without operands");
			}
			const Operand &d = ins.operands[0];
			if (!d.is(OperandType::Temp) && !d.is(OperandType::Output)) {
				if (!known) {
					continue;
				}
				return refuse("destination " + dxbc::format_instruction(ins));
			}
			bool any = false;
			std::vector<uint8_t> written;
			for (uint8_t c = 0; c < 4; ++c) {
				if (((d.mask >> c) & 1u) == 0) {
					continue;
				}
				const auto it = live.find(lane_key(d.type, d.index[0], c));
				if (it != live.end()) {
					any = true;
					live.erase(it);
					written.push_back(c);
				}
			}
			if (!any) {
				continue;
			}
			needed_[i] = true;
			if (d.is(OperandType::Output)) {
				outputs_used_.insert(d.index[0]);
			}
			if (!known) {
				for (size_t s = 1; s < ins.operands.size(); ++s) {
					for (uint8_t l = 0; l < 4; ++l) {
						note_read(ins.operands[s], ins.operands[s].lane(l), live);
					}
				}
				continue;
			}
			for (uint8_t c : written) {
				for (const Read &r : reads_for(ins, c, coordinate_lanes(ins))) {
					note_read(ins.operands[r.operand], r.lane, live);
				}
			}
			if (sampling(ins.opcode)) {
				note_texture(ins.operands[2]);
			}
		}
		walked_ = true;
		for (uint64_t key : live) {
			const uint8_t type = uint8_t(key >> 40);
			if (type != uint8_t(OperandType::Output)) {
				defer("reads a value no instruction writes");
			}
		}
		return pending_.empty() ? core::Status::success() : core::Status(refuse(pending_));
	}

	uint8_t coordinate_lanes(const Instruction &p_ins) const {
		return sampling(p_ins.opcode) && p_ins.operands.size() > 2 && p_ins.operands[2].is(OperandType::Resource) &&
						emitter_.cubes.count(p_ins.operands[2].index[0]) != 0
				? 3
				: 2;
	}

	void defer(const std::string &p_why) {
		if (pending_.empty()) {
			pending_ = p_why;
		}
	}

	void analyse(ProgramAnalysis &r_out) const {
		std::map<uint64_t, uint32_t> origin;
		std::set<uint32_t> vector_use, scalar_use;
		for (size_t i = 0; i < ps_.code.size(); ++i) {
			if (!needed_[i]) {
				continue;
			}
			const Instruction &ins = ps_.code[i];
			const Operand *d = ins.operands.empty() || ins.is(Op::Discard) ? nullptr : &ins.operands[0];
			std::vector<uint8_t> lanes;
			for (uint8_t c = 0; c < 4; ++c) {
				if (d != nullptr && ((d->mask >> c) & 1u) != 0) {
					lanes.push_back(c);
				}
			}
			if (ins.is(Op::Discard)) {
				lanes.push_back(0);
			}
			for (size_t s = ins.is(Op::Discard) ? 0 : 1; s < ins.operands.size(); ++s) {
				const Operand &op = ins.operands[s];
				if (!op.is(OperandType::Temp)) {
					continue;
				}
				std::map<uint32_t, std::set<uint8_t>> read;
				const uint8_t width = dot_width(ins.opcode);
				const auto note = [&](uint8_t p_lane) {
					const auto o = origin.find(lane_key(op.type, op.index[0], p_lane));
					if (o != origin.end()) {
						read[o->second].insert(p_lane);
					}
				};
				if (width > 0) {
					for (uint8_t l = 0; l < width; ++l) {
						note(op.lane(l));
					}
				} else if (!componentwise(ins.opcode) && !sampling(ins.opcode) && !ins.is(Op::Discard)) {
					for (uint8_t l = 0; l < 4; ++l) {
						note(op.lane(l));
					}
				} else {
					for (uint8_t c : lanes) {
						note(op.lane(c));
					}
				}
				for (const auto &[texture, used] : read) {
					(used.size() > 1 || (width > 0 && op.lane(0) != op.lane(width - 1)) ? vector_use : scalar_use)
							.insert(texture);
				}
			}
			if (d == nullptr || !d->is(OperandType::Temp)) {
				continue;
			}
			for (uint8_t c : lanes) {
				const uint64_t key = lane_key(d->type, d->index[0], c);
				if (sampling(ins.opcode) && ins.operands[2].is(OperandType::Resource)) {
					origin[key] = ins.operands[2].index[0];
				} else {
					origin.erase(key);
				}
			}
		}
		r_out.sliced = true;
		r_out.textures = textures_;
		r_out.mask_only.clear();
		for (uint32_t t : scalar_use) {
			if (vector_use.count(t) == 0) {
				r_out.mask_only.push_back(t);
			}
		}
	}

	void trace_coordinates() {
		std::map<uint64_t, std::set<uint64_t>> deps;
		const auto lane_deps = [&](const Operand &p_op, uint8_t p_lane) {
			std::set<uint64_t> out;
			if (p_op.is(OperandType::Input)) {
				out.insert(lane_key(p_op.type, p_op.index[0], p_lane));
			} else if (p_op.is(OperandType::Temp)) {
				const auto found = deps.find(lane_key(p_op.type, p_op.index[0], p_lane));
				if (found != deps.end()) {
					out = found->second;
				}
			}
			return out;
		};
		for (const Instruction &ins : ps_.code) {
			if (ins.operands.empty()) {
				continue;
			}
			if (sampling(ins.opcode) && request_.texture_usage && ins.operands.size() > 2 &&
					ins.operands[2].is(OperandType::Resource) && coordinate_lanes(ins) == 2) {
				const uint32_t usage = request_.texture_usage(ins.operands[2].index[0]);
				for (uint8_t l = 0; l < 2; ++l) {
					for (uint64_t input : lane_deps(ins.operands[1], ins.operands[1].lane(l))) {
						if (usage != 0) {
							input_usage_[input].insert(usage);
						}
					}
				}
			}
			const Operand &d = ins.operands[0];
			if (!d.is(OperandType::Temp)) {
				continue;
			}
			for (uint8_t c = 0; c < 4; ++c) {
				if (((d.mask >> c) & 1u) == 0) {
					continue;
				}
				std::set<uint64_t> merged;
				for (const Read &r : reads_for(ins, c, coordinate_lanes(ins))) {
					const std::set<uint64_t> from = lane_deps(ins.operands[r.operand], r.lane);
					merged.insert(from.begin(), from.end());
				}
				deps[lane_key(d.type, d.index[0], c)] = std::move(merged);
			}
		}
	}

	void note_read(const Operand &p_op, uint8_t p_lane, std::set<uint64_t> &r_live) {
		if (p_op.is(OperandType::Temp) || p_op.is(OperandType::Output)) {
			r_live.insert(lane_key(p_op.type, p_op.index[0], p_lane));
		} else if (p_op.is(OperandType::Input)) {
			inputs_[p_op.index[0]] |= uint8_t(1u << p_lane);
		} else if (p_op.is(OperandType::ConstantBuffer) && p_op.dims == 2) {
			constants_[uint64_t(p_op.index[0]) << 32 | p_op.index[1]] |= uint8_t(1u << p_lane);
		}
	}

	void note_texture(const Operand &p_resource) {
		if (!p_resource.is(OperandType::Resource)) {
			defer("sample without a resource");
			return;
		}
		const uint32_t reg = p_resource.index[0];
		const dxbc::Binding *b = ps_.binding(dxbc::BindingType::Texture, reg);
		if (b != nullptr && b->name == kDepthTexture) {
			depth_ = true;
			emitter_.depth_register = reg;
			return;
		}
		if (std::find(request_.bound_textures.begin(), request_.bound_textures.end(), reg) ==
				request_.bound_textures.end()) {
			defer("texture t" + std::to_string(reg) + (b != nullptr ? " (" + b->name + ")" : std::string()) +
					" is not a material texture");
			return;
		}
		if (std::find(textures_.begin(), textures_.end(), reg) == textures_.end()) {
			textures_.push_back(reg);
			std::sort(textures_.begin(), textures_.end());
		}
	}

	core::Result<std::string> input_lane(uint32_t p_reg, uint8_t p_lane) {
		const dxbc::SignatureElement *element = nullptr;
		for (const dxbc::SignatureElement &e : ps_.inputs) {
			if (e.reg == p_reg && ((e.mask >> p_lane) & 1u) != 0) {
				element = &e;
			}
		}
		const std::string where = "interpolant v" + std::to_string(p_reg) + "." + kLanes[p_lane];
		if (element == nullptr) {
			return refuse(where + " is not declared");
		}
		if (element->system_value == kSystemPosition) {
			static const char *kFrag[] = { "FRAGCOORD.x", "FRAGCOORD.y", "FRAGCOORD.z", "(1.0 / FRAGCOORD.w)" };
			return std::string(kFrag[p_lane]);
		}
		if (request_.vertex == nullptr) {
			return refuse(where + " without a vertex program");
		}
		const dxbc::SignatureElement *out = nullptr;
		for (const dxbc::SignatureElement &e : request_.vertex->outputs) {
			if (e.name == element->name && e.semantic_index == element->semantic_index) {
				out = &e;
			}
		}
		if (out == nullptr) {
			return refuse(where + " has no vertex output");
		}
		if (!origins_) {
			origins_.emplace(*request_.vertex);
		}
		const Origin o = origins_->output(out->reg, p_lane);
		if (o.kind == Origin::Kind::Clip) {
			clip_ = true;
			return std::string("clip_pos.") + kLanes[o.lane];
		}
		if (o.kind != Origin::Kind::Element || o.element >= request_.vertex_formats.size()) {
			return computed_lane(out->reg, p_lane);
		}
		const uint32_t format = request_.vertex_formats[o.element];
		std::string attribute;
		const auto usage = input_usage_.find(lane_key(uint8_t(OperandType::Input), p_reg, p_lane));
		const bool four = format == kDxgiR16G16B16A16Float || format == kDxgiR32G32B32A32Float;
		if ((two_channel(format) || (four && o.element > 0)) && usage != input_usage_.end() &&
				usage->second.size() == 1) {
			const uint32_t set = *usage->second.begin();
			if (set != kUsageTexCoord0 && set != kUsageTexCoord1) {
				return refuse(where + " samples with vertex usage " + std::to_string(set));
			}
			attribute = std::string(set == kUsageTexCoord0 ? "UV." : "UV2.") + kLanes[o.lane & 1u];
		} else if (two_channel(format) && o.lane < 2) {
			size_t rank = 0;
			for (uint32_t e = 0; e < o.element; ++e) {
				rank += two_channel(request_.vertex_formats[e]) ? 1 : 0;
			}
			if (rank > 1) {
				return refuse(where + " reads a third texture coordinate set");
			}
			attribute = std::string(rank == 0 ? "UV." : "UV2.") + kLanes[o.lane];
		} else if (colour_channel(format)) {
			static const char kBgra[] = "zyxw";
			attribute = std::string("COLOR.") + (format == kDxgiB8G8R8A8Unorm ? kBgra[o.lane] : kLanes[o.lane]);
		} else {
			return computed_lane(out->reg, p_lane);
		}
		const std::optional<std::string> scale = float_text(o.scale), offset = float_text(o.offset);
		if (!scale || !offset) {
			return refuse(where + " has a non-finite scale");
		}
		return "(" + attribute + " * " + *scale + " + " + *offset + ")";
	}

	std::string computed_lane(uint32_t p_reg, uint8_t p_lane) {
		if (!stage_) {
			stage_.emplace(*request_.vertex, request_.vertex_lanes);
		}
		return stage_->lane(p_reg, p_lane);
	}

	core::Result<std::string> constant_local(uint32_t p_cb, uint32_t p_reg, uint8_t p_lanes, std::string &r_head) {
		const dxbc::ConstantBuffer *cb = ps_.constant_buffer(p_cb);
		if (cb == nullptr) {
			return refuse("constant buffer cb" + std::to_string(p_cb) + " is not described");
		}
		std::array<std::string, 4> lanes{ "0.0", "0.0", "0.0", "0.0" };
		ProgramConstant external;
		external.uniform = "x" + std::to_string(p_cb) + "_" + std::to_string(p_reg);
		bool uses_external = false;
		for (uint8_t c = 0; c < 4; ++c) {
			if (((p_lanes >> c) & 1u) == 0) {
				continue;
			}
			const uint32_t at = p_reg * 16 + c * 4;
			const dxbc::Variable *var = nullptr;
			for (const dxbc::Variable &v : cb->variables) {
				if (at >= v.offset && at < v.offset + v.size) {
					var = &v;
				}
			}
			if (var == nullptr) {
				return refuse(cb->name + " offset " + std::to_string(at) + " is no variable");
			}
			const uint8_t component = uint8_t((at - var->offset) / 4);
			if (var->name.rfind(kExternalPrefix, 0) == 0) {
				if (component >= 4) {
					return refuse(var->name + " is wider than a vector");
				}
				external.lanes[c] = ProgramConstantLane{ var->name.substr(std::strlen(kExternalPrefix)), component };
				lanes[c] = external.uniform + "." + kLanes[c];
				uses_external = true;
				continue;
			}
			if (cb->name != kViewConstants) {
				return refuse(cb->name + "." + var->name);
			}
			core::Result<std::string> view = view_constant(var->name, component);
			if (!view) {
				return view.error();
			}
			lanes[c] = *view;
		}
		if (uses_external) {
			r_head += "uniform vec4 " + external.uniform + ";\n";
			out_.constants.push_back(std::move(external));
		}
		return "\tvec4 cb" + std::to_string(p_cb) + "_" + std::to_string(p_reg) + " = vec4(" + lanes[0] + ", " +
				lanes[1] + ", " + lanes[2] + ", " + lanes[3] + ");\n";
	}

	core::Result<std::string> view_constant(const std::string &p_name, uint8_t p_component) {
		if (p_name == "time" && p_component == 0) {
			return std::string("TIME");
		}
		if (p_name == "cameraPos" && p_component < 3) {
			return std::string("CAMERA_POSITION_WORLD.") + kLanes[p_component];
		}
		if (p_name == "screenSize") {
			static const char *kScreen[] = { "VIEWPORT_SIZE.x", "VIEWPORT_SIZE.y", "(1.0 / VIEWPORT_SIZE.x)",
				"(1.0 / VIEWPORT_SIZE.y)" };
			return std::string(kScreen[p_component & 3u]);
		}
		if (p_name == "viewportZMinMaxKzKw") {
			static const char *kDepth[] = { "0.0", "1.0", "(-PROJECTION_MATRIX[2][2])", "PROJECTION_MATRIX[3][2]" };
			return std::string(kDepth[p_component & 3u]);
		}
		return refuse(std::string(kViewConstants) + "." + p_name);
	}

	std::string header() const {
		std::string code = "shader_type spatial;\nrender_mode ";
		if (request_.target == ProgramTarget::Deferred) {
			code += "blend_mix, depth_draw_opaque, diffuse_burley, specular_schlick_ggx";
		} else {
			code += request_.blend == ProgramBlend::Add		   ? "blend_add"
					: request_.blend == ProgramBlend::Multiply ? "blend_mul"
															   : "blend_mix";
			code += ", depth_draw_never, unshaded";
		}
		code += request_.double_sided ? ", cull_disabled;\n" : ", cull_back;\n";
		return code;
	}

	std::string common_uniforms() const {
		if (request_.target != ProgramTarget::Deferred) {
			return std::string();
		}
		return R"(uniform sampler2D normal_tex : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
uniform bool has_normal = false;
uniform int normal_uv = 0;
uniform float normal_scale = 1.0;
uniform float roughness_value = 0.6;
)";
	}

	core::Result<std::string> vertex_stage() {
		std::string declarations, body;
		if (clip_) {
			declarations += "varying vec4 clip_pos;\n";
			body += "\tclip_pos = PROJECTION_MATRIX * (VIEW_MATRIX * (MODEL_MATRIX * vec4(VERTEX, 1.0)));\n";
		}
		if (stage_ && !stage_->empty()) {
			core::Result<std::string> computed = stage_->body();
			if (!computed) {
				return computed.error();
			}
			declarations += stage_->varyings();
			body += *computed;
		}
		if (body.empty()) {
			return std::string();
		}
		return declarations + "void vertex() {\n" + body + "}\n";
	}

	std::string tail() const {
		if (request_.target == ProgramTarget::Deferred) {
			return R"(	ALBEDO = o1.xyz * o1.xyz;
	ROUGHNESS = roughness_value;
	METALLIC = 0.0;
	SPECULAR = 0.5;
	if (has_normal) {
		NORMAL_MAP = vec3(texture(normal_tex, (normal_uv == 1 ? UV2 : UV) * normal_scale).rg, 1.0);
	}
)";
		}
		if (request_.blend == ProgramBlend::Mix) {
			return "\tALBEDO = o0.rgb;\n\tALPHA = o0.a;\n";
		}
		return "\tALBEDO = o0.rgb;\n\tALPHA = 1.0;\n";
	}

	const ProgramRequest &request_;
	const dxbc::Program &ps_;
	std::vector<bool> needed_;
	std::map<uint32_t, uint8_t> inputs_;
	std::map<uint64_t, std::set<uint32_t>> input_usage_;
	std::map<uint64_t, uint8_t> constants_;
	std::set<uint32_t> outputs_used_;
	std::vector<uint32_t> textures_;
	Emitter emitter_{ ConstantNames::Locals };
	bool depth_ = false;
	bool clip_ = false;
	bool walked_ = false;
	std::string pending_;
	std::optional<VertexOrigins> origins_;
	std::optional<VertexStage> stage_;
	TranslatedProgram out_;
};

}

core::Result<PixelTranslation> translate_pixel_program(const dxbc::Program &p_program) {
	if (p_program.kind != dxbc::kPixelProgram) {
		return refuse("not a pixel program");
	}
	Emitter emitter(ConstantNames::Arrays);
	PixelTranslation out;
	out.temps = p_program.temps;
	out.outputs = p_program.declared_outputs;
	std::string code;
	int depth = 0;
	bool returned = false;
	for (const Instruction &ins : p_program.code) {
		if (returned) {
			return refuse("code after return");
		}
		++out.instructions;
		if (ins.is(Op::Nop)) {
			continue;
		}
		if (ins.is(Op::Ret)) {
			if (depth != 0) {
				return refuse("conditional return");
			}
			returned = true;
			continue;
		}
		if ((ins.is(Op::Else) || ins.is(Op::EndIf)) && depth <= 0) {
			return refuse(dxbc::format_instruction(ins) + " outside if");
		}
		depth += ins.is(Op::If) ? 1 : ins.is(Op::EndIf) ? -1 : 0;
		core::Result<std::string> line = emitter.statement(ins);
		if (!line) {
			return line.error();
		}
		if (!line->empty()) {
			code += *line + "\n";
		}
	}
	if (depth != 0) {
		return refuse("unbalanced if");
	}
	std::string head;
	for (uint32_t i = 0; i < out.temps; ++i) {
		head += "vec4 r" + std::to_string(i) + " = vec4(0.0);\n";
	}
	for (const auto &[reg, size] : p_program.indexable_temps) {
		head += "vec4 x" + std::to_string(reg) + "[" + std::to_string(size) + "];\n";
	}
	out.body = head + code;
	out.textures.assign(emitter.textures.begin(), emitter.textures.end());
	out.inputs.assign(emitter.inputs.begin(), emitter.inputs.end());
	out.constant_buffers.assign(emitter.buffers.begin(), emitter.buffers.end());
	return out;
}

core::Result<TranslatedProgram> translate_program(const ProgramRequest &p_request) {
	if (p_request.pixel == nullptr || p_request.pixel->kind != dxbc::kPixelProgram) {
		return refuse("no pixel program");
	}
	Translator t(p_request);
	return t.run();
}

}
