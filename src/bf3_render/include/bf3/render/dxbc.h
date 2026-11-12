#pragma once

#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::render::dxbc {

enum class Op : uint16_t {
	Add = 0,
	And = 1,
	DerivRtx = 11,
	DerivRty = 12,
	Discard = 13,
	Div = 14,
	Dp2 = 15,
	Dp3 = 16,
	Dp4 = 17,
	Else = 18,
	EndIf = 21,
	Eq = 24,
	Exp = 25,
	Frc = 26,
	Ftou = 28,
	Ge = 29,
	Iadd = 30,
	If = 31,
	Ieq = 32,
	Ld = 45,
	Log = 47,
	Lt = 49,
	Mad = 50,
	Min = 51,
	Max = 52,
	CustomData = 53,
	Mov = 54,
	Movc = 55,
	Mul = 56,
	Ne = 57,
	Nop = 58,
	Ret = 62,
	RoundNe = 64,
	RoundNi = 65,
	RoundPi = 66,
	RoundZ = 67,
	Rsq = 68,
	Sample = 69,
	SampleL = 72,
	SampleD = 73,
	SampleB = 74,
	Sqrt = 75,
	Utof = 86,
	DclOutput = 101,
	DclTemps = 104,
	DclIndexableTemp = 105,
};

enum class OperandType : uint8_t {
	Temp = 0,
	Input = 1,
	Output = 2,
	IndexableTemp = 3,
	Immediate32 = 4,
	Immediate64 = 5,
	Sampler = 6,
	Resource = 7,
	ConstantBuffer = 8,
	ImmediateConstantBuffer = 9,
	Null = 13,
};

struct Operand {
	uint8_t type = 0;
	uint8_t components = 0;
	uint8_t mode = 0;
	uint8_t mask = 0xF;
	std::array<uint8_t, 4> swizzle{ 0, 1, 2, 3 };
	bool negate = false;
	bool absolute = false;
	uint8_t dims = 0;
	bool relative = false;
	std::array<uint32_t, 3> index{};
	std::array<uint32_t, 4> imm{};

	bool is(OperandType p_type) const { return type == uint8_t(p_type); }
	uint8_t lane(uint8_t p_component) const;
	uint8_t write_mask() const { return components == 1 ? 1u : mask; }
};

struct Instruction {
	uint16_t opcode = 0;
	bool saturate = false;
	bool nonzero = false;
	std::array<int8_t, 3> offset{};
	std::vector<Operand> operands;

	bool is(Op p_op) const { return opcode == uint16_t(p_op); }
};

struct SignatureElement {
	std::string name;
	uint32_t semantic_index = 0;
	uint32_t system_value = 0;
	uint32_t reg = 0;
	uint8_t mask = 0;
};

struct Variable {
	std::string name;
	uint32_t offset = 0;
	uint32_t size = 0;
};

struct ConstantBuffer {
	std::string name;
	uint32_t size = 0;
	std::vector<Variable> variables;
};

enum class BindingType : uint32_t {
	ConstantBuffer = 0,
	TextureBuffer = 1,
	Texture = 2,
	Sampler = 3,
};

struct Binding {
	std::string name;
	uint32_t type = 0;
	uint32_t dimension = 0;
	uint32_t bind_point = 0;
};

inline constexpr uint16_t kPixelProgram = 0;
inline constexpr uint16_t kVertexProgram = 1;
inline constexpr size_t kMaxInstructions = 4096;
inline constexpr size_t kMaxBytes = size_t(1) << 22;
inline constexpr uint32_t kMaxTemps = 4096;

struct Program {
	uint16_t kind = kPixelProgram;
	uint8_t major = 0;
	uint8_t minor = 0;
	uint32_t temps = 0;
	uint32_t declared_outputs = 0;
	std::vector<std::pair<uint32_t, uint32_t>> indexable_temps;
	std::vector<Instruction> code;
	std::vector<SignatureElement> inputs;
	std::vector<SignatureElement> outputs;
	std::vector<ConstantBuffer> constant_buffers;
	std::vector<Binding> bindings;
	std::vector<std::array<uint32_t, 4>> immediate_constants;

	const Binding *binding(BindingType p_type, uint32_t p_point) const;
	const Binding *binding(BindingType p_type, std::string_view p_name) const;
	const ConstantBuffer *constant_buffer(uint32_t p_point) const;
	const Variable *variable(std::string_view p_buffer, std::string_view p_name) const;
};

core::Result<Program> parse(std::span<const uint8_t> p_blob);

std::string opcode_name(uint16_t p_opcode);
std::string format_instruction(const Instruction &p_instruction);

}
