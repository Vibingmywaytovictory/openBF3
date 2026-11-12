#include "bf3/render/dxbc.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace bf3::render {

namespace {

constexpr uint32_t kChunkRdef = 0x46454452u;
constexpr uint32_t kChunkShdr = 0x52444853u;
constexpr uint32_t kChunkShex = 0x58454853u;
constexpr uint32_t kMaxChunks = 64;
constexpr uint32_t kMaxRdefEntries = 1024;
constexpr uint32_t kMaxTemps = 4096;

enum Opcode : uint32_t {
	kAdd = 0,
	kAnd = 1,
	kDerivRtx = 11,
	kDerivRty = 12,
	kDiv = 14,
	kDp2 = 15,
	kDp3 = 16,
	kDp4 = 17,
	kElse = 18,
	kEndif = 21,
	kEq = 24,
	kExp = 25,
	kFrc = 26,
	kGe = 29,
	kIf = 31,
	kLog = 47,
	kLt = 49,
	kMad = 50,
	kMin = 51,
	kMax = 52,
	kCustomData = 53,
	kMov = 54,
	kMovc = 55,
	kMul = 56,
	kNe = 57,
	kNop = 58,
	kRet = 62,
	kRoundNe = 64,
	kRoundNi = 65,
	kRoundPi = 66,
	kRoundZ = 67,
	kRsq = 68,
	kSample = 69,
	kSampleL = 72,
	kSampleD = 73,
	kSampleB = 74,
	kSqrt = 75,
	kFirstDeclaration = 88,
	kDclConstantBuffer = 89,
	kDclInputPs = 98,
	kDclOutput = 101,
	kDclTemps = 104,
	kDclIndexableTemp = 105,
	kLastDeclaration = 106,
};

enum OperandType : uint32_t {
	kTemp = 0,
	kInput = 1,
	kOutput = 2,
	kIndexableTemp = 3,
	kImmediate32 = 4,
	kSampler = 6,
	kResource = 7,
	kConstantBuffer = 8,
};

uint32_t rd32(const uint8_t *p) {
	uint32_t v;
	std::memcpy(&v, p, 4);
	return v;
}

core::Error fail(const std::string &p_what) {
	return core::make_error("dxbc: " + p_what);
}

core::Result<std::string> rdef_string(std::span<const uint8_t> p_chunk, uint32_t p_offset) {
	if (p_offset >= p_chunk.size()) {
		return fail("rdef name out of range");
	}
	const void *nul = std::memchr(p_chunk.data() + p_offset, 0, p_chunk.size() - p_offset);
	if (nul == nullptr) {
		return fail("rdef name not terminated");
	}
	return std::string(reinterpret_cast<const char *>(p_chunk.data() + p_offset),
			size_t(static_cast<const uint8_t *>(nul) - p_chunk.data() - p_offset));
}

core::Status parse_rdef(std::span<const uint8_t> p_chunk, DxbcProgram &r_program) {
	if (p_chunk.size() < 28) {
		return fail("rdef header");
	}
	const uint32_t buffer_count = rd32(p_chunk.data());
	const uint32_t buffer_offset = rd32(p_chunk.data() + 4);
	const uint32_t binding_count = rd32(p_chunk.data() + 8);
	const uint32_t binding_offset = rd32(p_chunk.data() + 12);
	if (buffer_count > kMaxRdefEntries || binding_count > kMaxRdefEntries) {
		return fail("rdef counts");
	}
	if (uint64_t(binding_offset) + uint64_t(binding_count) * 32 > p_chunk.size() ||
			uint64_t(buffer_offset) + uint64_t(buffer_count) * 24 > p_chunk.size()) {
		return fail("rdef tables out of range");
	}
	for (uint32_t i = 0; i < binding_count; ++i) {
		const uint8_t *b = p_chunk.data() + binding_offset + 32 * i;
		core::Result<std::string> name = rdef_string(p_chunk, rd32(b));
		if (!name) {
			return name.error();
		}
		r_program.bindings.push_back(DxbcBinding{ std::move(*name), DxbcBindingType(rd32(b + 4)), rd32(b + 20) });
	}
	for (uint32_t i = 0; i < buffer_count; ++i) {
		const uint8_t *b = p_chunk.data() + buffer_offset + 24 * i;
		core::Result<std::string> name = rdef_string(p_chunk, rd32(b));
		if (!name) {
			return name.error();
		}
		DxbcConstantBuffer buffer;
		buffer.name = std::move(*name);
		buffer.size = rd32(b + 12);
		const uint32_t variable_count = rd32(b + 4);
		const uint32_t variable_offset = rd32(b + 8);
		if (variable_count > kMaxRdefEntries ||
				uint64_t(variable_offset) + uint64_t(variable_count) * 24 > p_chunk.size()) {
			return fail("rdef variables out of range");
		}
		for (uint32_t v = 0; v < variable_count; ++v) {
			const uint8_t *e = p_chunk.data() + variable_offset + 24 * v;
			core::Result<std::string> var = rdef_string(p_chunk, rd32(e));
			if (!var) {
				return var.error();
			}
			buffer.variables.push_back(DxbcVariable{ std::move(*var), rd32(e + 4), rd32(e + 8) });
		}
		r_program.constant_buffers.push_back(std::move(buffer));
	}
	return core::Status::success();
}

struct Operand {
	uint32_t type = 0;
	uint32_t components = 0;
	uint32_t mode = 0;
	uint32_t mask = 0;
	std::array<uint32_t, 4> swizzle{ 0, 1, 2, 3 };
	std::array<uint32_t, 3> index{};
	uint32_t dimension = 0;
	std::array<uint32_t, 4> immediate{};
	uint32_t modifier = 0;
};

class TokenReader {
public:
	explicit TokenReader(std::span<const uint32_t> p_tokens) : tokens_(p_tokens) {}

