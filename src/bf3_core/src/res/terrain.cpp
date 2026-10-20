#include "bf3/core/res/terrain.h"

#include "bf3/core/thread_pool.h"
#include "res_reader.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <tuple>

namespace bf3::core::res {

namespace {

constexpr uint8_t kMaxLevel = 16;
constexpr uint32_t kMaxSamplesPerSide = 4096;
constexpr size_t kParallelRows = 512;
constexpr uint32_t kChildX[4] = { 0, 1, 1, 0 };
constexpr uint32_t kChildY[4] = { 0, 0, 1, 1 };

TerrainNodeId child_of(const TerrainNodeId &p_id, int p_index) {
	return TerrainNodeId{ uint8_t(p_id.level + 1), 2 * p_id.x + kChildX[p_index], 2 * p_id.y + kChildY[p_index] };
}

struct Cursor {
	detail::Reader &r;
	uint64_t at = 0;

	Bytes take(uint64_t p_len) {
		const Bytes b = r.slice(at, p_len);
		if (r.ok()) {
			at += p_len;
		}
		return b;
	}
	uint8_t u8() {
		const uint8_t v = r.u8(at);
		at += 1;
		return v;
	}
	bool flag() { return u8() != 0; }
	uint32_t u32() {
		const uint32_t v = r.u32(at);
		at += 4;
		return v;
	}
	int32_t i32() { return int32_t(u32()); }
	float f32() {
		const uint32_t bits = u32();
		float f;
		std::memcpy(&f, &bits, 4);
		return f;
	}
	ChunkId chunk_id() {
		ChunkId id;
		const Bytes b = take(16);
		if (b.size() == 16) {
			std::memcpy(id.bytes.data(), b.data(), 16);
		}
		return id;
	}
};

struct HeightfieldParser {
	HeightfieldTree &tree;

	uint64_t min_max_samples() const {
		const uint64_t top = uint64_t(1) << (std::max<uint32_t>(tree.min_max_stack_depth, 1) - 1);
		uint64_t sum = 0;
		for (uint32_t level = 0; level < tree.min_max_stack_depth; ++level) {
			sum += (top >> level) * (top >> level) * 2;
		}
		return sum;
	}

	uint64_t occluder_samples() const {
		const uint64_t top = uint64_t(1) << (std::max<uint32_t>(tree.occluder_grid_stack_depth, 1) - 1);
		uint64_t sum = 0;
		for (uint32_t level = 0; level < tree.occluder_grid_stack_depth; ++level) {
			sum += ((top >> level) + 1) * ((top >> level) + 1);
		}
		return sum;
	}

