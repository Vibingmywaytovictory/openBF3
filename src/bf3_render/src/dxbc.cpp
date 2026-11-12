#include "bf3/render/dxbc.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace bf3::render::dxbc {

namespace {

constexpr size_t kContainerHeaderBytes = 32;
constexpr uint32_t kTag(const char (&p_tag)[5]) {
	return uint32_t(uint8_t(p_tag[0])) | uint32_t(uint8_t(p_tag[1])) << 8 | uint32_t(uint8_t(p_tag[2])) << 16 |
			uint32_t(uint8_t(p_tag[3])) << 24;
}
constexpr uint32_t kMaxChunks = 64;
constexpr uint32_t kMaxRdefEntries = 1024;
constexpr uint32_t kOpcodeMask = 0x7FF;
constexpr uint32_t kSaturateBit = 1u << 13;
constexpr uint32_t kNonzeroBit = 1u << 18;
constexpr uint32_t kExtendedBit = 1u << 31;
constexpr uint32_t kSampleControls = 1;
constexpr uint16_t kFirstDeclaration = 88;
constexpr uint16_t kLastDeclaration = 106;
constexpr size_t kMaxOperandDepth = 4;
constexpr uint32_t kImmediateConstantBufferClass = 3;
constexpr size_t kRdefHeaderBytes = 28;
constexpr size_t kBindingBytes = 32;
constexpr size_t kBufferBytes = 24;
constexpr size_t kVariableBytes4 = 24;
constexpr size_t kVariableBytes5 = 40;
constexpr size_t kSignatureBytes = 24;

uint32_t rd32(const uint8_t *p) {
	uint32_t v;
	std::memcpy(&v, p, 4);
	return v;
}

core::Error fail(const std::string &p_what) {
	return core::make_error("dxbc: " + p_what);
}

int8_t offset_bits(uint32_t p_token, uint32_t p_shift) {
	const int v = int((p_token >> p_shift) & 0xFu);
	return int8_t(v >= 8 ? v - 16 : v);
}

class Words {
public:
	explicit Words(std::span<const uint8_t> p_bytes) : bytes_(p_bytes) {}
	size_t size() const { return bytes_.size() / 4; }
	uint32_t operator[](size_t p_at) const { return rd32(bytes_.data() + p_at * 4); }

private:
	std::span<const uint8_t> bytes_;
};

core::Result<std::string> string_at(std::span<const uint8_t> p_chunk, uint32_t p_offset) {
	if (p_offset >= p_chunk.size()) {
		return fail("name offset out of range");
	}
	const void *nul = std::memchr(p_chunk.data() + p_offset, 0, p_chunk.size() - p_offset);
	if (nul == nullptr) {
		return fail("unterminated name");
	}
	return std::string(reinterpret_cast<const char *>(p_chunk.data() + p_offset),
			size_t(static_cast<const uint8_t *>(nul) - (p_chunk.data() + p_offset)));
}

core::Status read_signature(std::span<const uint8_t> p_chunk, std::vector<SignatureElement> &r_out) {
	if (p_chunk.size() < 8) {
		return fail("short signature");
	}
	const uint32_t count = rd32(p_chunk.data());
	if (count > kMaxRdefEntries || uint64_t(count) * kSignatureBytes + 8 > p_chunk.size()) {
		return fail("signature out of range");
	}
	for (uint32_t i = 0; i < count; ++i) {
		const uint8_t *e = p_chunk.data() + 8 + size_t(i) * kSignatureBytes;
		SignatureElement s;
		core::Result<std::string> name = string_at(p_chunk, rd32(e));
		if (!name) {
			return name.error();
		}
		s.name = std::move(*name);
		s.semantic_index = rd32(e + 4);
		s.system_value = rd32(e + 8);
		s.reg = rd32(e + 16);
		s.mask = e[20];
		r_out.push_back(std::move(s));
	}
	return core::Status::success();
}

core::Status read_rdef(std::span<const uint8_t> p_chunk, Program &r_out) {
	if (p_chunk.size() < kRdefHeaderBytes) {
		return fail("short RDEF");
	}
	const uint32_t buffers = rd32(p_chunk.data()), buffers_at = rd32(p_chunk.data() + 4);
	const uint32_t bindings = rd32(p_chunk.data() + 8), bindings_at = rd32(p_chunk.data() + 12);
	const uint8_t major = p_chunk[17];
	const size_t variable_bytes = major >= 5 ? kVariableBytes5 : kVariableBytes4;
	if (buffers > kMaxRdefEntries || bindings > kMaxRdefEntries) {
		return fail("RDEF counts");
	}
	if (uint64_t(bindings_at) + uint64_t(bindings) * kBindingBytes > p_chunk.size() ||
			uint64_t(buffers_at) + uint64_t(buffers) * kBufferBytes > p_chunk.size()) {
		return fail("RDEF tables out of range");
	}
	for (uint32_t i = 0; i < bindings; ++i) {
		const uint8_t *e = p_chunk.data() + bindings_at + size_t(i) * kBindingBytes;
		Binding b;
		core::Result<std::string> name = string_at(p_chunk, rd32(e));
		if (!name) {
			return name.error();
		}
		b.name = std::move(*name);
		b.type = rd32(e + 4);
		b.dimension = rd32(e + 12);
		b.bind_point = rd32(e + 20);
		r_out.bindings.push_back(std::move(b));
	}
	for (uint32_t i = 0; i < buffers; ++i) {
		const uint8_t *e = p_chunk.data() + buffers_at + size_t(i) * kBufferBytes;
		ConstantBuffer cb;
		core::Result<std::string> name = string_at(p_chunk, rd32(e));
		if (!name) {
			return name.error();
		}
		cb.name = std::move(*name);
		cb.size = rd32(e + 12);
		const uint32_t variables = rd32(e + 4), variables_at = rd32(e + 8);
		if (variables > kMaxRdefEntries ||
				uint64_t(variables_at) + uint64_t(variables) * variable_bytes > p_chunk.size()) {
			return fail("RDEF variables out of range");
		}
		for (uint32_t v = 0; v < variables; ++v) {
			const uint8_t *ve = p_chunk.data() + variables_at + size_t(v) * variable_bytes;
			Variable var;
			core::Result<std::string> vname = string_at(p_chunk, rd32(ve));
			if (!vname) {
				return vname.error();
			}
			var.name = std::move(*vname);
			var.offset = rd32(ve + 4);
			var.size = rd32(ve + 8);
			cb.variables.push_back(std::move(var));
		}
		r_out.constant_buffers.push_back(std::move(cb));
	}
	return core::Status::success();
}

core::Result<size_t> read_operand(const Words &p_w, size_t p_at, size_t p_end, Operand &r_op, size_t p_depth) {
	if (p_depth > kMaxOperandDepth || p_at >= p_end) {
		return fail("operand out of range");
	}
	const uint32_t t = p_w[p_at++];
	const uint32_t count = t & 3u;
	r_op.components = count == 0 ? 0 : count == 1 ? 1 : 4;
	if (count == 3) {
		return fail("N-component operand");
	}
	if (count == 2) {
		r_op.mode = uint8_t((t >> 2) & 3u);
		if (r_op.mode == 0) {
			r_op.mask = uint8_t((t >> 4) & 0xFu);
		} else if (r_op.mode == 1) {
			for (int c = 0; c < 4; ++c) {
				r_op.swizzle[size_t(c)] = uint8_t((t >> (4 + 2 * c)) & 3u);
			}
		} else if (r_op.mode == 2) {
			const uint8_t s = uint8_t((t >> 4) & 3u);
			r_op.swizzle = { s, s, s, s };
		} else {
			return fail("operand selection mode");
		}
	}
	r_op.type = uint8_t((t >> 12) & 0xFFu);
	r_op.dims = uint8_t((t >> 20) & 3u);
	bool extended = (t & kExtendedBit) != 0;
	while (extended) {
		if (p_at >= p_end) {
			return fail("extended operand out of range");
		}
		const uint32_t e = p_w[p_at++];
		if ((e & 0x3Fu) == 1u) {
			const uint32_t modifier = (e >> 6) & 0xFFu;
			r_op.negate = modifier == 1 || modifier == 3;
			r_op.absolute = modifier == 2 || modifier == 3;
		}
		extended = (e & kExtendedBit) != 0;
	}
	if (r_op.is(OperandType::Immediate32)) {
		const size_t n = r_op.components == 1 ? 1 : 4;
		if (p_at + n > p_end) {
			return fail("immediate out of range");
		}
		for (size_t c = 0; c < n; ++c) {
			r_op.imm[c] = p_w[p_at++];
		}
		if (n == 1) {
			r_op.imm = { r_op.imm[0], r_op.imm[0], r_op.imm[0], r_op.imm[0] };
		}
		return p_at;
	}
	if (r_op.is(OperandType::Immediate64)) {
		return fail("64-bit immediate");
	}
	for (uint8_t d = 0; d < r_op.dims; ++d) {
		const uint32_t rep = (t >> (22 + 3 * d)) & 7u;
		if (rep == 0) {
			if (p_at >= p_end) {
				return fail("index out of range");
			}
			r_op.index[d] = p_w[p_at++];
		} else if (rep == 2 || rep == 3) {
			if (rep == 3) {
				if (p_at >= p_end) {
					return fail("index out of range");
				}
				r_op.index[d] = p_w[p_at++];
			}
			Operand relative;
			core::Result<size_t> next = read_operand(p_w, p_at, p_end, relative, p_depth + 1);
			if (!next) {
				return next.error();
			}
			p_at = *next;
			r_op.relative = true;
		} else {
			return fail("64-bit index");
		}
	}
	return p_at;
}

core::Status read_declaration(const Words &p_w, uint16_t p_opcode, size_t p_at, size_t p_next, Program &r_out) {
	if (p_opcode == uint16_t(Op::DclTemps)) {
		if (p_at + 1 >= p_next || p_w[p_at + 1] > kMaxTemps) {
			return fail("dcl_temps");
		}
		r_out.temps = p_w[p_at + 1];
	} else if (p_opcode == uint16_t(Op::DclIndexableTemp)) {
		if (p_at + 2 >= p_next || p_w[p_at + 2] > kMaxTemps) {
			return fail("dcl_indexable_temp");
		}
		r_out.indexable_temps.emplace_back(p_w[p_at + 1], p_w[p_at + 2]);
	} else if (p_opcode == uint16_t(Op::DclOutput)) {
		Operand o;
		core::Result<size_t> after = read_operand(p_w, p_at + 1, p_next, o, 0);
		if (!after) {
			return after.error();
		}
		r_out.declared_outputs = std::max(r_out.declared_outputs, o.index[0] + 1);
	}
	return core::Status::success();
}

core::Status read_code(std::span<const uint8_t> p_chunk, Program &r_out) {
	const Words w(p_chunk);
	if (w.size() < 2 || p_chunk.size() % 4 != 0) {
		return fail("shader code size");
	}
	const uint32_t version = w[0];
	r_out.minor = uint8_t(version & 0xFu);
	r_out.major = uint8_t((version >> 4) & 0xFu);
	r_out.kind = uint16_t(version >> 16);
	const size_t end = w[1];
	if (end < 2 || end > w.size()) {
		return fail("shader length");
	}
	size_t at = 2;
	while (at < end) {
		const uint32_t token = w[at];
		const uint16_t opcode = uint16_t(token & kOpcodeMask);
		size_t length = (token >> 24) & 0x7Fu;
		if (opcode == uint16_t(Op::CustomData)) {
			if (at + 1 >= end) {
				return fail("short custom data");
			}
			length = w[at + 1];
			if (length < 2 || at + length > end) {
				return fail("custom data out of range");
			}
			if ((token >> 11) == kImmediateConstantBufferClass) {
				for (size_t i = at + 2; i + 4 <= at + length; i += 4) {
					r_out.immediate_constants.push_back({ w[i], w[i + 1], w[i + 2], w[i + 3] });
				}
			}
			at += length;
			continue;
		}
		if (length == 0 || at + length > end) {
			return fail("instruction length at word " + std::to_string(at));
		}
		const size_t next = at + length;
		if (opcode >= kFirstDeclaration && opcode <= kLastDeclaration) {
			BF3_TRY(read_declaration(w, opcode, at, next, r_out));
			at = next;
			continue;
		}
		if (r_out.code.size() >= kMaxInstructions) {
			return fail("more than " + std::to_string(kMaxInstructions) + " instructions");
		}
		Instruction ins;
		ins.opcode = opcode;
		ins.saturate = (token & kSaturateBit) != 0;
		ins.nonzero = (token & kNonzeroBit) != 0;
		size_t p = at + 1;
		bool extended = (token & kExtendedBit) != 0;
		while (extended && p < next) {
			const uint32_t e = w[p++];
			if ((e & 0x3Fu) == kSampleControls) {
				ins.offset = { offset_bits(e, 9), offset_bits(e, 13), offset_bits(e, 17) };
			}
			extended = (e & kExtendedBit) != 0;
		}
		while (p < next) {
			Operand op;
			core::Result<size_t> after = read_operand(w, p, next, op, 0);
			if (!after) {
				return after.error();
			}
			p = *after;
			ins.operands.push_back(op);
		}
		r_out.code.push_back(std::move(ins));
		at = next;
	}
	return core::Status::success();
}

}