	bool done() const { return at_ >= tokens_.size(); }
	size_t at() const { return at_; }
	void seek(size_t p_at) { at_ = p_at; }

	core::Result<uint32_t> next() {
		if (at_ >= tokens_.size()) {
			return fail("token stream truncated");
		}
		return tokens_[at_++];
	}

	core::Result<Operand> operand() {
		core::Result<uint32_t> head = next();
		if (!head) {
			return head.error();
		}
		Operand op;
		const uint32_t t = *head;
		const uint32_t count_type = t & 3u;
		op.components = count_type == 0 ? 0 : count_type == 1 ? 1 : count_type == 2 ? 4 : 0xFFu;
		if (op.components == 0xFFu) {
			return fail("operand with N components");
		}
		if (op.components == 4) {
			op.mode = (t >> 2) & 3u;
			if (op.mode == 0) {
				op.mask = (t >> 4) & 15u;
			} else if (op.mode == 1) {
				for (uint32_t i = 0; i < 4; ++i) {
					op.swizzle[i] = (t >> (4 + 2 * i)) & 3u;
				}
			} else if (op.mode == 2) {
				op.swizzle.fill((t >> 4) & 3u);
			} else {
				return fail("operand selection mode");
			}
		} else if (op.components == 1) {
			op.swizzle.fill(0);
		}
		op.type = (t >> 12) & 0xFFu;
		op.dimension = (t >> 20) & 3u;
		if ((t >> 31) != 0) {
			core::Result<uint32_t> ext = next();
			if (!ext) {
				return ext.error();
			}
			if ((*ext & 0x3Fu) == 1) {
				op.modifier = (*ext >> 6) & 0xFFu;
			}
			if ((*ext >> 31) != 0) {
				return fail("chained extended operand");
			}
		}
		if (op.type == kImmediate32) {
			for (uint32_t i = 0; i < op.components; ++i) {
				core::Result<uint32_t> v = next();
				if (!v) {
					return v.error();
				}
				op.immediate[i] = *v;
			}
			return op;
		}
		for (uint32_t d = 0; d < op.dimension; ++d) {
			const uint32_t representation = (t >> (22 + 3 * d)) & 7u;
			if (representation != 0) {
				return fail("relative operand index");
			}
			core::Result<uint32_t> v = next();
			if (!v) {
				return v.error();
			}
			op.index[d] = *v;
		}
		return op;
	}

private:
	std::span<const uint32_t> tokens_;
	size_t at_ = 0;
};

std::string float_literal(uint32_t p_bits) {
	float f;
	std::memcpy(&f, &p_bits, 4);
	if (!std::isfinite(f)) {
		return {};
	}
	char buf[48];
	const auto r = std::to_chars(buf, buf + sizeof(buf), f);
	std::string s(buf, r.ptr);
	const size_t e = s.find('e');
	if (s.find('.') == std::string::npos) {
		s.insert(e == std::string::npos ? s.size() : e, ".0");
	}
	return f < 0.0f ? "(" + s + ")" : s;
}

constexpr char kLetters[] = "xyzw";

struct Translator {
	PixelTranslation out;
	std::string code;
	std::set<uint32_t> textures, inputs;
	std::map<uint32_t, uint32_t> buffers;
	std::map<uint32_t, uint32_t> indexable;
	uint32_t samples = 0;
	int depth = 0;
	bool returned = false;

