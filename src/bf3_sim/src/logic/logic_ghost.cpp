#include "bf3/sim/logic/logic_runtime.h"

#include "bf3/core/gameplay/logic_types.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <tuple>

namespace bf3::sim::logic {

namespace gp = core::gameplay;

core::Result<LogicGhostLayout> LogicGhostLayout::of(const LogicGraph &p_graph) {
	LogicGhostLayout out;
	const auto order = [&](std::vector<uint32_t> &r_list) {
		std::sort(r_list.begin(), r_list.end(), [&](uint32_t a, uint32_t b) {
			const gp::LogicNode &na = p_graph.nodes()[a];
			const gp::LogicNode &nb = p_graph.nodes()[b];
			const uint64_t pa = p_graph.scopes()[na.scope].path;
			const uint64_t pb = p_graph.scopes()[nb.scope].path;
			return std::tie(pa, na.guid, a) < std::tie(pb, nb.guid, b);
		});
	};
	for (const uint32_t i : p_graph.nodes_of_type(gp::LogicTypeId::SyncedBool)) {
		out.bools.push_back(i);
	}
	for (const uint32_t i : p_graph.nodes_of_type(gp::LogicTypeId::SyncedTransform)) {
		out.transforms.push_back(i);
	}
	for (const uint32_t i : p_graph.nodes_of_type(gp::LogicTypeId::SyncedSequence)) {
		out.sequences.push_back(i);
	}
	for (const uint32_t i : p_graph.nodes_of_type(gp::LogicTypeId::Sequence)) {
		if (p_graph.nodes()[i].realm == gp::LogicRealm::ClientAndServer) {
			out.sequences.push_back(i);
		}
	}
	for (const auto *list : { &out.bools, &out.transforms, &out.sequences }) {
		if (list->size() > kMaxGhostEntries) {
			return core::make_error("logic ghost: " + std::to_string(list->size()) + " entries in one list (more than " + std::to_string(kMaxGhostEntries) + ")");
		}
	}
	order(out.bools);
	order(out.transforms);
	order(out.sequences);
	return out;
}

namespace {

void put16(std::vector<uint8_t> &r_out, uint32_t p_v) {
	r_out.push_back(static_cast<uint8_t>(p_v));
	r_out.push_back(static_cast<uint8_t>(p_v >> 8));
}

void put32(std::vector<uint8_t> &r_out, uint32_t p_v) {
	for (int i = 0; i < 4; ++i) {
		r_out.push_back(static_cast<uint8_t>(p_v >> (8 * i)));
	}
}

uint32_t get16(std::span<const uint8_t> p_b, size_t p_at) {
	return uint32_t(p_b[p_at]) | (uint32_t(p_b[p_at + 1]) << 8);
}

uint32_t get32(std::span<const uint8_t> p_b, size_t p_at) {
	return uint32_t(p_b[p_at]) | (uint32_t(p_b[p_at + 1]) << 8) | (uint32_t(p_b[p_at + 2]) << 16) | (uint32_t(p_b[p_at + 3]) << 24);
}

bool bit(const std::vector<uint64_t> &p_bits, size_t p_i) {
	return p_i / 64 < p_bits.size() && ((p_bits[p_i / 64] >> (p_i % 64)) & 1u) != 0;
}

}

std::vector<uint8_t> LogicGhost::encode() const {
	const size_t n = std::min(bool_count, kMaxGhostEntries);
	std::vector<uint8_t> out;
	out.reserve(10 + (n + 7) / 8 + transforms.size() * 48 + sequence_start.size() * 4);
	put32(out, tick);
	put16(out, static_cast<uint32_t>(n));
	put16(out, static_cast<uint32_t>(std::min(transforms.size(), kMaxGhostEntries)));
	put16(out, static_cast<uint32_t>(std::min(sequence_start.size(), kMaxGhostEntries)));
	for (size_t byte = 0; byte < (n + 7) / 8; ++byte) {
		uint8_t v = 0;
		for (size_t k = 0; k < 8 && byte * 8 + k < n; ++k) {
			v |= static_cast<uint8_t>(bit(bool_bits, byte * 8 + k) ? 1u << k : 0u);
		}
		out.push_back(v);
	}
	for (size_t t = 0; t < std::min(transforms.size(), kMaxGhostEntries); ++t) {
		for (const float f : transforms[t]) {
			uint32_t u = 0;
			std::memcpy(&u, &f, 4);
			put32(out, u);
		}
	}
	for (size_t s = 0; s < std::min(sequence_start.size(), kMaxGhostEntries); ++s) {
		put32(out, sequence_start[s]);
	}
	return out;
}

core::Result<LogicGhost> LogicGhost::decode(std::span<const uint8_t> p_bytes, const LogicGhostLayout &p_layout) {
	if (p_bytes.size() < 10) {
		return core::make_error("logic ghost: " + std::to_string(p_bytes.size()) + " bytes, shorter than the header");
	}
	const size_t bools = get16(p_bytes, 4), transforms = get16(p_bytes, 6), sequences = get16(p_bytes, 8);
	if (bools != p_layout.bools.size() || transforms != p_layout.transforms.size() || sequences != p_layout.sequences.size()) {
		return core::make_error("logic ghost: counts " + std::to_string(bools) + "/" + std::to_string(transforms) + "/" + std::to_string(sequences) +
				" differ from the layout's");
	}
	const size_t expect = 10 + (bools + 7) / 8 + transforms * 48 + sequences * 4;
	if (p_bytes.size() != expect) {
		return core::make_error("logic ghost: " + std::to_string(p_bytes.size()) + " bytes, expected " + std::to_string(expect));
	}
	LogicGhost g;
	g.tick = get32(p_bytes, 0);
	g.bool_count = bools;
	g.bool_bits.assign((bools + 63) / 64, 0);
	size_t at = 10;
	for (size_t i = 0; i < bools; ++i) {
		if ((p_bytes[at + i / 8] >> (i % 8)) & 1u) {
			g.bool_bits[i / 64] |= uint64_t(1) << (i % 64);
		}
	}

	if (bools % 8 != 0 && (p_bytes[at + bools / 8] >> (bools % 8)) != 0) {
		return core::make_error("logic ghost: padding bits set");
	}
	at += (bools + 7) / 8;
	g.transforms.resize(transforms);
	for (size_t t = 0; t < transforms; ++t) {
		for (size_t k = 0; k < 12; ++k) {
			const uint32_t u = get32(p_bytes, at);
			std::memcpy(&g.transforms[t][k], &u, 4);
			at += 4;
		}
	}
	g.sequence_start.resize(sequences);
	for (size_t s = 0; s < sequences; ++s) {
		g.sequence_start[s] = get32(p_bytes, at);
		at += 4;
	}
	return g;
}

std::vector<LogicGhost::BoolDelta> LogicGhost::bool_delta(const LogicGhost &p_base) const {
	std::vector<BoolDelta> out;
	const size_t n = std::min(bool_count, kMaxGhostEntries);
	for (size_t i = 0; i < n; ++i) {
		const bool now = bit(bool_bits, i);
		if (now != bit(p_base.bool_bits, i)) {
			out.push_back({ static_cast<uint16_t>(i), now });
		}
	}
	return out;
}

}
