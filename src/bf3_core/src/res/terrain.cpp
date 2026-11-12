#include "bf3/core/res/terrain.h"

#include "bf3/core/res/dx_texture.h"
#include "bf3/core/thread_pool.h"
#include "res_reader.h"

#include <algorithm>
#include <atomic>
#include <cmath>
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
constexpr int kChildrenChunkOrder[4] = { 3, 2, 1, 0 };

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
	uint16_t u16() {
		const uint16_t v = r.u16(at);
		at += 2;
		return v;
	}
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

uint64_t min_max_samples(const HeightfieldTree &p_tree) {
	const uint64_t top = uint64_t(1) << (std::max<uint32_t>(p_tree.min_max_stack_depth, 1) - 1);
	uint64_t sum = 0;
	for (uint32_t level = 0; level < p_tree.min_max_stack_depth; ++level) {
		sum += (top >> level) * (top >> level) * 2;
	}
	return sum;
}

uint64_t occluder_samples(const HeightfieldTree &p_tree) {
	const uint64_t top = uint64_t(1) << (std::max<uint32_t>(p_tree.occluder_grid_stack_depth, 1) - 1);
	uint64_t sum = 0;
	for (uint32_t level = 0; level < p_tree.occluder_grid_stack_depth; ++level) {
		sum += ((top >> level) + 1) * ((top >> level) + 1);
	}
	return sum;
}

uint64_t height_tile_bytes(const HeightfieldTree &p_tree) {
	return 2 *
			(uint64_t(p_tree.samples_per_side) * p_tree.samples_per_side + min_max_samples(p_tree) +
					occluder_samples(p_tree));
}

