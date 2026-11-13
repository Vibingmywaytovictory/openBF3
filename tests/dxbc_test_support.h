#pragma once

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>

namespace dxbct {

constexpr uint32_t kMaskMode = 0, kSwizzleMode = 1;
constexpr uint32_t kTemp = 0, kInput = 1, kOutput = 2, kImmediate = 4, kSampler = 6, kResource = 7, kConstantBuffer = 8;
constexpr uint32_t kIdentity = 0 | 1 << 2 | 2 << 4 | 3 << 6;

inline uint32_t swz(uint32_t p_x, uint32_t p_y, uint32_t p_z, uint32_t p_w) {
	return p_x | p_y << 2 | p_z << 4 | p_w << 6;
}

inline std::vector<uint32_t> reg(uint32_t p_type, uint32_t p_index, uint32_t p_mode, uint32_t p_bits) {
	return { 2u | p_mode << 2 | p_bits << 4 | p_type << 12 | 1u << 20, p_index };
}

inline std::vector<uint32_t> dst(uint32_t p_type, uint32_t p_index, uint32_t p_mask) {
	return reg(p_type, p_index, kMaskMode, p_mask);
}

inline std::vector<uint32_t> src(uint32_t p_type, uint32_t p_index, uint32_t p_swizzle = kIdentity) {
	return reg(p_type, p_index, kSwizzleMode, p_swizzle);
}

inline std::vector<uint32_t> cb(uint32_t p_slot, uint32_t p_index, uint32_t p_swizzle) {
	return { 2u | kSwizzleMode << 2 | p_swizzle << 4 | kConstantBuffer << 12 | 2u << 20, p_slot, p_index };
}

inline std::vector<uint32_t> imm(float p_x, float p_y, float p_z, float p_w) {
	std::vector<uint32_t> out = { 2u | kImmediate << 12 };
	for (float f : { p_x, p_y, p_z, p_w }) {
		uint32_t u;
		std::memcpy(&u, &f, 4);
		out.push_back(u);
	}
	return out;
}

inline std::vector<uint32_t> sampler(uint32_t p_index) {
	return { kSampler << 12 | 1u << 20, p_index };
}

inline std::vector<uint32_t> ins(
		uint32_t p_opcode, std::initializer_list<std::vector<uint32_t>> p_operands, bool p_saturate = false) {
	std::vector<uint32_t> out = { 0 };
	for (const std::vector<uint32_t> &o : p_operands) {
		out.insert(out.end(), o.begin(), o.end());
	}
	out[0] = p_opcode | (p_saturate ? 1u << 13 : 0u) | uint32_t(out.size()) << 24;
	return out;
}

inline void put(std::vector<uint8_t> &r_out, uint32_t p_v) {
	for (int i = 0; i < 4; ++i) {
		r_out.push_back(uint8_t(p_v >> (8 * i)));
	}
}

inline void put_at(std::vector<uint8_t> &r_out, size_t p_at, uint32_t p_v) {
	std::memcpy(r_out.data() + p_at, &p_v, 4);
}

struct SigEntry {
	const char *name;
	uint32_t index;
	uint32_t system;
	uint32_t reg;
	uint8_t mask;
};

inline std::vector<uint8_t> signature(const std::vector<SigEntry> &p_entries) {
	std::vector<uint8_t> out;
	put(out, uint32_t(p_entries.size()));
	put(out, 8);
	const size_t names = 8 + 24 * p_entries.size();
	std::vector<uint8_t> text;
	for (const SigEntry &e : p_entries) {
		put(out, uint32_t(names + text.size()));
		put(out, e.index);
		put(out, e.system);
		put(out, 3);
		put(out, e.reg);
		out.push_back(e.mask);
		out.push_back(e.mask);
		out.push_back(0);
		out.push_back(0);
		text.insert(text.end(), e.name, e.name + std::strlen(e.name) + 1);
	}
	out.insert(out.end(), text.begin(), text.end());
	return out;
}

struct BindEntry {
	const char *name;
	uint32_t type;
	uint32_t point;
	uint32_t dimension = 4;
};

struct VarEntry {
	const char *name;
	uint32_t offset;
	uint32_t size = 4;
};

inline std::vector<uint8_t> rdef(
		const std::vector<BindEntry> &p_bindings, const char *p_buffer, const std::vector<VarEntry> &p_variables) {
	std::vector<uint8_t> out(28, 0);
	const bool with_buffer = p_buffer != nullptr;
	put_at(out, 0, with_buffer ? 1 : 0);
	put_at(out, 8, uint32_t(p_bindings.size()));
	put_at(out, 12, 28);
	out[16] = 1;
	out[17] = 4;
	const size_t bindings_at = 28, buffer_at = bindings_at + 32 * p_bindings.size(),
				 variables_at = buffer_at + (with_buffer ? 24 : 0);
	const size_t text_at = variables_at + 24 * p_variables.size();
	put_at(out, 4, uint32_t(buffer_at));
	out.resize(text_at, 0);
	std::vector<uint8_t> text;
	const auto name = [&](const char *p_s) {
		const uint32_t at = uint32_t(text_at + text.size());
		text.insert(text.end(), p_s, p_s + std::strlen(p_s) + 1);
		return at;
	};
	for (size_t i = 0; i < p_bindings.size(); ++i) {
		const size_t at = bindings_at + 32 * i;
		put_at(out, at, name(p_bindings[i].name));
		put_at(out, at + 4, p_bindings[i].type);
		put_at(out, at + 12, p_bindings[i].dimension);
		put_at(out, at + 20, p_bindings[i].point);
		put_at(out, at + 24, 1);
	}
	if (with_buffer) {
		put_at(out, buffer_at, name(p_buffer));
		put_at(out, buffer_at + 4, uint32_t(p_variables.size()));
		put_at(out, buffer_at + 8, uint32_t(variables_at));
		put_at(out, buffer_at + 12, 16);
		for (size_t v = 0; v < p_variables.size(); ++v) {
			const size_t at = variables_at + 24 * v;
			put_at(out, at, name(p_variables[v].name));
			put_at(out, at + 4, p_variables[v].offset);
			put_at(out, at + 8, p_variables[v].size);
		}
	}
	out.insert(out.end(), text.begin(), text.end());
	return out;
}

inline std::vector<uint8_t> words(const std::vector<uint32_t> &p_words) {
	std::vector<uint8_t> out;
	for (uint32_t w : p_words) {
		put(out, w);
	}
	return out;
}

inline std::vector<uint8_t> code_chunk(
		uint32_t p_kind, uint32_t p_temps, const std::vector<std::vector<uint32_t>> &p_code) {
	std::vector<uint32_t> tokens = { 1u | 4u << 4 | p_kind << 16, 0, 104u | 2u << 24, p_temps };
	for (const std::vector<uint32_t> &i : p_code) {
		tokens.insert(tokens.end(), i.begin(), i.end());
	}
	tokens.push_back(62u | 1u << 24);
	tokens[1] = uint32_t(tokens.size());
	return words(tokens);
}

inline std::vector<uint8_t> container(const std::vector<std::pair<const char *, std::vector<uint8_t>>> &p_chunks) {
	std::vector<uint8_t> out = { 'D', 'X', 'B', 'C' };
	out.resize(32, 0);
	put_at(out, 24, 0);
	put_at(out, 28, uint32_t(p_chunks.size()));
	const size_t table = out.size();
	out.resize(table + 4 * p_chunks.size(), 0);
	for (size_t i = 0; i < p_chunks.size(); ++i) {
		put_at(out, table + 4 * i, uint32_t(out.size()));
		out.insert(out.end(), p_chunks[i].first, p_chunks[i].first + 4);
		put(out, uint32_t(p_chunks[i].second.size()));
		out.insert(out.end(), p_chunks[i].second.begin(), p_chunks[i].second.end());
	}
	put_at(out, 24, uint32_t(out.size()));
	return out;
}

}