uint8_t Operand::lane(uint8_t p_component) const {
	if (components != 4) {
		return 0;
	}
	return mode == 0 ? p_component : swizzle[p_component & 3u];
}

const Binding *Program::binding(BindingType p_type, uint32_t p_point) const {
	for (const Binding &b : bindings) {
		if (b.type == uint32_t(p_type) && b.bind_point == p_point) {
			return &b;
		}
	}
	return nullptr;
}

const Binding *Program::binding(BindingType p_type, std::string_view p_name) const {
	for (const Binding &b : bindings) {
		if (b.type == uint32_t(p_type) && b.name == p_name) {
			return &b;
		}
	}
	return nullptr;
}

const ConstantBuffer *Program::constant_buffer(uint32_t p_point) const {
	const Binding *b = binding(BindingType::ConstantBuffer, p_point);
	if (b == nullptr) {
		return nullptr;
	}
	for (const ConstantBuffer &cb : constant_buffers) {
		if (cb.name == b->name) {
			return &cb;
		}
	}
	return nullptr;
}

const Variable *Program::variable(std::string_view p_buffer, std::string_view p_name) const {
	for (const ConstantBuffer &cb : constant_buffers) {
		if (cb.name != p_buffer) {
			continue;
		}
		for (const Variable &v : cb.variables) {
			if (v.name == p_name) {
				return &v;
			}
		}
	}
	return nullptr;
}

