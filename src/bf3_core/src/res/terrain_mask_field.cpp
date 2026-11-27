#include "bf3/core/res/terrain_mask_field.h"

#include <algorithm>
#include <cmath>

namespace bf3::core::res {

namespace {

constexpr uint8_t kMaxSizeLevel = 20;
constexpr uint64_t kMaxFieldBytes = uint64_t(1) << 28;

}

uint64_t TerrainMaskField::key(uint8_t p_layer, uint8_t p_size_level, uint32_t p_x, uint32_t p_z) {
	return (uint64_t(p_layer) << 56) | (uint64_t(p_size_level) << 48) | (uint64_t(p_x & 0xFFFFFFu) << 24) |
			uint64_t(p_z & 0xFFFFFFu);
}

ResError TerrainMaskField::build(
		const TerrainStreamingTree &p_tree, const TerrainChunkFetch &p_fetch, TerrainMaskField &r_field) {
	if (!p_tree.mask) {
		return ResError::malformed("streaming tree without a mask layer", 0);
	}
	const TerrainMaskTree &tree = *p_tree.mask;
	TerrainMaskField field;
	field.side_ = tree.samples_per_side;
	field.layers_ = tree.layer_count();
	field.bounds_ = tree.bounds;
	const double span_x = double(tree.bounds[2]) - double(tree.bounds[0]);
	const double span_z = double(tree.bounds[3]) - double(tree.bounds[1]);
	const size_t tile_bytes = tree.tile_bytes();
	for (const TerrainMaskNode &node : tree.nodes) {
		if (node.records.empty()) {
			continue;
		}
		std::optional<std::vector<uint8_t>> chunk;
		const uint8_t *tiles = nullptr;
		if (node.tiles > 0) {
			if (node.persistent) {
				tiles = node.persistent->data();
			} else {
				TerrainChunkLayout layout;
				const ResError e = p_tree.chunk_layout(node.id, layout);
				if (!e.ok()) {
					return e;
				}
				chunk = p_fetch(layout.chunk);
				if (!chunk) {
					return ResError::malformed("mask chunk missing", node.id.level);
				}
				if (chunk->size() < layout.total) {
					return ResError::truncated(layout.mask_offset, layout.total, chunk->size());
				}
				tiles = chunk->data() + layout.mask_offset;
			}
		}
		uint32_t stored = 0;
		for (const TerrainMaskRecord &r : node.records) {
			const double size = double(r.rect[2]) - double(r.rect[0]);
			const double size_z = double(r.rect[3]) - double(r.rect[1]);
			const double level = std::log2(span_x / size);
			const double rounded = std::round(level);
			if (!(size > 0.0) || std::fabs(level - rounded) > 1e-3 || rounded < 0.0 || rounded > kMaxSizeLevel ||
					std::fabs(size_z / span_z - size / span_x) > 1e-6) {
				return ResError::unsupported("mask record rect", node.id.level);
			}
			Record record;
			record.rect = r.rect;
			if (!r.full) {
				if (field.tiles_.size() + tile_bytes > kMaxFieldBytes) {
					return ResError::unsupported("mask field bytes", uint32_t(field.tiles_.size() >> 20));
				}
				record.tile = uint32_t(field.tiles_.size() / tile_bytes);
				const uint8_t *tile = tiles + size_t(stored++) * tile_bytes;
				field.tiles_.insert(field.tiles_.end(), tile, tile + tile_bytes);
			}
			const uint8_t size_level = uint8_t(rounded);
			const uint32_t x = uint32_t(std::llround((double(r.rect[0]) - double(tree.bounds[0])) / size));
			const uint32_t z = uint32_t(std::llround((double(r.rect[1]) - double(tree.bounds[1])) / size_z));
			field.records_[key(r.layer, size_level, x, z)] = record;
			field.deepest_ = std::max(field.deepest_, size_level);
		}
	}
	r_field = std::move(field);
	return {};
}

float TerrainMaskField::coverage(uint8_t p_layer, float p_x, float p_z) const {
	if (!(p_x >= bounds_[0]) || !(p_x < bounds_[2]) || !(p_z >= bounds_[1]) || !(p_z < bounds_[3])) {
		return 0.0f;
	}
	const double fx = (double(p_x) - double(bounds_[0])) / (double(bounds_[2]) - double(bounds_[0]));
	const double fz = (double(p_z) - double(bounds_[1])) / (double(bounds_[3]) - double(bounds_[1]));
	// INFERRED: the finest record at the point gives the coverage, bilinear over its tile with the border texels split
	// evenly on both sides. Settle: a note on how the terrain samples its mask tree.
	for (int level = deepest_; level >= 0; --level) {
		const double cells = double(uint32_t(1) << level);
		const auto found = records_.find(key(p_layer, uint8_t(level), uint32_t(fx * cells), uint32_t(fz * cells)));
		if (found == records_.end()) {
			continue;
		}
		const Record &record = found->second;
		if (record.tile == kFull) {
			return float(kTerrainFullMask);
		}
		const uint8_t *tile = tiles_.data() + size_t(record.tile) * side_ * side_;
		const double interior = double(side_ - kTerrainMaskBorder);
		const double shift = double(kTerrainMaskBorder) * 0.5 - 0.5;
		const int last = int(side_) - 1;
		const double u = std::clamp(
				(double(p_x) - double(record.rect[0])) * interior / (double(record.rect[2]) - double(record.rect[0])) +
						shift,
				0.0, double(last));
		const double v = std::clamp(
				(double(p_z) - double(record.rect[1])) * interior / (double(record.rect[3]) - double(record.rect[1])) +
						shift,
				0.0, double(last));
		const int i0 = std::min(int(u), last - 1);
		const int j0 = std::min(int(v), last - 1);
		const double tx = u - double(i0);
		const double tz = v - double(j0);
		const auto at = [&](int p_i, int p_j) { return double(tile[size_t(p_j) * side_ + size_t(p_i)]); };
		const double top = at(i0, j0) + (at(i0 + 1, j0) - at(i0, j0)) * tx;
		const double bottom = at(i0, j0 + 1) + (at(i0 + 1, j0 + 1) - at(i0, j0 + 1)) * tx;
		return float(top + (bottom - top) * tz);
	}
	return 0.0f;
}

}