struct HeightfieldParser {
	HeightfieldTree &tree;

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
			c.take(2 * min_max_samples(tree));
			c.take(2 * occluder_samples(tree));
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

constexpr uint32_t kMaxMaskSide = 1024;
constexpr uint32_t kMaxColourSide = 1024;
constexpr uint32_t kMaxMaskRecords = 4096;
constexpr uint32_t kMaxCoverageBytes = 4096;

struct LayerHeader {
	uint32_t side = 0;
	std::array<float, 4> bounds{};
};

LayerHeader read_layer_header(Cursor &c) {
	LayerHeader h;
	h.side = c.u32();
	c.u32();
	c.u32();
	c.u32();
	c.u32();
	for (float &v : h.bounds) {
		v = c.f32();
	}
	return h;
}

bool bounds_ok(const std::array<float, 4> &p_bounds) {
	return std::isfinite(p_bounds[0]) && std::isfinite(p_bounds[1]) && std::isfinite(p_bounds[2]) &&
			std::isfinite(p_bounds[3]) && p_bounds[2] > p_bounds[0] && p_bounds[3] > p_bounds[1];
}

bool read_mask_node(Cursor &c, const TerrainNodeId &p_id, TerrainMaskTree &r_tree) {
	if (p_id.level > kMaxLevel) {
		return c.r.fail(ResError::malformed("mask tree depth", c.at));
	}
	const uint8_t disabled = c.u8();
	const uint16_t records = c.u16();
	const uint16_t tiles = c.u16();
	c.u16();
	if (!c.r.ok()) {
		return false;
	}
	if (disabled != 0) {
		return c.r.fail(ResError::unsupported("disabled mask node", disabled));
	}
	if (records > kMaxMaskRecords) {
		return c.r.fail(ResError::unsupported("mask records per node", records));
	}
	const size_t index = r_tree.nodes.size();
	r_tree.nodes.push_back(TerrainMaskNode{ p_id, {}, tiles, std::nullopt });
	if (records == 0) {
		return true;
	}
	std::vector<TerrainMaskRecord> list(records);
	uint32_t stored = 0;
	for (TerrainMaskRecord &rec : list) {
		rec.layer = c.u8();
		for (float &v : rec.rect) {
			v = c.f32();
		}
		rec.refined = c.flag();
		rec.full = c.flag();
		const uint16_t bytes = c.u16();
		if (!c.r.ok()) {
			return false;
		}
		if (bytes > kMaxCoverageBytes) {
			return c.r.fail(ResError::unsupported("mask coverage bytes", bytes));
		}
		const Bytes coverage = c.take(bytes);
		if (!c.r.ok()) {
			return false;
		}
		rec.coverage.assign(coverage.begin(), coverage.end());
		if (!bounds_ok(rec.rect)) {
			return c.r.fail(ResError::malformed("mask record rect", c.at));
		}
		stored += rec.full ? 0u : 1u;
	}
	if (stored != tiles) {
		return c.r.fail(ResError::malformed("mask tile count", c.at));
	}
	r_tree.nodes[index].records = std::move(list);
	if (c.flag()) {
		const uint32_t size = c.u32();
		if (!c.r.ok()) {
			return false;
		}
		if (uint64_t(size) != uint64_t(tiles) * r_tree.tile_bytes()) {
			return c.r.fail(ResError::malformed("persistent mask size", c.at));
		}
		const Bytes data = c.take(size);
		if (!c.r.ok()) {
			return false;
		}
		r_tree.nodes[index].persistent = std::vector<uint8_t>(data.begin(), data.end());
	}
	const bool has_children = c.flag();
	if (!c.r.ok()) {
		return false;
	}
	if (has_children) {
		for (int i = 0; i < 4; ++i) {
			if (!read_mask_node(c, child_of(p_id, i), r_tree)) {
				return false;
			}
		}
	}
	return true;
}

ResError parse_mask(Bytes p_payload, TerrainMaskTree &r_tree, uint64_t &r_used) {
	detail::Reader reader(p_payload);
	Cursor c{ reader };
	TerrainMaskTree tree;
	const LayerHeader h = read_layer_header(c);
	const uint32_t node_count = c.u32();
	c.u32();
	c.u32();
	if (!reader.ok()) {
		return reader.error();
	}
	if (h.side <= kTerrainMaskBorder || h.side > kMaxMaskSide || !bounds_ok(h.bounds)) {
		return ResError::unsupported("mask tile side", h.side);
	}
	tree.samples_per_side = h.side;
	tree.bounds = h.bounds;
	if (!read_mask_node(c, TerrainNodeId{}, tree)) {
		return reader.error();
	}
	if (tree.nodes.size() != node_count) {
		return ResError::malformed("mask node count", c.at);
	}
	r_used = c.at;
	r_tree = std::move(tree);
	return {};
}

bool read_colour_node(Cursor &c, const TerrainNodeId &p_id, TerrainColourTree &r_tree, uint32_t &r_disabled) {
	if (p_id.level > kMaxLevel) {
		return c.r.fail(ResError::malformed("colour tree depth", c.at));
	}
	const bool disabled = c.flag();
	if (!c.r.ok()) {
		return false;
	}
	if (disabled) {
		++r_disabled;
		return true;
	}
	TerrainColourNode node;
	node.id = p_id;
	node.can_stream = c.flag();
	const bool persistent = c.flag();
	if (node.can_stream && persistent) {
		const Bytes data = c.take(r_tree.tile_bytes());
		if (!c.r.ok()) {
			return false;
		}
		node.persistent = std::vector<uint8_t>(data.begin(), data.end());
	}
	const bool has_children = c.flag();
	if (!c.r.ok()) {
		return false;
	}
	r_tree.nodes.push_back(std::move(node));
	if (has_children) {
		for (int i = 0; i < 4; ++i) {
			if (!read_colour_node(c, child_of(p_id, i), r_tree, r_disabled)) {
				return false;
			}
		}
	}
	return true;
}

ResError parse_colour(Bytes p_payload, TerrainColourTree &r_tree, uint64_t &r_used) {
	detail::Reader reader(p_payload);
	Cursor c{ reader };
	TerrainColourTree tree;
	const LayerHeader h = read_layer_header(c);
	c.u32();
	const uint32_t border = c.u32();
	const uint32_t node_count = c.u32();
	c.u32();
	if (!reader.ok()) {
		return reader.error();
	}
	if (h.side <= kTerrainColourBorder || h.side > kMaxColourSide || h.side % 4 != 0 || !bounds_ok(h.bounds)) {
		return ResError::unsupported("colour tile side", h.side);
	}
	if (border != kTerrainColourBorder) {
		return ResError::unsupported("colour tile border", border);
	}
	tree.texels_per_side = h.side;
	tree.bounds = h.bounds;
	uint32_t disabled_nodes = 0;
	if (!read_colour_node(c, TerrainNodeId{}, tree, disabled_nodes)) {
		return reader.error();
	}
	if (tree.nodes.size() + disabled_nodes != node_count) {
		return ResError::malformed("colour node count", c.at);
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
		node.children_chunk_size = c.u32();
		node.children_chunk = c.chunk_id();
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
		} else if (kind == 1) {
			TerrainMaskTree mask;
			uint64_t used = 0;
			const ResError e = parse_mask(payload, mask, used);
			if (!e.ok()) {
				return e;
			}
			if (used != len) {
				return ResError::malformed("mask tree size", offset + used);
			}
			tree.mask = std::move(mask);
		} else if (kind == 2) {
			TerrainColourTree colour;
			uint64_t used = 0;
			const ResError e = parse_colour(payload, colour, used);
			if (!e.ok()) {
				return e;
			}
			if (used != len) {
				return ResError::malformed("colour tree size", offset + used);
			}
			tree.colour = std::move(colour);
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

namespace {

template <class T> const T *find_node(const std::vector<T> &p_nodes, const TerrainNodeId &p_id) {
	for (const T &n : p_nodes) {
		if (n.id == p_id) {
			return &n;
		}
	}
	return nullptr;
}

std::array<float, 4> node_rect(const std::array<float, 4> &p_bounds, const TerrainNodeId &p_id) {
	const float n = float(uint32_t(1) << p_id.level);
	const float sx = (p_bounds[2] - p_bounds[0]) / n;
	const float sz = (p_bounds[3] - p_bounds[1]) / n;
	const float x0 = p_bounds[0] + sx * float(p_id.x);
	const float z0 = p_bounds[1] + sz * float(p_id.y);
	return { x0, z0, x0 + sx, z0 + sz };
}

struct PixelSpan {
	uint32_t x0 = 0, x1 = 0, z0 = 0, z1 = 0;
	bool empty() const { return x0 >= x1 || z0 >= z1; }
};

PixelSpan covered_pixels(const TerrainRegion &p_region, const std::array<float, 4> &p_rect) {
	const auto first = [&](float p_edge, float p_origin, uint32_t p_n) {
		const double f = std::ceil((double(p_edge) - double(p_origin)) / double(p_region.texel) - 0.5);
		return uint32_t(std::clamp(f, 0.0, double(p_n)));
	};
	PixelSpan s;
	s.x0 = first(p_rect[0], p_region.origin[0], p_region.width);
	s.x1 = first(p_rect[2], p_region.origin[0], p_region.width);
	s.z0 = first(p_rect[1], p_region.origin[1], p_region.height);
	s.z1 = first(p_rect[3], p_region.origin[1], p_region.height);
	return s;
}

bool region_ok(const TerrainRegion &p_region) {
	return p_region.width > 0 && p_region.height > 0 && p_region.width <= 8192 && p_region.height <= 8192 &&
			std::isfinite(p_region.texel) && p_region.texel > 0.0f && std::isfinite(p_region.origin[0]) &&
			std::isfinite(p_region.origin[1]);
}

template <int Channels, class Write>
void resample_tile(const TerrainRegion &p_region, const std::array<float, 4> &p_rect, const uint8_t *p_tile,
		uint32_t p_side, uint32_t p_border, const PixelSpan &p_span, Write &&p_write) {
	const double interior = double(p_side - p_border);
	const double shift = double(p_border) * 0.5 - 0.5;
	const double sx = interior / (double(p_rect[2]) - double(p_rect[0]));
	const double sz = interior / (double(p_rect[3]) - double(p_rect[1]));
	const int last = int(p_side) - 1;
	for (uint32_t z = p_span.z0; z < p_span.z1; ++z) {
		const double wz = double(p_region.origin[1]) + (double(z) + 0.5) * double(p_region.texel);
		const double v = std::clamp((wz - double(p_rect[1])) * sz + shift, 0.0, double(last));
		const int j0 = std::min(int(v), last - 1);
		const double fz = v - double(j0);
		for (uint32_t x = p_span.x0; x < p_span.x1; ++x) {
			const double wx = double(p_region.origin[0]) + (double(x) + 0.5) * double(p_region.texel);
			const double u = std::clamp((wx - double(p_rect[0])) * sx + shift, 0.0, double(last));
			const int i0 = std::min(int(u), last - 1);
			const double fx = u - double(i0);
			std::array<uint8_t, Channels> out{};
			for (int ch = 0; ch < Channels; ++ch) {
				const auto at = [&](int p_i, int p_j) {
					return double(p_tile[(size_t(p_j) * p_side + size_t(p_i)) * Channels + size_t(ch)]);
				};
				const double top = at(i0, j0) + (at(i0 + 1, j0) - at(i0, j0)) * fx;
				const double bottom = at(i0, j0 + 1) + (at(i0 + 1, j0 + 1) - at(i0, j0 + 1)) * fx;
				out[size_t(ch)] = uint8_t(std::clamp(top + (bottom - top) * fz + 0.5, 0.0, 255.0));
			}
			p_write(size_t(z) * p_region.width + x, out);
		}
	}
}

}

uint8_t TerrainMaskTree::layer_count() const {
	uint32_t count = 0;
	for (const TerrainMaskNode &n : nodes) {
		for (const TerrainMaskRecord &r : n.records) {
			count = std::max<uint32_t>(count, uint32_t(r.layer) + 1);
		}
	}
	return uint8_t(count);
}

uint8_t TerrainMaskTree::max_level() const {
	uint8_t level = 0;
	for (const TerrainMaskNode &n : nodes) {
		if (!n.records.empty()) {
			level = std::max(level, n.id.level);
		}
	}
	return level;
}

uint8_t TerrainColourTree::max_level() const {
	uint8_t level = 0;
	for (const TerrainColourNode &n : nodes) {
		if (n.can_stream) {
			level = std::max(level, n.id.level);
		}
	}
	return level;
}

ResError TerrainStreamingTree::chunk_layout(const TerrainNodeId &p_id, TerrainChunkLayout &r_layout) const {
	struct Parts {
		TerrainNodeId id;
		uint64_t heights = 0;
		uint32_t mask_tiles = 0;
		bool colour = false;
	};
	const uint64_t mask_bytes = mask ? mask->tile_bytes() : 0;
	const uint64_t colour_bytes = colour ? colour->tile_bytes() : 0;
	const auto parts_of = [&](const TerrainNodeId &p_node) {
		Parts parts;
		parts.id = p_node;
		if (heightfield) {
			const HeightfieldNode *h = find_node(heightfield->nodes, p_node);
			if (h != nullptr && !h->disabled && h->can_stream && !h->heights) {
				parts.heights = height_tile_bytes(*heightfield);
			}
		}
		if (mask) {
			const TerrainMaskNode *m = find_node(mask->nodes, p_node);
			if (m != nullptr && !m->persistent) {
				parts.mask_tiles = m->tiles;
			}
		}
		if (colour) {
			const TerrainColourNode *c = find_node(colour->nodes, p_node);
			parts.colour = c != nullptr && c->can_stream && !c->persistent;
		}
		return parts;
	};
	TerrainChunkLayout layout;
	std::vector<Parts> stored;
	uint64_t chunk_size = 0;
	const char *what = "terrain chunk layout";
	const TerrainStreamingNode *node = find_node(streaming_nodes, p_id);
	if (node != nullptr && node->lod0_chunk_size > 0) {
		layout.chunk = node->lod0_chunk;
		chunk_size = node->lod0_chunk_size;
		stored.push_back(parts_of(p_id));
	} else {
		if (p_id.level == 0) {
			return ResError::malformed("no streaming node", 0);
		}
		const TerrainNodeId parent{ uint8_t(p_id.level - 1), p_id.x / 2, p_id.y / 2 };
		const TerrainStreamingNode *holder = find_node(streaming_nodes, parent);
		if (holder == nullptr || holder->children_chunk_size == 0) {
			return ResError::malformed("no streaming node", p_id.level);
		}
		for (int i : kChildrenChunkOrder) {
			const TerrainNodeId child = child_of(parent, i);
			const TerrainStreamingNode *own_node = find_node(streaming_nodes, child);
			if (own_node == nullptr || own_node->lod0_chunk_size == 0) {
				stored.push_back(parts_of(child));
			}
		}
		layout.chunk = holder->children_chunk;
		chunk_size = holder->children_chunk_size;
		what = "terrain children chunk layout";
	}
	uint64_t heights_end = 0, masks_end = 0;
	for (const Parts &p : stored) {
		heights_end += p.heights;
		masks_end += uint64_t(p.mask_tiles) * mask_bytes;
	}
	masks_end += heights_end;
	uint64_t heights_at = 0, mask_at = heights_end, colour_at = masks_end;
	bool found = false;
	for (const Parts &p : stored) {
		if (p.id == p_id) {
			found = true;
			layout.heights_offset = heights_at;
			layout.heights_bytes = p.heights;
			layout.mask_offset = mask_at;
			layout.mask_tiles = p.mask_tiles;
			if (p.colour) {
				layout.colour_offset = colour_at;
			}
		}
		heights_at += p.heights;
		mask_at += uint64_t(p.mask_tiles) * mask_bytes;
		colour_at += p.colour ? colour_bytes : 0;
	}
	layout.total = colour_at;
	if (!found || layout.total != chunk_size) {
		return ResError::malformed(what, chunk_size);
	}
	r_layout = layout;
	return {};
}

ResError TerrainStreamingTree::mask_grids(const TerrainRegion &p_region, uint8_t p_depth,
		const TerrainChunkFetch &p_fetch, std::vector<std::vector<uint8_t>> &r_layers) const {
	if (!mask) {
		return ResError::malformed("streaming tree without a mask layer", 0);
	}
	if (!region_ok(p_region)) {
		return ResError::unsupported("mask region", p_region.width);
	}
	const TerrainMaskTree &tree = *mask;
	std::vector<std::vector<uint8_t>> layers(
			tree.layer_count(), std::vector<uint8_t>(size_t(p_region.width) * p_region.height, 0));
	const uint32_t side = tree.samples_per_side;
	struct Job {
		const TerrainMaskRecord *record = nullptr;
		const uint8_t *tile = nullptr;
		PixelSpan span;
	};
	for (uint8_t level = 0; level <= std::min<uint8_t>(p_depth, kMaxLevel); ++level) {
		std::vector<Job> jobs;
		std::vector<std::vector<uint8_t>> held;
		held.reserve(tree.nodes.size());
		for (const TerrainMaskNode &node : tree.nodes) {
			if (node.id.level != level || node.records.empty()) {
				continue;
			}
			bool touches = false;
			for (const TerrainMaskRecord &r : node.records) {
				touches = touches || !covered_pixels(p_region, r.rect).empty();
			}
			if (!touches) {
				continue;
			}
			const uint8_t *tiles = nullptr;
			if (node.tiles > 0) {
				if (node.persistent) {
					tiles = node.persistent->data();
				} else {
					TerrainChunkLayout layout;
					const ResError e = chunk_layout(node.id, layout);
					if (!e.ok()) {
						return e;
					}
					std::optional<std::vector<uint8_t>> chunk = p_fetch(layout.chunk);
					if (!chunk) {
						return ResError::malformed("mask chunk missing", node.id.level);
					}
					if (chunk->size() < layout.total) {
						return ResError::truncated(layout.mask_offset, layout.total, chunk->size());
					}
					held.push_back(std::move(*chunk));
					tiles = held.back().data() + layout.mask_offset;
				}
			}
			uint32_t stored = 0;
			for (const TerrainMaskRecord &r : node.records) {
				const uint8_t *tile = r.full ? nullptr : tiles + size_t(stored++) * tree.tile_bytes();
				const PixelSpan span = covered_pixels(p_region, r.rect);
				if (!span.empty()) {
					jobs.push_back(Job{ &r, tile, span });
				}
			}
		}
		parallel_for(jobs.size(), [&](size_t p_job) {
			const Job &job = jobs[p_job];
			std::vector<uint8_t> &out = layers[job.record->layer];
			if (job.tile == nullptr) {
				for (uint32_t z = job.span.z0; z < job.span.z1; ++z) {
					const size_t row = size_t(z) * p_region.width;
					std::fill(out.begin() + ptrdiff_t(row + job.span.x0), out.begin() + ptrdiff_t(row + job.span.x1),
							kTerrainFullMask);
				}
				return;
			}
			resample_tile<1>(p_region, job.record->rect, job.tile, side, kTerrainMaskBorder, job.span,
					[&](size_t p_at, const std::array<uint8_t, 1> &p_v) { out[p_at] = p_v[0]; });
		});
	}
	r_layers = std::move(layers);
	return {};
}

ResError TerrainStreamingTree::colour_grid(const TerrainRegion &p_region, uint8_t p_depth,
		const TerrainChunkFetch &p_fetch, std::vector<uint8_t> &r_rgba) const {
	if (!colour) {
		return ResError::malformed("streaming tree without a colour layer", 0);
	}
	if (!region_ok(p_region)) {
		return ResError::unsupported("colour region", p_region.width);
	}
	const TerrainColourTree &tree = *colour;
	const uint32_t side = tree.texels_per_side;
	std::vector<uint8_t> rgba(size_t(p_region.width) * p_region.height * 4, 0);
	struct Job {
		std::array<float, 4> rect{};
		std::vector<uint8_t> blocks;
		PixelSpan span;
	};
	for (uint8_t level = 0; level <= std::min<uint8_t>(p_depth, kMaxLevel); ++level) {
		std::vector<Job> jobs;
		for (const TerrainColourNode &node : tree.nodes) {
			if (node.id.level != level || !node.can_stream) {
				continue;
			}
			Job job;
			job.rect = node_rect(tree.bounds, node.id);
			job.span = covered_pixels(p_region, job.rect);
			if (job.span.empty()) {
				continue;
			}
			if (node.persistent) {
				job.blocks = *node.persistent;
			} else {
				TerrainChunkLayout layout;
				const ResError e = chunk_layout(node.id, layout);
				if (!e.ok()) {
					return e;
				}
				std::optional<std::vector<uint8_t>> chunk = p_fetch(layout.chunk);
				if (!chunk || !layout.colour_offset) {
					return ResError::malformed("colour chunk missing", node.id.level);
				}
				if (chunk->size() < layout.total) {
					return ResError::truncated(*layout.colour_offset, layout.total, chunk->size());
				}
				const auto begin = chunk->begin() + ptrdiff_t(*layout.colour_offset);
				job.blocks.assign(begin, begin + ptrdiff_t(tree.tile_bytes()));
			}
			jobs.push_back(std::move(job));
		}
		std::atomic<bool> undecoded = false;
		parallel_for(jobs.size(), [&](size_t p_job) {
			const Job &job = jobs[p_job];
			MipSlice slice;
			slice.width = slice.height = side;
			slice.row_pitch = uint32_t(texture_row_pitch(TextureFormat::Dxt1, side));
			slice.data = job.blocks;
			std::vector<std::array<float, 4>> decoded;
			if (!slice.texels(TextureFormat::Dxt1, decoded)) {
				undecoded = true;
				return;
			}
			std::vector<uint8_t> texels(decoded.size() * 4);
			for (size_t i = 0; i < decoded.size(); ++i) {
				for (size_t ch = 0; ch < 4; ++ch) {
					texels[4 * i + ch] = uint8_t(std::clamp(decoded[i][ch] * 255.0f + 0.5f, 0.0f, 255.0f));
				}
			}
			resample_tile<4>(p_region, job.rect, texels.data(), side, kTerrainColourBorder, job.span,
					[&](size_t p_at, const std::array<uint8_t, 4> &p_v) {
						std::memcpy(rgba.data() + 4 * p_at, p_v.data(), 4);
					});
		});
		if (undecoded) {
			return ResError::malformed("colour tile does not decode", level);
		}
	}
	r_rgba = std::move(rgba);
	return {};
}
}
