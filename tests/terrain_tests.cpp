#include <doctest/doctest.h>

#include "bf3/core/res/terrain.h"

#include <cmath>
#include <cstring>
#include <random>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::res;

namespace {

struct Writer {
	std::vector<uint8_t> bytes;

	void u8(uint8_t p_v) { bytes.push_back(p_v); }
	void u16(uint16_t p_v) {
		u8(uint8_t(p_v));
		u8(uint8_t(p_v >> 8));
	}
	void u32(uint32_t p_v) {
		for (int i = 0; i < 4; ++i) {
			u8(uint8_t(p_v >> (8 * i)));
		}
	}
	void f32(float p_v) {
		uint32_t bits;
		std::memcpy(&bits, &p_v, 4);
		u32(bits);
	}
	void raw(const std::vector<uint8_t> &p_b) { bytes.insert(bytes.end(), p_b.begin(), p_b.end()); }
};

constexpr uint32_t kSide = 5;
constexpr uint16_t kRoot = 1000;
constexpr uint16_t kStreamed = 2000;
constexpr uint16_t kEmbedded = 3000;

void bounds(Writer &w) {
	for (float v : { -8.0f, 0.0f, -8.0f, 8.0f, 100.0f, 8.0f }) {
		w.f32(v);
	}
}

void embedded_tile(Writer &w, uint16_t p_value) {
	for (uint32_t i = 0; i < kSide * kSide; ++i) {
		w.u16(p_value);
	}
	for (int i = 0; i < 2; ++i) {
		w.u16(0);
	}
	for (int i = 0; i < 4; ++i) {
		w.u16(0);
	}
}

std::vector<uint8_t> heightfield() {
	Writer w;
	w.u32(kSide);
	w.u32(0);
	w.u32(0);
	w.u32(0);
	w.f32(65535.0f);
	w.f32(1.0f);
	w.u32(0);
	w.u8(0);
	w.f32(0.0f);
	w.u32(1);
	w.u32(1);
	w.u32(5);
	w.u32(2);
	w.u32(0);
	w.u32(1);

	bounds(w);
	w.u8(0);
	w.u8(0);
	w.u8(1);
	w.u8(1);
	embedded_tile(w, kRoot);
	w.u8(1);

	bounds(w);
	w.u8(1);

	bounds(w);
	w.u8(0);
	w.u8(0);
	w.u8(1);
	w.u8(0);
	w.u8(0);

	bounds(w);
	w.u8(0);
	w.u8(0);
	w.u8(0);

	bounds(w);
	w.u8(0);
	w.u8(0);
	w.u8(1);
	w.u8(1);
	embedded_tile(w, kEmbedded);
	w.u8(0);
	return w.bytes;
}

void streaming_node(Writer &w, uint8_t p_chunk, bool p_children) {
	w.u32(64);
	for (int i = 0; i < 16; ++i) {
		w.u8(i == 0 ? p_chunk : 0);
	}
	w.u8(0);
	w.u8(0);
	w.u8(p_children ? 1 : 0);
}

std::vector<uint8_t> streaming_tree() {
	Writer w;
	w.u32(7);
	w.u8(0);
	w.f32(1.0f);
	w.f32(1.0f);
	w.u32(0);
	w.u32(5);
	w.u8(0);
	w.u8(1);
	w.u32(3);
	w.raw({ 9, 9, 9 });
	const std::vector<uint8_t> h = heightfield();
	w.u8(0);
	w.u32(uint32_t(h.size()));
	w.raw(h);
	w.u8(0xFF);
	streaming_node(w, 0x10, true);
	streaming_node(w, 0x20, false);
	streaming_node(w, 0x21, false);
	streaming_node(w, 0x22, false);
	streaming_node(w, 0x23, false);
	return w.bytes;
}

std::optional<std::vector<uint8_t>> fetch(const ChunkId &p_id) {
	if (p_id.bytes[0] != 0x21) {
		return std::nullopt;
	}
	Writer w;
	for (uint32_t i = 0; i < kSide * kSide; ++i) {
		w.u16(kStreamed);
	}
	w.raw({ 1, 2, 3, 4 });
	return w.bytes;
}

}