	core::Result<std::string> reg(const Operand &p_op) const {
		switch (p_op.type) {
			case kTemp:
				return "r" + std::to_string(p_op.index[0]);
			case kInput:
				return "v" + std::to_string(p_op.index[0]);
			case kOutput:
				return "o" + std::to_string(p_op.index[0]);
			case kIndexableTemp:
				return "x" + std::to_string(p_op.index[0]) + "[" + std::to_string(p_op.index[1]) + "]";
			case kConstantBuffer:
				return "cb" + std::to_string(p_op.index[0]) + "[" + std::to_string(p_op.index[1]) + "]";
			default:
				return fail("operand type " + std::to_string(p_op.type));
		}
	}

	core::Result<std::string> component(const Operand &p_op, uint32_t p_position) {
		std::string e;
		if (p_op.type == kImmediate32) {
			e = float_literal(p_op.immediate[p_op.components == 1 ? 0 : p_position]);
			if (e.empty()) {
				return fail("non-finite immediate");
			}
		} else {
			core::Result<std::string> r = reg(p_op);
			if (!r) {
				return r.error();
			}
			if (p_op.type == kInput) {
				inputs.insert(p_op.index[0]);
			}
			if (p_op.type == kConstantBuffer) {
				uint32_t &size = buffers[p_op.index[0]];
				size = std::max(size, p_op.index[1] + 1);
			}
			e = *r + "." + kLetters[p_op.mode == 0 && p_op.components == 4 ? p_position : p_op.swizzle[p_position]];
		}
		if (p_op.modifier == 2 || p_op.modifier == 3) {
			e = "abs(" + e + ")";
		}
		if (p_op.modifier == 1 || p_op.modifier == 3) {
			e = "(-" + e + ")";
		}
		return e;
	}

	core::Status write(const Operand &p_dst, bool p_saturate, const std::vector<std::string> &p_values) {
		core::Result<std::string> r = reg(p_dst);
		if (!r) {
			return r.error();
		}
		std::string letters;
		std::string values;
		for (uint32_t c = 0; c < 4; ++c) {
			if (((p_dst.components == 1 ? 1u : p_dst.mask) >> c & 1u) == 0) {
				continue;
			}
			std::string v = p_values[c];
			if (p_saturate) {
				v = "clamp(" + v + ", 0.0, 1.0)";
			}
			values += (values.empty() ? "" : ", ") + v;
			letters += kLetters[c];
		}
		if (letters.empty()) {
			return core::Status::success();
		}
		if (letters.size() == 1) {
			code += *r + "." + letters + " = " + values + ";\n";
		} else {
			code += *r + "." + letters + " = vec" + std::to_string(letters.size()) + "(" + values + ");\n";
		}
		return core::Status::success();
	}

	core::Result<std::string> texture_name(const Operand &p_resource) {
		if (p_resource.type != kResource) {
			return fail("sample without a resource operand");
		}
		textures.insert(p_resource.index[0]);
		return "t" + std::to_string(p_resource.index[0]);
	}