core::Result<Program> parse(std::span<const uint8_t> p_blob) {
	if (p_blob.size() < kContainerHeaderBytes || p_blob.size() > kMaxBytes ||
			std::memcmp(p_blob.data(), "DXBC", 4) != 0) {
		return fail("not a DXBC container");
	}
	const uint32_t total = rd32(p_blob.data() + 24);
	const uint32_t chunks = rd32(p_blob.data() + 28);
	if (total != p_blob.size() || chunks > kMaxChunks || uint64_t(chunks) * 4 + kContainerHeaderBytes > p_blob.size()) {
		return fail("container header");
	}
	Program out;
	bool code = false;
	for (uint32_t i = 0; i < chunks; ++i) {
		const uint32_t at = rd32(p_blob.data() + kContainerHeaderBytes + 4 * i);
		if (uint64_t(at) + 8 > p_blob.size()) {
			return fail("chunk out of range");
		}
		const uint32_t tag = rd32(p_blob.data() + at), size = rd32(p_blob.data() + at + 4);
		if (uint64_t(at) + 8 + size > p_blob.size()) {
			return fail("chunk body out of range");
		}
		const std::span<const uint8_t> body = p_blob.subspan(size_t(at) + 8, size);
		if (tag == kTag("RDEF")) {
			BF3_TRY(read_rdef(body, out));
		} else if (tag == kTag("ISGN")) {
			BF3_TRY(read_signature(body, out.inputs));
		} else if (tag == kTag("OSGN")) {
			BF3_TRY(read_signature(body, out.outputs));
		} else if (tag == kTag("SHDR") || tag == kTag("SHEX")) {
			BF3_TRY(read_code(body, out));
			code = true;
		}
	}
	if (!code) {
		return fail("no shader code chunk");
	}
	return out;
}