TEST_SUITE("collision_unit") {
	TEST_CASE("terrain: streaming tree heightfield, nodes painted by level, holes and shared edges") {
		Writer name;
		for (char c : std::string("Levels/X/Terrain.StreamingTree")) {
			name.u8(uint8_t(c));
		}
		name.u8(0);
		std::string tree_name;
		REQUIRE(terrain_streaming_tree_name(name.bytes, tree_name).ok());
		CHECK(tree_name == "Levels/X/Terrain.StreamingTree");
		CHECK_FALSE(terrain_streaming_tree_name(Bytes(name.bytes.data(), name.bytes.size() - 1), tree_name).ok());

		const std::vector<uint8_t> bytes = streaming_tree();
		TerrainStreamingTree tree;
		REQUIRE(TerrainStreamingTree::parse(bytes, tree).ok());
		REQUIRE(tree.heightfield);
		CHECK(tree.heightfield->nodes.size() == 5);
		CHECK(tree.heightfield->interior_cells() == 2);
		CHECK(tree.heightfield->max_level() == 1);
		CHECK(tree.streaming_nodes.size() == 5);
		CHECK(tree.streaming_nodes[2].id == TerrainNodeId{ 1, 1, 0 });

		HeightGrid root;
		REQUIRE(tree.height_grid(0, fetch, root).ok());
		CHECK(root.width == 3);
		CHECK(root.sample_spacing[0] == 8.0f);
		for (size_t i = 0; i < 9; ++i) {
			CHECK(std::fabs(root.heights[i] - float(kRoot)) < 1e-3f);
			CHECK(root.holes[i] == 0);
		}

		HeightGrid grid;
		REQUIRE(tree.height_grid(1, fetch, grid).ok());
		REQUIRE(grid.width == 5);
		REQUIRE(grid.height == 5);
		CHECK(grid.origin_xz[0] == -8.0f);
		CHECK(grid.sample_spacing[1] == 4.0f);
		for (size_t z = 0; z < 5; ++z) {
			for (size_t x = 0; x < 5; ++x) {
				CAPTURE(x);
				CAPTURE(z);

				CHECK(grid.hole(x, z) == (x < 2 && z < 2));
				if (grid.hole(x, z)) {
					continue;
				}

				const float want = (x <= 2 && z >= 2) ? float(kEmbedded) : (x >= 2 && z <= 2) ? float(kStreamed) : float(kRoot);
				CHECK(std::fabs(grid.at(x, z) - want) < 1e-3f);
			}
		}

		HeightGrid unused;
		CHECK_FALSE(tree.height_grid(17, fetch, unused).ok());
		const auto short_chunk = [](const ChunkId &) -> std::optional<std::vector<uint8_t>> { return std::vector<uint8_t>(10, 0); };
		CHECK_FALSE(tree.height_grid(1, short_chunk, unused).ok());
	}

	TEST_CASE("terrain: truncated and bit-flipped streaming trees fail cleanly") {
		const std::vector<uint8_t> bytes = streaming_tree();
		size_t refused = 0;
		for (size_t len = 0; len < bytes.size(); ++len) {
			TerrainStreamingTree tree;
			refused += TerrainStreamingTree::parse(Bytes(bytes.data(), len), tree).ok() ? 0 : 1;
		}
		CHECK(refused == bytes.size());
		std::mt19937 rng(20261015u);
		size_t parsed = 0, grids = 0;
		for (int mutant = 0; mutant < 3000; ++mutant) {
			std::vector<uint8_t> m = bytes;
			const int flips = 1 + int(rng() % 8);
			for (int f = 0; f < flips; ++f) {
				m[rng() % m.size()] ^= uint8_t(1u << (rng() % 8));
			}
			TerrainStreamingTree tree;
			if (!TerrainStreamingTree::parse(m, tree).ok()) {
				continue;
			}
			++parsed;
			for (uint8_t depth = 0; depth <= 2; ++depth) {
				HeightGrid grid;
				if (tree.height_grid(depth, fetch, grid).ok()) {
					++grids;
					CHECK(grid.width <= kMaxGridSide);
					CHECK(grid.heights.size() == grid.width * grid.height);
					CHECK(grid.holes.size() == grid.heights.size());
				}
			}
		}
		MESSAGE("terrain mutants: " << parsed << " parsed, " << grids << " grids");
		CHECK(parsed > 0);
	}
}