	core::Result<std::string> vec2_of(const Operand &p_op) {
		core::Result<std::string> a = component(p_op, 0);
		core::Result<std::string> b = component(p_op, 1);
		if (!a || !b) {
			return fail("sample coordinates");
		}
		return "vec2(" + *a + ", " + *b + ")";
	}
};

core::Status translate_instruction(
		Translator &r_t, uint32_t p_opcode, bool p_saturate, bool p_nonzero, std::vector<Operand> &p_ops) {
	const auto need = [&](size_t p_count) -> core::Status {
		if (p_ops.size() != p_count) {
			return fail("opcode " + std::to_string(p_opcode) + " has " + std::to_string(p_ops.size()) + " operands");
		}
		return core::Status::success();
	};
	const auto per_component = [&](size_t p_sources, auto &&p_make) -> core::Status {
		BF3_TRY(need(p_sources + 1));
		std::vector<std::string> values(4);
		for (uint32_t c = 0; c < 4; ++c) {
			std::vector<std::string> s;
			for (size_t i = 1; i <= p_sources; ++i) {
				core::Result<std::string> e = r_t.component(p_ops[i], c);
				if (!e) {
					return e.error();
				}
				s.push_back(std::move(*e));
			}
			values[c] = p_make(s);
		}
		return r_t.write(p_ops[0], p_saturate, values);
	};
	const auto dot = [&](uint32_t p_n) -> core::Status {
		BF3_TRY(need(3));
		std::string sum;
		for (uint32_t c = 0; c < p_n; ++c) {
			core::Result<std::string> a = r_t.component(p_ops[1], c);
			core::Result<std::string> b = r_t.component(p_ops[2], c);
			if (!a || !b) {
				return fail("dot operand");
			}
			sum += (sum.empty() ? "" : " + ") + *a + " * " + *b;
		}
		const std::string v = "(" + sum + ")";
		return r_t.write(p_ops[0], p_saturate, { v, v, v, v });
	};
	const auto unary = [&](const char *p_fn) {
		return per_component(
				1, [&](const std::vector<std::string> &s) { return std::string(p_fn) + "(" + s[0] + ")"; });
	};
	const auto compare = [&](const char *p_op) {
		return per_component(2, [&](const std::vector<std::string> &s) {
			return "(" + s[0] + " " + p_op + " " + s[1] + " ? 1.0 : 0.0)";
		});
	};
	switch (p_opcode) {
		case kMov:
			return per_component(1, [](const std::vector<std::string> &s) { return s[0]; });
		case kAdd:
			return per_component(2, [](const std::vector<std::string> &s) { return "(" + s[0] + " + " + s[1] + ")"; });
		case kMul:
			return per_component(2, [](const std::vector<std::string> &s) { return "(" + s[0] + " * " + s[1] + ")"; });
		case kDiv:
			return per_component(2, [](const std::vector<std::string> &s) { return "(" + s[0] + " / " + s[1] + ")"; });
		case kMad:
			return per_component(3,
					[](const std::vector<std::string> &s) { return "(" + s[0] + " * " + s[1] + " + " + s[2] + ")"; });
		case kMin:
			return per_component(
					2, [](const std::vector<std::string> &s) { return "min(" + s[0] + ", " + s[1] + ")"; });
		case kMax:
			return per_component(
					2, [](const std::vector<std::string> &s) { return "max(" + s[0] + ", " + s[1] + ")"; });
		case kAnd: {
			BF3_TRY(need(3));
			for (size_t i = 1; i <= 2; ++i) {
				for (uint32_t bits : p_ops[i].type == kImmediate32 ? p_ops[i].immediate : std::array<uint32_t, 4>{}) {
					float value = 0.0f;
					std::memcpy(&value, &bits, 4);
					if (value != 0.0f && !std::isnormal(value)) {
						return fail("and with the integer mask " + std::to_string(bits));
					}
				}
			}
			return per_component(2, [&](const std::vector<std::string> &s) {
				return p_ops[1].type == kImmediate32 ? "(" + s[1] + " != 0.0 ? " + s[0] + " : 0.0)"
													 : "(" + s[0] + " != 0.0 ? " + s[1] + " : 0.0)";
			});
		}
		case kMovc:
			return per_component(3, [](const std::vector<std::string> &s) {
				return "(" + s[0] + " != 0.0 ? " + s[1] + " : " + s[2] + ")";
			});
		case kLt:
			return compare("<");
		case kGe:
			return compare(">=");
		case kEq:
			return compare("==");
		case kNe:
			return compare("!=");
		case kDp2:
			return dot(2);
		case kDp3:
			return dot(3);
		case kDp4:
			return dot(4);
		case kRsq:
			return unary("inversesqrt");
		case kSqrt:
			return unary("sqrt");
		case kFrc:
			return unary("fract");
		case kLog:
			return unary("log2");
		case kExp:
			return unary("exp2");
		case kRoundNe:
			return unary("roundEven");
		case kRoundNi:
			return unary("floor");
		case kRoundPi:
			return unary("ceil");
		case kRoundZ:
			return unary("trunc");
		case kDerivRtx:
			return unary("dFdx");
		case kDerivRty:
			return unary("dFdy");
		case kIf: {
			BF3_TRY(need(1));
			core::Result<std::string> c = r_t.component(p_ops[0], 0);
			if (!c) {
				return c.error();
			}
			r_t.code += std::string("if (") + *c + (p_nonzero ? " != 0.0) {\n" : " == 0.0) {\n");
			++r_t.depth;
			return core::Status::success();
		}
		case kElse:
			if (r_t.depth <= 0) {
				return fail("else outside if");
			}
			r_t.code += "} else {\n";
			return core::Status::success();
		case kEndif:
			if (r_t.depth <= 0) {
				return fail("endif outside if");
			}
			--r_t.depth;
			r_t.code += "}\n";
			return core::Status::success();
		case kRet:
			if (r_t.depth != 0) {
				return fail("conditional return");
			}
			r_t.returned = true;
			return core::Status::success();
		case kNop:
			return core::Status::success();
		case kSample:
		case kSampleL:
		case kSampleB:
		case kSampleD: {
			const size_t operands = p_opcode == kSample ? 4 : p_opcode == kSampleD ? 6 : 5;
			BF3_TRY(need(operands));
			core::Result<std::string> tex = r_t.texture_name(p_ops[2]);
			core::Result<std::string> uv = r_t.vec2_of(p_ops[1]);
			if (!tex || !uv) {
				return fail("sample operands");
			}
			std::string call;
			if (p_opcode == kSample) {
				call = "texture(" + *tex + ", " + *uv + ")";
			} else if (p_opcode == kSampleD) {
				core::Result<std::string> dx = r_t.vec2_of(p_ops[4]);
				core::Result<std::string> dy = r_t.vec2_of(p_ops[5]);
				if (!dx || !dy) {
					return fail("sample gradients");
				}
				call = "textureGrad(" + *tex + ", " + *uv + ", " + *dx + ", " + *dy + ")";
			} else {
				core::Result<std::string> lod = r_t.component(p_ops[4], 0);
				if (!lod) {
					return lod.error();
				}
				call = (p_opcode == kSampleL ? "textureLod(" : "texture(") + *tex + ", " + *uv + ", " + *lod + ")";
			}
			const std::string name = "s" + std::to_string(r_t.samples++);
			r_t.code += "vec4 " + name + " = " + call + ";\n";
			std::vector<std::string> values(4);
			for (uint32_t c = 0; c < 4; ++c) {
				values[c] = name + "." + kLetters[p_ops[2].swizzle[c]];
			}
			return r_t.write(p_ops[0], p_saturate, values);
		}
		default:
			return fail("unsupported opcode " + std::to_string(p_opcode));
	}
}

}

