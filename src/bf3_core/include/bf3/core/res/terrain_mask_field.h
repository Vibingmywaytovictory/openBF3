#pragma once

#include "bf3/core/res/terrain.h"

#include <unordered_map>

namespace bf3::core::res {

class TerrainMaskField {
public:
	static ResError build(
			const TerrainStreamingTree &p_tree, const TerrainChunkFetch &p_fetch, TerrainMaskField &r_field);

	uint8_t layers() const { return layers_; }
	const std::array<float, 4> &bounds() const { return bounds_; }
	size_t records() const { return records_.size(); }
	uint64_t bytes() const { return tiles_.size(); }

	float coverage(uint8_t p_layer, float p_x, float p_z) const;

private:
	static constexpr uint32_t kFull = 0xFFFFFFFFu;
	struct Record {
		std::array<float, 4> rect{};
		uint32_t tile = kFull;
	};

	static uint64_t key(uint8_t p_layer, uint8_t p_size_level, uint32_t p_x, uint32_t p_z);

	uint32_t side_ = 0;
	uint8_t layers_ = 0;
	uint8_t deepest_ = 0;
	std::array<float, 4> bounds_{};
	std::vector<uint8_t> tiles_;
	std::unordered_map<uint64_t, Record> records_;
};

}