	bool read_node(Cursor &c, const TerrainNodeId &p_id) {
		if (p_id.level > kMaxLevel) {
			return c.r.fail(ResError::malformed("heightfield depth", c.at));
		}
		float bounds[6];
		for (float &v : bounds) {
			v = c.f32();
		}
		if (!c.r.ok()) {
			return false;
		}
		const size_t index = tree.nodes.size();
		HeightfieldNode node;
		node.id = p_id;
		node.bounds_min = { bounds[0], bounds[1], bounds[2] };
		node.bounds_max = { bounds[3], bounds[4], bounds[5] };
		tree.nodes.push_back(std::move(node));
		const bool disabled = c.flag();
		if (!c.r.ok()) {
			return false;
		}
		if (disabled) {
			tree.nodes[index].disabled = true;
			return true;
		}
		const bool pruned = c.flag();
		const bool can_stream = c.flag();
		if (!c.r.ok()) {
			return false;
		}
		tree.nodes[index].pruned = pruned;
		tree.nodes[index].can_stream = can_stream;
		if (!pruned && !can_stream) {
			return true;
		}
		const bool persistent = c.flag();
		if (!c.r.ok()) {
			return false;
		}
		tree.nodes[index].persistent = persistent;
		if (can_stream && persistent) {
			const uint64_t samples = uint64_t(tree.samples_per_side) * tree.samples_per_side;
			const Bytes tile = c.take(2 * samples);
			if (!c.r.ok()) {
				return false;
			}
			std::vector<uint16_t> heights(static_cast<size_t>(samples));
			for (size_t i = 0; i < heights.size(); ++i) {
				heights[i] = uint16_t(tile[2 * i] | (tile[2 * i + 1] << 8));
			}
			tree.nodes[index].heights = std::move(heights);
			c.take(2 * min_max_samples());
			c.take(2 * occluder_samples());
			if (!c.r.ok()) {
				return false;
			}
		}
		const bool has_children = c.flag();
		if (!c.r.ok()) {
			return false;
		}
		if (has_children) {
			for (int i = 0; i < 4; ++i) {
				if (!read_node(c, child_of(p_id, i))) {
					return false;
				}
			}
		}
		return true;
	}
};

ResError parse_heightfield(Bytes p_payload, HeightfieldTree &r_tree, uint64_t &r_used) {
	detail::Reader reader(p_payload);
	Cursor c{ reader };
	HeightfieldTree tree;
	tree.samples_per_side = c.u32();
	c.u32();
	c.u32();
	c.i32();
	tree.world_size_y = c.f32();
	tree.physics_meters_per_sample = c.f32();
	c.i32();
	c.flag();
	c.f32();
	tree.min_max_stack_depth = c.u32();
	tree.occluder_grid_stack_depth = c.u32();
	c.u32();
	c.u32();
	c.u32();
	tree.border_width = c.u32();
	if (!reader.ok()) {
		return reader.error();
	}
	if (uint64_t(tree.samples_per_side) <= 2 * uint64_t(tree.border_width) || tree.min_max_stack_depth > 16) {
		return ResError::malformed("heightfield tile layout", 0);
	}
	if (tree.samples_per_side > kMaxSamplesPerSide) {
		return ResError::unsupported("heightfield tile side", tree.samples_per_side);
	}
	if (tree.occluder_grid_stack_depth > 16) {
		return ResError::unsupported("occluder grid stack depth", tree.occluder_grid_stack_depth);
	}
	HeightfieldParser parser{ tree };
	if (!parser.read_node(c, TerrainNodeId{})) {
		return reader.error();
	}
	r_used = c.at;
	r_tree = std::move(tree);
	return {};
}

bool read_streaming_node(Cursor &c, const TerrainNodeId &p_id, std::vector<TerrainStreamingNode> &r_out) {
	if (p_id.level > kMaxLevel) {
		return c.r.fail(ResError::malformed("streaming tree depth", c.at));
	}
	TerrainStreamingNode node;
	node.id = p_id;
	node.lod0_chunk_size = c.u32();
	node.lod0_chunk = c.chunk_id();
	if (c.flag()) {
		c.u32();
		c.chunk_id();
	}
	c.flag();
	const bool has_children = c.flag();
	if (!c.r.ok()) {
		return false;
	}
	r_out.push_back(node);
	if (has_children) {
		for (int i = 0; i < 4; ++i) {
			if (!read_streaming_node(c, child_of(p_id, i), r_out)) {
				return false;
			}
		}
	}
	return true;
}

struct Range {
	size_t start = 0;
	size_t end = 0;
};

Range covered_samples(const HeightfieldTree &p_tree, uint32_t p_index, uint8_t p_level, uint8_t p_depth, size_t p_len) {
	const size_t cells = p_tree.interior_cells();
	Range r;
	if (p_level <= p_depth) {
		const size_t region = cells << (p_depth - p_level);
		r.start = size_t(p_index) * region;
		r.end = r.start + region + 1;
	} else {
		const uint32_t shift = uint32_t(p_level - p_depth);
		const size_t start = size_t(p_index) * cells;
		const size_t unit = size_t(1) << shift;
		r.start = (start >> shift) + 1;
		r.end = (start + cells + unit - 1) / unit;
	}
	r.start = std::min(r.start, p_len);
	r.end = std::min(r.end, p_len);
	return r;
}

void paint_hole(const HeightfieldTree &p_tree, HeightGrid &r_grid, const HeightfieldNode &p_node, uint8_t p_depth) {
	const Range xs = covered_samples(p_tree, p_node.id.x, p_node.id.level, p_depth, r_grid.width);
	const Range zs = covered_samples(p_tree, p_node.id.y, p_node.id.level, p_depth, r_grid.height);
	if (xs.start >= xs.end) {
		return;
	}
	for (size_t z = zs.start; z < zs.end; ++z) {
		std::fill(r_grid.holes.begin() + ptrdiff_t(xs.start + z * r_grid.width), r_grid.holes.begin() + ptrdiff_t(xs.end + z * r_grid.width), uint8_t(1));
	}
}

void paint(const HeightfieldTree &p_tree, HeightGrid &r_grid, const HeightfieldNode &p_node, uint8_t p_depth, const std::vector<uint16_t> &p_tile) {
	const size_t step = size_t(1) << (p_depth - p_node.id.level);
	const size_t region = size_t(p_tree.interior_cells()) * step;
	const size_t x0 = size_t(p_node.id.x) * region;
	const size_t z0 = size_t(p_node.id.y) * region;
	const size_t side = p_tree.samples_per_side;
	const size_t border = p_tree.border_width;
	const auto sample = [&](size_t p_i, size_t p_j) {
		const size_t i = std::min(p_i + border, side - 1);
		const size_t j = std::min(p_j + border, side - 1);
		return p_tree.height_metres(p_tile[i + j * side]);
	};
	const auto row = [&](size_t p_dz) {
		const size_t z = z0 + p_dz;
		if (z >= r_grid.height) {
			return;
		}
		for (size_t dx = 0; dx <= region; ++dx) {
			const size_t x = x0 + dx;
			if (x >= r_grid.width) {
				break;
			}
			const size_t cell = x + z * r_grid.width;
			const size_t ix = dx / step;
			const float fx = float(dx % step) / float(step);
			const size_t iz = p_dz / step;
			const float fz = float(p_dz % step) / float(step);
			const float h00 = sample(ix, iz);
			float height = h00;
			if (!(fx == 0.0f && fz == 0.0f)) {
				const float h10 = sample(ix + 1, iz);
				const float h01 = sample(ix, iz + 1);
				const float h11 = sample(ix + 1, iz + 1);
				const float top = h00 + (h10 - h00) * fx;
				const float bottom = h01 + (h11 - h01) * fx;
				height = top + (bottom - top) * fz;
			}
			r_grid.heights[cell] = height;
			r_grid.holes[cell] = 0;
		}
	};

	if (region >= kParallelRows) {
		parallel_for(region + 1, row);
	} else {
		for (size_t dz = 0; dz <= region; ++dz) {
			row(dz);
		}
	}
}

}

ResError terrain_streaming_tree_name(Bytes p_terrain_res, std::string &r_name) {
	const auto end = std::find(p_terrain_res.begin(), p_terrain_res.end(), uint8_t(0));
	if (end == p_terrain_res.end()) {
		return ResError::malformed("unterminated string", 0);
	}
	const size_t len = size_t(end - p_terrain_res.begin());
	if (!detail::valid_utf8(p_terrain_res.data(), len)) {
		return ResError::malformed("string encoding", 0);
	}
	r_name.assign(reinterpret_cast<const char *>(p_terrain_res.data()), len);
	return {};
}

uint8_t HeightfieldTree::max_level() const {
	uint8_t level = 0;
	for (const HeightfieldNode &node : nodes) {
		level = std::max(level, node.id.level);
	}
	return level;
}

float HeightfieldTree::height_metres(uint16_t p_sample) const {
	return float(p_sample) * world_size_y / 65535.0f;
}

ResError TerrainStreamingTree::parse(Bytes p_res, TerrainStreamingTree &r_tree) {
	detail::Reader reader(p_res);
	Cursor c{ reader };
	TerrainStreamingTree tree;
	c.u32();
	c.flag();
	c.f32();
	c.f32();
	c.u32();
	c.u32();
	c.flag();
	for (;;) {
		const uint8_t kind = c.u8();
		if (!reader.ok()) {
			return reader.error();
		}
		if (kind == 0xFF) {
			break;
		}
		const uint32_t len = c.u32();
		const uint64_t offset = c.at;
		const Bytes payload = c.take(len);
		if (!reader.ok()) {
			return reader.error();
		}
		if (kind == 0) {
			HeightfieldTree heightfield;
			uint64_t used = 0;
			const ResError e = parse_heightfield(payload, heightfield, used);
			if (!e.ok()) {
				return e;
			}
			if (used != len) {
				return ResError::malformed("heightfield tree size", offset + used);
			}
			tree.heightfield = std::move(heightfield);
		}
	}
	if (!read_streaming_node(c, TerrainNodeId{}, tree.streaming_nodes)) {
		return reader.error();
	}
	r_tree = std::move(tree);
	return {};
}

ResError TerrainStreamingTree::height_grid(uint8_t p_depth, const std::function<std::optional<std::vector<uint8_t>>(const ChunkId &)> &p_fetch, HeightGrid &r_grid) const {
	if (!heightfield) {
		return ResError::malformed("streaming tree without a heightfield", 0);
	}
	const HeightfieldTree &tree = *heightfield;
	if (p_depth > kMaxLevel) {
		return ResError::unsupported("height grid depth", p_depth);
	}
	if (tree.nodes.empty()) {
		return ResError::malformed("empty heightfield tree", 0);
	}
	const HeightfieldNode &root = tree.nodes.front();
	const uint64_t cells = tree.interior_cells();
	const uint64_t side = (cells << p_depth) + 1;
	if (side > kMaxGridSide) {
		return ResError::unsupported("height grid side", uint32_t(std::min<uint64_t>(side, UINT32_MAX)));
	}
	const float span_x = root.bounds_max[0] - root.bounds_min[0];
	const float span_z = root.bounds_max[2] - root.bounds_min[2];
	HeightGrid grid;
	grid.origin_xz = { root.bounds_min[0], root.bounds_min[2] };
	grid.sample_spacing = { span_x / float(side - 1), span_z / float(side - 1) };
	grid.width = size_t(side);
	grid.height = size_t(side);
	grid.heights.assign(size_t(side * side), 0.0f);
	grid.holes.assign(size_t(side * side), uint8_t(1));

	std::map<std::tuple<uint8_t, uint32_t, uint32_t>, const TerrainStreamingNode *> streamed;
	for (const TerrainStreamingNode &node : streaming_nodes) {
		streamed[{ node.id.level, node.id.x, node.id.y }] = &node;
	}
	std::vector<const HeightfieldNode *> order;
	for (const HeightfieldNode &node : tree.nodes) {
		if (node.id.level <= p_depth || node.disabled) {
			order.push_back(&node);
		}
	}
	std::stable_sort(order.begin(), order.end(), [](const HeightfieldNode *p_a, const HeightfieldNode *p_b) {

		if (p_a->id.level != p_b->id.level) {
			return p_a->id.level < p_b->id.level;
		}
		return p_a->disabled && !p_b->disabled;
	});
	const size_t samples = size_t(tree.samples_per_side) * tree.samples_per_side;
	std::vector<uint16_t> fetched;
	for (const HeightfieldNode *node : order) {
		if (node->disabled) {
			paint_hole(tree, grid, *node, p_depth);
			continue;
		}
		const std::vector<uint16_t> *tile = nullptr;
		if (node->heights) {
			tile = &*node->heights;
		} else if (node->can_stream) {
			const auto it = streamed.find({ node->id.level, node->id.x, node->id.y });
			if (it != streamed.end()) {
				const std::optional<std::vector<uint8_t>> chunk = p_fetch(it->second->lod0_chunk);
				if (chunk) {
					if (chunk->size() < 2 * samples) {
						return ResError::truncated(0, 2 * samples, chunk->size());
					}
					fetched.resize(samples);
					for (size_t i = 0; i < samples; ++i) {
						fetched[i] = uint16_t((*chunk)[2 * i] | ((*chunk)[2 * i + 1] << 8));
					}
					tile = &fetched;
				}
			}
		}
		if (tile != nullptr) {
			paint(tree, grid, *node, p_depth, *tile);
		}
	}
	r_grid = std::move(grid);
	return {};
}

}