const DxbcBinding *DxbcProgram::binding(DxbcBindingType p_type, std::string_view p_name) const {
	for (const DxbcBinding &b : bindings) {
		if (b.type == p_type && b.name == p_name) {
			return &b;
		}
	}
	return nullptr;
}

const DxbcVariable *DxbcProgram::variable(std::string_view p_buffer, std::string_view p_name) const {
	for (const DxbcConstantBuffer &cb : constant_buffers) {
		if (cb.name != p_buffer) {
			continue;
		}
		for (const DxbcVariable &v : cb.variables) {
			if (v.name == p_name) {
				return &v;
			}
		}
	}
	return nullptr;
}

core::Result<DxbcProgram> parse_dxbc(std::span<const uint8_t> p_bytes) {
	if (p_bytes.size() < 32 || p_bytes.size() > kMaxDxbcBytes || std::memcmp(p_bytes.data(), "DXBC", 4) != 0) {
		return fail("not a DXBC container");
	}
	const uint32_t total = rd32(p_bytes.data() + 24);
	const uint32_t chunks = rd32(p_bytes.data() + 28);
	if (total != p_bytes.size() || chunks > kMaxChunks || 32 + uint64_t(chunks) * 4 > p_bytes.size()) {
		return fail("container header");
	}
	DxbcProgram program;
	bool have_code = false;
	for (uint32_t i = 0; i < chunks; ++i) {
		const uint32_t at = rd32(p_bytes.data() + 32 + 4 * i);
		if (uint64_t(at) + 8 > p_bytes.size()) {
			return fail("chunk offset");
		}
		const uint32_t fourcc = rd32(p_bytes.data() + at);
		const uint32_t size = rd32(p_bytes.data() + at + 4);
		if (uint64_t(at) + 8 + size > p_bytes.size()) {
			return fail("chunk size");
		}
		const std::span<const uint8_t> chunk = p_bytes.subspan(size_t(at) + 8, size);
		if (fourcc == kChunkRdef) {
			BF3_TRY(parse_rdef(chunk, program));
		} else if (fourcc == kChunkShdr || fourcc == kChunkShex) {
			if (size < 8 || size % 4 != 0) {
				return fail("shader chunk size");
			}
			const uint32_t version = rd32(chunk.data());
			const uint32_t length = rd32(chunk.data() + 4);
			if (uint64_t(length) * 4 > size || length < 2) {
				return fail("shader length");
			}
			program.program_type = version >> 16;
			program.major = (version >> 4) & 15u;
			program.minor = version & 15u;
			program.tokens.resize(length - 2);
			std::memcpy(program.tokens.data(), chunk.data() + 8, program.tokens.size() * 4);
			have_code = true;
		}
	}
	if (!have_code) {
		return fail("no shader code");
	}
	return program;
}