std::string opcode_name(uint16_t p_opcode) {
	switch (Op(p_opcode)) {
		case Op::Add:
			return "add";
		case Op::And:
			return "and";
		case Op::DerivRtx:
			return "deriv_rtx";
		case Op::DerivRty:
			return "deriv_rty";
		case Op::Discard:
			return "discard";
		case Op::Div:
			return "div";
		case Op::Dp2:
			return "dp2";
		case Op::Dp3:
			return "dp3";
		case Op::Dp4:
			return "dp4";
		case Op::Else:
			return "else";
		case Op::EndIf:
			return "endif";
		case Op::Eq:
			return "eq";
		case Op::Exp:
			return "exp";
		case Op::Frc:
			return "frc";
		case Op::Ge:
			return "ge";
		case Op::If:
			return "if";
		case Op::Log:
			return "log";
		case Op::Lt:
			return "lt";
		case Op::Mad:
			return "mad";
		case Op::Min:
			return "min";
		case Op::Max:
			return "max";
		case Op::Mov:
			return "mov";
		case Op::Movc:
			return "movc";
		case Op::Mul:
			return "mul";
		case Op::Ne:
			return "ne";
		case Op::Nop:
			return "nop";
		case Op::Ret:
			return "ret";
		case Op::RoundNe:
			return "round_ne";
		case Op::RoundNi:
			return "round_ni";
		case Op::RoundPi:
			return "round_pi";
		case Op::RoundZ:
			return "round_z";
		case Op::Rsq:
			return "rsq";
		case Op::Sample:
			return "sample";
		case Op::SampleL:
			return "sample_l";
		case Op::SampleD:
			return "sample_d";
		case Op::SampleB:
			return "sample_b";
		case Op::Sqrt:
			return "sqrt";
		default:
			return "op" + std::to_string(p_opcode);
	}
}

std::string format_instruction(const Instruction &p_instruction) {
	static const char kLanes[] = "xyzw";
	std::string out = opcode_name(p_instruction.opcode);
	if (p_instruction.saturate) {
		out += "_sat";
	}
	if (p_instruction.is(Op::Discard) || p_instruction.is(Op::If)) {
		out += p_instruction.nonzero ? "_nz" : "_z";
	}
	if (p_instruction.offset != std::array<int8_t, 3>{}) {
		out += "_aoffimmi(" + std::to_string(p_instruction.offset[0]) + "," + std::to_string(p_instruction.offset[1]) +
				"," + std::to_string(p_instruction.offset[2]) + ")";
	}
	for (size_t i = 0; i < p_instruction.operands.size(); ++i) {
		const Operand &o = p_instruction.operands[i];
		out += i == 0 ? " " : ", ";
		std::string text;
		if (o.is(OperandType::Immediate32)) {
			text = "l(";
			for (size_t c = 0; c < (o.components == 1 ? 1u : 4u); ++c) {
				float f;
				std::memcpy(&f, &o.imm[c], 4);
				char buf[32];
				std::snprintf(buf, sizeof(buf), "%s%f", c == 0 ? "" : ", ", double(f));
				text += buf;
			}
			text += ")";
		} else {
			static const char *kPrefix[] = { "r", "v", "o", "x", "l", "d", "s", "t", "cb", "icb" };
			text = o.type < 10 ? kPrefix[o.type] : "?" + std::to_string(o.type) + "_";
			if ((o.is(OperandType::ConstantBuffer) || o.is(OperandType::IndexableTemp)) && o.dims == 2) {
				text += std::to_string(o.index[0]) + "[" + std::to_string(o.index[1]) + (o.relative ? "+rel" : "") +
						"]";
			} else if (o.dims >= 1) {
				text += std::to_string(o.index[0]);
			}
			if (o.components == 4) {
				text += ".";
				if (o.mode == 0) {
					for (int c = 0; c < 4; ++c) {
						if ((o.mask >> c) & 1u) {
							text += kLanes[c];
						}
					}
				} else {
					for (int c = 0; c < (o.mode == 2 ? 1 : 4); ++c) {
						text += kLanes[o.swizzle[size_t(c)]];
					}
				}
			}
		}
		if (o.absolute) {
			text = "|" + text + "|";
		}
		if (o.negate) {
			text = "-" + text;
		}
		out += text;
	}
	return out;
}

}