core::Result<PixelTranslation> translate_pixel_program(const DxbcProgram &p_program) {
	if (p_program.program_type != kDxbcPixelProgram) {
		return fail("not a pixel program");
	}
	Translator t;
	TokenReader r(p_program.tokens);
	while (!r.done()) {
		const size_t start = r.at();
		core::Result<uint32_t> head = r.next();
		if (!head) {
			return head.error();
		}
		const uint32_t opcode = *head & 0x7FFu;
		uint32_t length = (*head >> 24) & 0x7Fu;
		if (opcode == kCustomData) {
			core::Result<uint32_t> n = r.next();
			if (!n) {
				return n.error();
			}
			length = *n;
		}
		if (length == 0 || start + length > p_program.tokens.size()) {
			return fail("instruction length at token " + std::to_string(start));
		}
		const size_t end = start + length;
		if (t.returned) {
			return fail("code after return");
		}
		if (opcode == kCustomData) {
			r.seek(end);
			continue;
		}
		if (opcode >= kFirstDeclaration && opcode <= kLastDeclaration) {
			if (opcode == kDclTemps) {
				core::Result<uint32_t> n = r.next();
				if (!n || *n > kMaxTemps) {
					return fail("dcl_temps");
				}
				t.out.temps = *n;
			} else if (opcode == kDclIndexableTemp) {
				core::Result<uint32_t> reg = r.next();
				core::Result<uint32_t> size = r.next();
				if (!reg || !size || *size > kMaxTemps) {
					return fail("dcl_indexable_temp");
				}
				t.indexable[*reg] = *size;
			} else if (opcode == kDclOutput) {
				core::Result<Operand> o = r.operand();
				if (!o) {
					return o.error();
				}
				t.out.outputs = std::max(t.out.outputs, o->index[0] + 1);
			}
			r.seek(end);
			continue;
		}
		bool extended = (*head >> 31) != 0;
		while (extended) {
			core::Result<uint32_t> ext = r.next();
			if (!ext) {
				return ext.error();
			}
			if ((*ext & 0x3Fu) == 1 && (*ext & 0x1FFE00u) != 0) {
				return fail("sample offsets");
			}
			extended = (*ext >> 31) != 0;
		}
		std::vector<Operand> ops;
		while (r.at() < end) {
			core::Result<Operand> op = r.operand();
			if (!op) {
				return op.error();
			}
			ops.push_back(*op);
		}
		if (r.at() != end) {
			return fail("operands overrun instruction at token " + std::to_string(start));
		}
		const bool saturate = ((*head >> 13) & 1u) != 0;
		const bool nonzero = ((*head >> 18) & 1u) != 0;
		BF3_TRY(translate_instruction(t, opcode, saturate, nonzero, ops));
		++t.out.instructions;
	}
	if (t.depth != 0) {
		return fail("unbalanced if");
	}
	std::string head;
	for (uint32_t i = 0; i < t.out.temps; ++i) {
		head += "vec4 r" + std::to_string(i) + " = vec4(0.0);\n";
	}
	for (const auto &[reg, size] : t.indexable) {
		head += "vec4 x" + std::to_string(reg) + "[" + std::to_string(size) + "];\n";
	}
	t.out.body = head + t.code;
	t.out.textures.assign(t.textures.begin(), t.textures.end());
	t.out.inputs.assign(t.inputs.begin(), t.inputs.end());
	t.out.constant_buffers.assign(t.buffers.begin(), t.buffers.end());
	return std::move(t.out);
}

}
