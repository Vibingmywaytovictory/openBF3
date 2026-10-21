#include "static_world_internal.h"

#include "jolt_runtime.h"

#include "bf3/core/cache/blob_io.h"
#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/ids.h"
#include "bf3/core/level/collision_soup.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/source_digest.h"
#include "bf3/core/res/terrain.h"
#include "bf3/core/thread_pool.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/core/util/finite.h"

#include <Jolt/AABBTree/NodeCodec/NodeCodecQuadTreeHalfFloat.h>
#include <Jolt/AABBTree/TriangleCodec/TriangleCodecIndexed8BitPackSOA4Flags.h>
#include <Jolt/Core/StreamIn.h>
#include <Jolt/Core/StreamOut.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SubShapeID.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

namespace bf3::sim {

namespace {

using Clock = std::chrono::steady_clock;

using bf3::core::seconds_since;

constexpr const char *kTileMagic = "OBF3JTIL";
constexpr const char *kWorldMagic = "OBF3JWLD";
constexpr const char *kTerrainMagic = "OBF3JTER";
constexpr size_t kWorldTerrainBytes = 4 + 32;
constexpr size_t kTagBytes = 8;
constexpr size_t kLayerHeaderBytes = 1 + 4 + 4 + 4;
constexpr size_t kWorldTileBytes = 4 + 4 + 32 + 3 * 4 + 4;

class VectorStreamOut final : public JPH::StreamOut {
public:
	explicit VectorStreamOut(std::vector<uint8_t> &r_bytes) :
			bytes_(r_bytes) {}
	void WriteBytes(const void *p_data, size_t p_size) override {
		const uint8_t *b = static_cast<const uint8_t *>(p_data);
		bytes_.insert(bytes_.end(), b, b + p_size);
	}
	bool IsFailed() const override { return false; }

private:
	std::vector<uint8_t> &bytes_;
};

class SpanStreamIn final : public JPH::StreamIn {
public:
	explicit SpanStreamIn(std::span<const uint8_t> p_bytes) :
			bytes_(p_bytes) {}
	void ReadBytes(void *r_data, size_t p_size) override {
		if (eof_ || p_size > bytes_.size() - at_) {
			std::memset(r_data, 0, p_size);
			eof_ = true;
			return;
		}
		std::memcpy(r_data, bytes_.data() + at_, p_size);
		at_ += p_size;
	}
	bool IsEOF() const override { return eof_; }
	bool IsFailed() const override { return false; }
	bool at_end() const { return !eof_ && at_ == bytes_.size(); }

private:
	std::span<const uint8_t> bytes_;
	size_t at_ = 0;
	bool eof_ = false;
};

using NodeCodec = JPH::NodeCodecQuadTreeHalfFloat;
using TriangleCodec = JPH::TriangleCodecIndexed8BitPackSOA4Flags;

template <class T>
T load(std::span<const uint8_t> p_bytes, size_t p_at) {
	T v;
	std::memcpy(&v, p_bytes.data() + p_at, sizeof(T));
	return v;
}

using bf3::core::finite;

constexpr uint32_t kMaxTreeDepth = 40;
constexpr uint32_t kPaddingCount = NodeCodec::TRIANGLE_COUNT_MASK;

constexpr uint32_t kMeshTriangleBits = 3;
constexpr uint32_t kMeshMaxTrianglesPerLeaf = 1u << kMeshTriangleBits;
constexpr uint8_t kMeshMaterialMask = (1u << 5) - 1;
constexpr uint32_t kSubShapeIdBits = JPH::SubShapeID::MaxBits;

core::Status validate_mesh_tree(std::span<const uint8_t> p_tree, uint32_t p_tags, uint32_t &r_triangles) {
	r_triangles = 0;
	const size_t size = p_tree.size();
	constexpr size_t kHeaders = size_t(NodeCodec::HeaderSize) + size_t(TriangleCodec::TriangleHeaderSize);
	if (size < kHeaders) {
		return core::make_error("mesh tree: " + std::to_string(size) + " bytes is smaller than its headers");
	}
	if (size > (size_t(NodeCodec::OFFSET_MASK) << NodeCodec::OFFSET_NON_SIGNIFICANT_BITS)) {
		return core::make_error("mesh tree: " + std::to_string(size) + " bytes is past what its offsets address");
	}
	const NodeCodec::Header header = load<NodeCodec::Header>(p_tree, 0);
	const TriangleCodec::TriangleHeader triangle_header = load<TriangleCodec::TriangleHeader>(p_tree, NodeCodec::HeaderSize);
	if (!finite(header.mRootBoundsMin) || !finite(header.mRootBoundsMax) || !finite(triangle_header.mOffset) ||
			!finite(triangle_header.mScale)) {
		return core::make_error("mesh tree: non-finite header bounds");
	}
	const uint32_t block_bits = header.mBlockIDBits;
	if (block_bits > uint32_t(NodeCodec::OFFSET_BITS) || block_bits + kMeshTriangleBits > kSubShapeIdBits) {
		return core::make_error("mesh tree: " + std::to_string(block_bits) + " block id bits");
	}
	std::vector<uint8_t> seen(size / 4 + 1, 0);
	struct Pending {
		uint32_t properties;
		uint32_t depth;
	};
	std::vector<Pending> stack{ { header.mRootProperties, 0 } };
	uint64_t triangles = 0;
	while (!stack.empty()) {
		const Pending p = stack.back();
		stack.pop_back();
		const uint32_t count = p.properties >> NodeCodec::TRIANGLE_COUNT_SHIFT;
		const uint32_t id = p.properties & NodeCodec::OFFSET_MASK;
		const size_t at = size_t(id) << NodeCodec::OFFSET_NON_SIGNIFICANT_BITS;
		if (count == kPaddingCount) {
			if (p.depth == 0) {
				return core::make_error("mesh tree: the root is a padding slot");
			}
			continue;
		}
		if (at < kHeaders || at >= size || seen[at / 4] != 0) {
			return core::make_error("mesh tree: " + std::string(count == 0 ? "node" : "triangle block") + " at " + std::to_string(at) +
					" is outside the buffer, inside the headers or reached twice");
		}
		seen[at / 4] = 1;
		if (count == 0) {
			if (p.depth >= kMaxTreeDepth) {
				return core::make_error("mesh tree: deeper than " + std::to_string(kMaxTreeDepth) + " levels");
			}
			if (size - at < sizeof(NodeCodec::Node)) {
				return core::make_error("mesh tree: node at " + std::to_string(at) + " runs past the buffer");
			}
			const NodeCodec::Node node = load<NodeCodec::Node>(p_tree, at);
			for (uint32_t child : node.mNodeProperties) {
				stack.push_back({ child, p.depth + 1 });
			}
			continue;
		}

		if (count > kMeshMaxTrianglesPerLeaf) {
			return core::make_error("mesh tree: a leaf of " + std::to_string(count) + " triangles");
		}
		if (block_bits < 32 && (uint64_t(id) >> block_bits) != 0) {
			return core::make_error("mesh tree: block id " + std::to_string(id) + " does not fit " + std::to_string(block_bits) + " bits");
		}
		const size_t blocks = (count + 3) / 4;
		const size_t first_block = at + sizeof(TriangleCodec::TriangleBlockHeader);
		if (size - at < sizeof(TriangleCodec::TriangleBlockHeader) + blocks * sizeof(TriangleCodec::TriangleBlock)) {
			return core::make_error("mesh tree: triangle block at " + std::to_string(at) + " runs past the buffer");
		}
		const uint32_t flags = load<uint32_t>(p_tree, at);
		const size_t vertices = at + (size_t(flags & TriangleCodec::OFFSET_TO_VERTICES_MASK) << TriangleCodec::OFFSET_NON_SIGNIFICANT_BITS);
		const size_t user_blocks = flags >> TriangleCodec::OFFSET_TO_VERTICES_BITS;
		if (user_blocks != blocks) {
			return core::make_error("mesh tree: triangle block at " + std::to_string(at) + " has no per-triangle user data");
		}
		const size_t user = first_block + user_blocks * sizeof(TriangleCodec::TriangleBlock);
		if (user > size || size - user < size_t(count) * 4) {
			return core::make_error("mesh tree: user data of the block at " + std::to_string(at) + " runs past the buffer");
		}
		for (size_t b = 0; b < blocks; ++b) {
			const TriangleCodec::TriangleBlock block = load<TriangleCodec::TriangleBlock>(p_tree, first_block + b * sizeof(TriangleCodec::TriangleBlock));
			for (size_t slot = 0; slot < 4; ++slot) {
				for (size_t v = 0; v < 3; ++v) {
					const size_t vertex = vertices + size_t(block.mIndices[v][slot]) * sizeof(TriangleCodec::VertexData);
					if (vertex >= size || size - vertex < sizeof(TriangleCodec::VertexData)) {
						return core::make_error("mesh tree: a vertex of the block at " + std::to_string(at) + " is outside the buffer");
					}
				}
				if (b * 4 + slot < count && (block.mFlags[slot] & kMeshMaterialMask) != 0) {
					return core::make_error("mesh tree: a triangle names a material");
				}
			}
		}
		for (uint32_t t = 0; t < count; ++t) {
			const uint32_t tag = load<uint32_t>(p_tree, user + size_t(t) * 4);
			if (tag >= p_tags) {
				return core::make_error("mesh tree: user data " + std::to_string(tag) + " is past the " + std::to_string(p_tags) + " tags");
			}
		}
		triangles += count;
	}
	if (triangles > UINT32_MAX) {
		return core::make_error("mesh tree: too many triangles");
	}
	r_triangles = uint32_t(triangles);
	return core::Status::success();
}

core::Result<JPH::RefConst<JPH::Shape>> restore_mesh(std::span<const uint8_t> p_stream, uint32_t p_tags, uint32_t &r_triangles) {
	constexpr size_t kHead = 1 + 8 + 4;
	if (p_stream.size() < kHead) {
		return core::make_error("mesh stream: truncated");
	}
	if (p_stream[0] != uint8_t(JPH::EShapeSubType::Mesh)) {
		return core::make_error("mesh stream: shape sub type " + std::to_string(int(p_stream[0])) + " is not a mesh");
	}
	if (load<uint64_t>(p_stream, 1) != 0) {
		return core::make_error("mesh stream: unexpected shape user data");
	}
	const uint32_t tree_bytes = load<uint32_t>(p_stream, 9);
	if (tree_bytes != p_stream.size() - kHead) {
		return core::make_error("mesh stream: tree of " + std::to_string(tree_bytes) + " bytes in a stream of " + std::to_string(p_stream.size()));
	}
	BF3_TRY(validate_mesh_tree(p_stream.subspan(kHead), p_tags, r_triangles));
	SpanStreamIn in(p_stream);
	const JPH::Shape::ShapeResult result = JPH::Shape::sRestoreFromBinaryState(in);
	if (result.HasError()) {
		return core::make_error("mesh stream: Jolt: " + std::string(result.GetError().c_str()));
	}
	if (!in.at_end()) {
		return core::make_error("mesh stream: Jolt did not read the whole stream");
	}
	return JPH::RefConst<JPH::Shape>(result.Get());
}

size_t counted(core::BlobReader &r_reader, size_t p_element_bytes) {
	const uint32_t n = r_reader.u32();
	if (r_reader.ok() && n > r_reader.remaining() / p_element_bytes) {
		r_reader.fail("count " + std::to_string(n) + " larger than the blob can hold");
		return 0;
	}
	return r_reader.ok() ? n : 0;
}

std::string float_bits_hex(float p_value) {
	uint32_t bits;
	std::memcpy(&bits, &p_value, sizeof(bits));
	char text[16];
	std::snprintf(text, sizeof(text), "%08x", bits);
	return text;
}

std::string build_params(const StaticWorldBuildOptions &p_options) {
	return "active_edge_degrees=" + float_bits_hex(p_options.active_edge_degrees) + ";raycast=" + (p_options.include_raycast ? "1" : "0");
}

constexpr size_t kRangeBlockBytes = 16;

uint32_t bit_width(uint32_t p_v) {
	uint32_t bits = 0;
	while (p_v != 0) {
		++bits;
		p_v >>= 1;
	}
	return bits;
}

core::Status validate_field_stream(std::span<const uint8_t> p_stream, uint32_t &r_samples) {
	constexpr size_t kHead = 1 + 8 + 12 + 12 + 4 + 4 + 1 + 2 + 2 + 4;
	if (p_stream.size() < kHead + 4 + 1) {
		return core::make_error("field stream: truncated");
	}
	if (p_stream[0] != uint8_t(JPH::EShapeSubType::HeightField)) {
		return core::make_error("field stream: shape sub type " + std::to_string(int(p_stream[0])) + " is not a height field");
	}
	if (load<uint64_t>(p_stream, 1) != 0) {
		return core::make_error("field stream: unexpected shape user data");
	}
	float offset[3], scale[3];
	for (int i = 0; i < 3; ++i) {
		offset[i] = load<float>(p_stream, 9 + 4 * size_t(i));
		scale[i] = load<float>(p_stream, 21 + 4 * size_t(i));
		if (!std::isfinite(offset[i]) || !std::isfinite(scale[i])) {
			return core::make_error("field stream: non-finite offset or scale");
		}
	}
	if (!(scale[0] > 0.0f) || !(scale[2] > 0.0f) || scale[1] == 0.0f) {
		return core::make_error("field stream: scale x / z must be positive and y non-zero");
	}
	const uint32_t samples = load<uint32_t>(p_stream, 33);
	const uint32_t block = load<uint32_t>(p_stream, 37);
	const uint8_t bits = p_stream[41];
	const uint16_t min_sample = load<uint16_t>(p_stream, 42);
	const uint16_t max_sample = load<uint16_t>(p_stream, 44);
	const uint32_t material_bytes = load<uint32_t>(p_stream, 46);
	if (block < 2 || block > 8) {
		return core::make_error("field stream: block size " + std::to_string(block));
	}
	const size_t max_samples = (core::res::kMaxGridSide + 8 - 1) / 8 * 8 + 8;
	if (samples < 2 * block || samples > max_samples || samples % block != 0) {
		return core::make_error("field stream: " + std::to_string(samples) + " samples a side (block " + std::to_string(block) + ")");
	}
	if (bits < 1 || bits > JPH::HeightFieldShapeConstants::cMaxBitsPerSample) {
		return core::make_error("field stream: " + std::to_string(int(bits)) + " bits per sample");
	}
	const uint32_t num_blocks = samples / block;
	if (num_blocks < 2 || num_blocks > (1u << JPH::HeightFieldShapeConstants::cNumBitsXY)) {
		return core::make_error("field stream: " + std::to_string(num_blocks) + " blocks a side");
	}
	if (2 * bit_width(samples - 1) + 1 > JPH::SubShapeID::MaxBits) {
		return core::make_error("field stream: too many samples for the sub-shape id bits");
	}
	const bool none = min_sample == JPH::HeightFieldShapeConstants::cNoCollisionValue16;
	if (!none && !(min_sample <= max_sample && max_sample <= JPH::HeightFieldShapeConstants::cMaxHeightValue16)) {
		return core::make_error("field stream: sample range " + std::to_string(min_sample) + ".." + std::to_string(max_sample));
	}
	if (material_bytes != 0) {
		return core::make_error("field stream: " + std::to_string(material_bytes) + " material index bytes (the cook sets no materials)");
	}
	const uint32_t material_bits = load<uint32_t>(p_stream, kHead);
	if (material_bits != 0) {
		return core::make_error("field stream: " + std::to_string(material_bits) + " bits per material index");
	}
	const uint8_t has_heights = p_stream[kHead + 4];
	if (has_heights != 1) {
		return core::make_error("field stream: no heights (a field without collision is never stored)");
	}
	const uint32_t max_level = bit_width(num_blocks - 1);
	const uint64_t grid_offset = ((uint64_t(1) << (2 * (max_level - 1))) - 1) / 3;
	const uint64_t max_stride = (uint64_t(num_blocks) + 1) >> 1;
	const uint64_t range_blocks = grid_offset + max_stride * max_stride;
	const uint64_t height_bytes = (uint64_t(samples) * samples * bits + 7) / 8 + 2;
	const uint64_t edge_bytes = ((uint64_t(samples) - 1) * (samples - 1) * 3 + 7) / 8 + 1;
	const uint64_t body = range_blocks * kRangeBlockBytes + height_bytes + edge_bytes;
	if (p_stream.size() - (kHead + 5) != body) {
		return core::make_error("field stream: " + std::to_string(p_stream.size() - (kHead + 5)) + " bytes after the header, the header says " +
				std::to_string(body));
	}
	r_samples = samples;
	return core::Status::success();
}

core::Result<JPH::RefConst<JPH::Shape>> restore_field(std::span<const uint8_t> p_stream, uint32_t &r_samples) {
	BF3_TRY(validate_field_stream(p_stream, r_samples));
	SpanStreamIn in(p_stream);
	const JPH::Shape::ShapeResult result = JPH::Shape::sRestoreFromBinaryState(in);
	if (result.HasError()) {
		return core::make_error("field stream: Jolt: " + std::string(result.GetError().c_str()));
	}
	if (!in.at_end()) {
		return core::make_error("field stream: Jolt did not read the whole stream");
	}
	return JPH::RefConst<JPH::Shape>(result.Get());
}

}

std::vector<uint8_t> encode_static_tile(const StaticWorldShapes::Impl::Tile &p_tile) {
	core::BlobWriter w;
	w.magic(kTileMagic, kStaticWorldFormatVersion);
	w.i32(p_tile.info.x);
	w.i32(p_tile.info.z);
	w.raw(p_tile.info.content.bytes.data(), p_tile.info.content.bytes.size());
	for (const Vec3 *v : { &p_tile.info.min, &p_tile.info.max }) {
		for (float c : *v) {
			w.f32(c);
		}
	}
	w.u32(uint32_t(p_tile.tags.size()));
	for (const SurfaceTag &t : p_tile.tags) {
		w.u32(t.material);
		w.u8(t.surface);
		w.u8(t.body);
		w.u8(t.flags);
		w.u8(t.reserved);
	}
	uint32_t layers = 0;
	for (const JPH::RefConst<JPH::Shape> &shape : p_tile.shapes) {
		layers += shape != nullptr ? 1 : 0;
	}
	w.u32(layers);
	for (size_t layer = 0; layer < kStaticLayerCount; ++layer) {
		const JPH::Shape *shape = p_tile.shapes[layer].GetPtr();
		if (shape == nullptr) {
			continue;
		}
		std::vector<uint8_t> stream;
		VectorStreamOut out(stream);
		shape->SaveBinaryState(out);
		w.u8(uint8_t(layer));
		w.u32(p_tile.info.triangles[layer]);
		w.u32(shape->GetStats().mNumTriangles);
		w.u32(uint32_t(stream.size()));
		w.raw(stream.data(), stream.size());
	}
	return w.take();
}

core::Result<StaticWorldShapes::Impl::Tile> decode_static_tile(std::span<const uint8_t> p_blob) {
	core::BlobReader r(p_blob);
	const uint32_t version = r.magic(kTileMagic);
	if (!r.ok()) {
		return r.error("jolt-tile");
	}
	if (version != kStaticWorldFormatVersion) {
		return core::make_error("jolt-tile: format version " + std::to_string(version) + ", expected " + std::to_string(kStaticWorldFormatVersion));
	}
	StaticWorldShapes::Impl::Tile tile;
	StaticTileInfo &info = tile.info;
	info.x = r.i32();
	info.z = r.i32();
	for (uint8_t &b : info.content.bytes) {
		b = r.u8();
	}
	for (Vec3 *v : { &info.min, &info.max }) {
		for (float &c : *v) {
			c = r.f32();
		}
	}
	const size_t tags = counted(r, kTagBytes);
	if (!r.ok()) {
		return r.error("jolt-tile");
	}
	tile.tags.resize(tags);
	for (SurfaceTag &t : tile.tags) {
		t.material = r.u32();
		t.surface = r.u8();
		t.body = r.u8();
		t.flags = r.u8();
		t.reserved = r.u8();
	}
	info.tags = uint32_t(tags);
	const uint32_t layers = r.u32();
	if (!r.ok()) {
		return r.error("jolt-tile");
	}
	if (!finite(info.min) || !finite(info.max)) {
		return core::make_error("jolt-tile: non-finite bounds");
	}
	if (layers > kStaticLayerCount) {
		return core::make_error("jolt-tile: " + std::to_string(layers) + " layers");
	}
	if (tags == 0 && layers != 0) {
		return core::make_error("jolt-tile: shapes without a tag table");
	}
	int previous = -1;
	for (uint32_t i = 0; i < layers; ++i) {
		const uint8_t layer = r.u8();
		const uint32_t input = r.u32();
		const uint32_t triangles = r.u32();
		const uint32_t bytes = r.u32();
		if (!r.ok()) {
			return r.error("jolt-tile");
		}
		if (layer >= kStaticLayerCount || int(layer) <= previous) {
			return core::make_error("jolt-tile: layer " + std::to_string(int(layer)) + " out of order");
		}
		previous = layer;
		if (triangles == 0 || triangles > input) {
			return core::make_error("jolt-tile: layer " + std::to_string(int(layer)) + " has " + std::to_string(triangles) + " of " +
					std::to_string(input) + " triangles");
		}
		std::vector<uint8_t> stream;
		if (!r.array(bytes, stream)) {
			return r.error("jolt-tile");
		}
		uint32_t found = 0;
		core::Result<JPH::RefConst<JPH::Shape>> shape = restore_mesh(stream, uint32_t(tags), found);
		if (!shape) {
			return core::error_at("jolt-tile (" + std::to_string(info.x) + ", " + std::to_string(info.z) + ") layer " + std::to_string(int(layer)),
					shape.error());
		}
		if (found != triangles) {
			return core::make_error("jolt-tile: layer " + std::to_string(int(layer)) + " holds " + std::to_string(found) + " triangles, the blob says " +
					std::to_string(triangles));
		}
		tile.shapes[layer] = std::move(*shape);
		info.triangles[layer] = input;
	}
	if (!r.ok()) {
		return r.error("jolt-tile");
	}
	if (!r.at_end()) {
		return core::make_error("jolt-tile: " + std::to_string(r.remaining()) + " trailing bytes");
	}
	return tile;
}

std::vector<uint8_t> encode_static_tile_blob(const StaticWorldShapes &p_world, size_t p_tile) {
	const StaticWorldShapes::Impl &w = p_world.impl();
	if (p_tile >= w.tiles.size()) {
		return {};
	}
	return encode_static_tile(w.tiles[p_tile]);
}

core::Result<std::shared_ptr<const StaticWorldShapes>> decode_static_tile_blobs(std::span<const std::span<const uint8_t>> p_tiles) {
	const Clock::time_point start = Clock::now();
	auto impl = std::make_unique<StaticWorldShapes::Impl>();
	impl->runtime = jolt_runtime_acquire();
	impl->tiles.reserve(p_tiles.size());
	for (size_t i = 0; i < p_tiles.size(); ++i) {
		core::Result<StaticWorldShapes::Impl::Tile> tile = decode_static_tile(p_tiles[i]);
		if (!tile) {
			return core::error_at("tile blob " + std::to_string(i), tile.error());
		}
		if (!impl->tiles.empty()) {
			const StaticTileInfo &prev = impl->tiles.back().info;
			if (std::pair(prev.x, prev.z) >= std::pair(tile->info.x, tile->info.z)) {
				return core::make_error("tile blob " + std::to_string(i) + ": tiles are not in increasing (x, z) order");
			}
		}
		impl->stats.cache_bytes += p_tiles[i].size();
		impl->tiles.push_back(std::move(*tile));
	}
	impl->stats.tiles_hit = impl->tiles.size();
	impl->stats.seconds_restore = seconds_since(start);
	return StaticWorldBuilder::finish(std::move(impl));
}

std::vector<uint8_t> encode_static_world_index(const StaticWorldIndex &p_index) {
	core::BlobWriter w;
	w.magic(kWorldMagic, kStaticWorldIndexVersion);
	w.u32(uint32_t(p_index.tiles.size()));
	for (const StaticWorldEntry &t : p_index.tiles) {
		w.i32(t.x);
		w.i32(t.z);
		w.raw(t.content.bytes.data(), t.content.bytes.size());
		for (uint32_t n : t.triangles) {
			w.u32(n);
		}
		w.u32(t.tags);
	}
	w.u64(p_index.soup_sum);
	w.u64(p_index.soup_triangles);
	w.u32(uint32_t(p_index.terrains.size()));
	for (const StaticWorldIndex::Terrain &t : p_index.terrains) {
		w.u32(t.object);
		w.raw(t.content.bytes.data(), t.content.bytes.size());
	}
	return w.take();
}

core::Result<StaticWorldIndex> decode_static_world_index(std::span<const uint8_t> p_blob) {
	core::BlobReader r(p_blob);
	const uint32_t version = r.magic(kWorldMagic);
	if (r.ok() && version != kStaticWorldIndexVersion) {
		return core::make_error("jolt-world: format version " + std::to_string(version));
	}
	StaticWorldIndex out;
	const size_t tiles = counted(r, kWorldTileBytes);
	if (!r.ok()) {
		return r.error("jolt-world");
	}
	out.tiles.resize(tiles);
	for (StaticWorldEntry &t : out.tiles) {
		t.x = r.i32();
		t.z = r.i32();
		for (uint8_t &b : t.content.bytes) {
			b = r.u8();
		}
		for (uint32_t &n : t.triangles) {
			n = r.u32();
		}
		t.tags = r.u32();
	}
	out.soup_sum = r.u64();
	out.soup_triangles = r.u64();
	const size_t terrains = counted(r, kWorldTerrainBytes);
	if (!r.ok()) {
		return r.error("jolt-world");
	}
	if (terrains > kMaxStaticTerrains) {
		return core::make_error("jolt-world: " + std::to_string(terrains) + " terrains");
	}
	out.terrains.resize(terrains);
	for (StaticWorldIndex::Terrain &t : out.terrains) {
		t.object = r.u32();
		for (uint8_t &b : t.content.bytes) {
			b = r.u8();
		}
	}
	if (!r.ok()) {
		return r.error("jolt-world");
	}
	if (!r.at_end()) {
		return core::make_error("jolt-world: " + std::to_string(r.remaining()) + " trailing bytes");
	}
	for (size_t i = 1; i < out.tiles.size(); ++i) {
		if (std::pair(out.tiles[i - 1].x, out.tiles[i - 1].z) >= std::pair(out.tiles[i].x, out.tiles[i].z)) {
			return core::make_error("jolt-world: tiles are not in increasing (x, z) order");
		}
	}
	return out;
}

std::vector<uint8_t> encode_static_terrain(const StaticWorldShapes::Impl::Terrain &p_terrain) {
	const StaticTerrainInfo &info = p_terrain.info;
	core::BlobWriter w;
	w.magic(kTerrainMagic, kStaticTerrainFormatVersion);
	for (uint32_t v : { info.object, info.width, info.height, info.holes, info.holed, info.patch_triangles }) {
		w.u32(v);
	}
	w.f32(info.max_error);
	for (const Vec3 *v : { &info.min, &info.max }) {
		for (float c : *v) {
			w.f32(c);
		}
	}
	w.raw(info.content.bytes.data(), info.content.bytes.size());
	for (float c : p_terrain.position) {
		w.f32(c);
	}
	for (float c : p_terrain.rotation) {
		w.f32(c);
	}
	const auto put_stream = [&](const JPH::Shape *p_shape) {
		if (p_shape == nullptr) {
			w.u32(0);
			return;
		}
		std::vector<uint8_t> stream;
		VectorStreamOut out(stream);
		p_shape->SaveBinaryState(out);
		w.u32(uint32_t(stream.size()));
		w.raw(stream.data(), stream.size());
	};
	put_stream(p_terrain.field.GetPtr());
	w.u32(p_terrain.patch != nullptr ? p_terrain.patch->GetStats().mNumTriangles : 0);
	put_stream(p_terrain.patch.GetPtr());
	return w.take();
}

core::Result<StaticWorldShapes::Impl::Terrain> decode_static_terrain(std::span<const uint8_t> p_blob) {
	core::BlobReader r(p_blob);
	const uint32_t version = r.magic(kTerrainMagic);
	if (!r.ok()) {
		return r.error("jolt-terrain");
	}
	if (version != kStaticTerrainFormatVersion) {
		return core::make_error("jolt-terrain: format version " + std::to_string(version) + ", expected " + std::to_string(kStaticTerrainFormatVersion));
	}
	StaticWorldShapes::Impl::Terrain terrain;
	StaticTerrainInfo &info = terrain.info;
	for (uint32_t *v : { &info.object, &info.width, &info.height, &info.holes, &info.holed, &info.patch_triangles }) {
		*v = r.u32();
	}
	info.max_error = r.f32();
	for (Vec3 *v : { &info.min, &info.max }) {
		for (float &c : *v) {
			c = r.f32();
		}
	}
	for (uint8_t &b : info.content.bytes) {
		b = r.u8();
	}
	for (float &c : terrain.position) {
		c = r.f32();
	}
	for (float &c : terrain.rotation) {
		c = r.f32();
	}
	if (!r.ok()) {
		return r.error("jolt-terrain");
	}
	if (!finite(info.min) || !finite(info.max) || !finite(terrain.position) || !finite(terrain.rotation) ||
			!std::isfinite(info.max_error) || info.max_error < 0.0f) {
		return core::make_error("jolt-terrain: non-finite info or transform");
	}
	const double q = double(terrain.rotation[0]) * terrain.rotation[0] + double(terrain.rotation[1]) * terrain.rotation[1] +
			double(terrain.rotation[2]) * terrain.rotation[2] + double(terrain.rotation[3]) * terrain.rotation[3];
	if (std::fabs(q - 1.0) > 1e-4) {
		return core::make_error("jolt-terrain: the rotation is not a unit quaternion");
	}
	if (info.width < 2 || info.height < 2 || info.width > core::res::kMaxGridSide || info.height > core::res::kMaxGridSide) {
		return core::make_error("jolt-terrain: grid " + std::to_string(info.width) + " x " + std::to_string(info.height));
	}
	const uint64_t cells = uint64_t(info.width - 1) * (info.height - 1);
	if (info.holes > uint64_t(info.width) * info.height || info.holed > cells || info.patch_triangles > 8ull * info.holed) {
		return core::make_error("jolt-terrain: counts past the grid");
	}

	const uint32_t field_bytes = r.u32();
	std::vector<uint8_t> stream;
	if (!r.ok() || !r.array(field_bytes, stream)) {
		return r.error("jolt-terrain");
	}
	if (field_bytes != 0) {
		uint32_t samples = 0;
		core::Result<JPH::RefConst<JPH::Shape>> field = restore_field(stream, samples);
		if (!field) {
			return core::error_at("jolt-terrain field", field.error());
		}
		const uint32_t block = kStaticTerrainBlock;
		uint32_t expected = std::max({ info.width, info.height, 2 * block });
		expected = (expected + block - 1) / block * block;
		if (samples != expected) {
			return core::make_error("jolt-terrain: field of " + std::to_string(samples) + " samples a side for a " + std::to_string(info.width) + " x " +
					std::to_string(info.height) + " grid");
		}
		terrain.field = std::move(*field);
	}

	const uint32_t patch_triangles = r.u32();
	const uint32_t patch_bytes = r.u32();
	stream.clear();
	if (!r.ok() || !r.array(patch_bytes, stream)) {
		return r.error("jolt-terrain");
	}
	if (patch_bytes != 0) {
		if (patch_triangles == 0 || patch_triangles > info.patch_triangles) {
			return core::make_error("jolt-terrain: patch of " + std::to_string(patch_triangles) + " of " + std::to_string(info.patch_triangles) + " triangles");
		}
		uint32_t found = 0;
		core::Result<JPH::RefConst<JPH::Shape>> patch = restore_mesh(stream, 1, found);
		if (!patch) {
			return core::error_at("jolt-terrain patch", patch.error());
		}
		if (found != patch_triangles) {
			return core::make_error("jolt-terrain: the patch holds " + std::to_string(found) + " triangles, the blob says " + std::to_string(patch_triangles));
		}
		terrain.patch = std::move(*patch);
	} else if (patch_triangles != 0) {
		return core::make_error("jolt-terrain: patch triangles without a patch");
	}
	if (!r.at_end()) {
		return core::make_error("jolt-terrain: " + std::to_string(r.remaining()) + " trailing bytes");
	}
	return terrain;
}

std::vector<uint8_t> encode_static_terrain_blob(const StaticWorldShapes &p_world, size_t p_terrain) {
	const StaticWorldShapes::Impl &w = p_world.impl();
	if (p_terrain >= w.terrains.size()) {
		return {};
	}
	return encode_static_terrain(w.terrains[p_terrain]);
}

core::Result<std::shared_ptr<const StaticWorldShapes>> decode_static_terrain_blob(std::span<const uint8_t> p_blob) {
	const Clock::time_point start = Clock::now();
	auto impl = std::make_unique<StaticWorldShapes::Impl>();
	impl->runtime = jolt_runtime_acquire();
	core::Result<StaticWorldShapes::Impl::Terrain> terrain = decode_static_terrain(p_blob);
	if (!terrain) {
		return terrain.error();
	}
	impl->terrains.push_back(std::move(*terrain));
	impl->stats.cache_bytes = p_blob.size();
	impl->stats.terrains_hit = 1;
	impl->stats.seconds_restore = seconds_since(start);
	return StaticWorldBuilder::finish(std::move(impl));
}

core::CacheKeyInputs static_terrain_key_inputs(const core::Sha256Digest &p_content, const StaticWorldBuildOptions &p_options) {
	core::CacheKeyInputs in;
	in.kind = kStaticTerrainCacheKind;
	in.converter_version = kStaticWorldConverterVersion;
	in.engine = std::string(core::cache_engine_id());
	in.format_version = kStaticTerrainFormatVersion;
	in.jolt_version = jolt_build_id();

	in.params = "content=" + p_content.hex() + ";bits=" + std::to_string(kStaticTerrainBits) + ";block=" + std::to_string(kStaticTerrainBlock) +
			";active_edge_degrees=" + float_bits_hex(p_options.active_edge_degrees);
	return in;
}

core::CacheKeyInputs static_tile_key_inputs(const core::Sha256Digest &p_content, const StaticWorldBuildOptions &p_options) {
	core::CacheKeyInputs in;
	in.kind = kStaticTileCacheKind;
	in.converter_version = kStaticWorldConverterVersion;
	in.engine = std::string(core::cache_engine_id());
	in.format_version = kStaticWorldFormatVersion;
	in.jolt_version = jolt_build_id();
	in.params = "content=" + p_content.hex() + ";" + build_params(p_options);
	return in;
}

core::CacheKeyInputs static_world_key_inputs(const core::Sha256Digest &p_source_digest, std::string_view p_level, std::string_view p_mode,
		const StaticWorldLoadOptions &p_options) {
	core::CacheKeyInputs in;
	in.kind = kStaticWorldCacheKind;
	in.converter_version = kStaticWorldConverterVersion;
	in.engine = std::string(core::cache_engine_id());
	in.format_version = kStaticWorldIndexVersion;
	in.jolt_version = jolt_build_id();

	in.params = "source=" + p_source_digest.hex() + ";level=" + core::to_lower_ascii(p_level) + ";mode=" + core::to_lower_ascii(p_mode) +
			";soup=" + std::to_string(core::level::kCollisionSoupVersion) + ";terrain=" + std::to_string(kStaticTerrainBits) + "/" +
			std::to_string(kStaticTerrainBlock) + ";terrain_spacing=" + float_bits_hex(p_options.terrain_spacing) + ";" + build_params(p_options.build);
	return in;
}

namespace {

std::optional<std::unique_ptr<StaticWorldShapes::Impl>> restore_world(core::Cache &r_cache, const StaticWorldIndex &p_index,
		const StaticWorldBuildOptions &p_options, std::shared_ptr<void> p_runtime) {
	const Clock::time_point start = Clock::now();
	std::vector<std::optional<StaticWorldShapes::Impl::Tile>> tiles(p_index.tiles.size());
	std::vector<std::optional<StaticWorldShapes::Impl::Terrain>> terrains(p_index.terrains.size());
	const size_t nt = p_index.terrains.size();
	std::vector<uint64_t> bytes(nt + p_index.tiles.size(), 0);
	std::atomic<bool> failed{ false };
	core::parallel_for(
			nt + p_index.tiles.size(),
			[&](size_t p_job) {
				if (failed.load(std::memory_order_relaxed)) {
					return;
				}
				if (p_job < nt) {
					const StaticWorldIndex::Terrain &e = p_index.terrains[p_job];
					const std::optional<core::CacheKey> key = core::make_cache_key(static_terrain_key_inputs(e.content, p_options));
					if (!key) {
						failed = true;
						return;
					}
					core::GetResult got = r_cache.get(*key);
					if (got.status != core::CacheStatus::Ok) {
						failed = true;
						return;
					}
					core::Result<StaticWorldShapes::Impl::Terrain> terrain = decode_static_terrain(got.entry.payload());
					if (!terrain.ok() || terrain->info.content != e.content) {
						got.entry.reset();
						if (!terrain.ok()) {
							r_cache.remove(*key);
						}
						failed = true;
						return;
					}
					terrain->info.object = e.object;
					bytes[p_job] = got.entry.payload().size();
					terrains[p_job] = std::move(*terrain);
					return;
				}
				const size_t p_at = p_job - nt;
				const StaticWorldEntry &e = p_index.tiles[p_at];
				const std::optional<core::CacheKey> key = core::make_cache_key(static_tile_key_inputs(e.content, p_options));
				if (!key) {
					failed = true;
					return;
				}
				core::GetResult got = r_cache.get(*key);
				if (got.status != core::CacheStatus::Ok) {
					failed = true;
					return;
				}
				core::Result<StaticWorldShapes::Impl::Tile> tile = decode_static_tile(got.entry.payload());
				bool same = tile.ok() && tile->info.x == e.x && tile->info.z == e.z && tile->info.content == e.content && tile->info.triangles == e.triangles &&
						tile->info.tags == e.tags;
				for (size_t layer = 0; same && layer < kStaticLayerCount; ++layer) {
					const bool wanted = layer != size_t(StaticLayer::Raycast) || p_options.include_raycast;
					same = wanted || tile->shapes[layer] == nullptr;
				}
				if (!same) {
					got.entry.reset();
					if (!tile.ok()) {
						r_cache.remove(*key);
					}
					failed = true;
					return;
				}
				bytes[p_job] = got.entry.payload().size();
				tiles[p_at] = std::move(*tile);
				if (p_options.tiles_done != nullptr) {
					p_options.tiles_done->fetch_add(1, std::memory_order_relaxed);
				}
			},
			p_options.threads);
	if (failed) {
		return std::nullopt;
	}
	auto impl = std::make_unique<StaticWorldShapes::Impl>();
	impl->runtime = std::move(p_runtime);
	impl->tiles.reserve(tiles.size());
	for (size_t i = 0; i < tiles.size(); ++i) {
		impl->tiles.push_back(std::move(*tiles[i]));
	}
	for (size_t i = 0; i < terrains.size(); ++i) {
		impl->terrains.push_back(std::move(*terrains[i]));
	}
	for (uint64_t b : bytes) {
		impl->stats.cache_bytes += b;
	}
	impl->stats.tiles_hit = impl->tiles.size();
	impl->stats.terrains_hit = impl->terrains.size();
	impl->stats.world_hit = true;
	impl->stats.seconds_restore = seconds_since(start);
	return impl;
}

}

core::Result<std::shared_ptr<const StaticWorldShapes>> load_static_world_cached(core::Cache *p_cache, const core::level::LoadedLevel &p_level,
		const core::level::World &p_world, const StaticWorldLoadOptions &p_options) {
	const Clock::time_point start = Clock::now();
	const StaticWorldBuildOptions &build = p_options.build;
	if (!std::isfinite(build.active_edge_degrees) || build.active_edge_degrees < 0.0f || build.active_edge_degrees > 180.0f) {
		return core::make_error("static world: active_edge_degrees " + std::to_string(build.active_edge_degrees) + " is outside 0..180");
	}
	if (!std::isfinite(p_options.terrain_spacing) || p_options.terrain_spacing <= 0.0f || p_options.terrain_spacing > 1024.0f) {
		return core::make_error("static world: terrain_spacing " + std::to_string(p_options.terrain_spacing) + " is outside (0, 1024]");
	}
	std::shared_ptr<void> runtime = jolt_runtime_acquire();
	core::Cache *cache = (p_cache != nullptr && p_cache->is_open()) ? p_cache : nullptr;

	std::optional<core::CacheKey> world_key;
	if (cache != nullptr) {
		const core::Result<core::level::SourceDigest> digest = core::level::level_source_digest(p_level);
		if (digest && digest->digest) {
			world_key = core::make_cache_key(static_world_key_inputs(*digest->digest, p_level.info.id, p_level.mode, p_options));
		}
	}
	if (world_key) {
		core::GetResult got = cache->get(*world_key);
		if (got.status == core::CacheStatus::Ok) {
			core::Result<StaticWorldIndex> index = decode_static_world_index(got.entry.payload());
			const uint64_t index_bytes = got.entry.payload().size();
			got.entry.reset();
			if (!index) {
				cache->remove(*world_key);
			} else if (std::optional<std::unique_ptr<StaticWorldShapes::Impl>> restored = restore_world(*cache, *index, build, runtime)) {
				std::unique_ptr<StaticWorldShapes::Impl> &impl = *restored;
				impl->stats.cache_bytes += index_bytes;
				impl->stats.seconds_total = seconds_since(start);
				return StaticWorldBuilder::finish(std::move(impl));
			}
			if (build.tiles_done != nullptr) {
				build.tiles_done->store(0, std::memory_order_relaxed);
			}
		}
	}

	Clock::time_point clock = Clock::now();
	core::level::SoupOptions soup_options;
	soup_options.threads = p_options.soup_threads;
	soup_options.terrain_spacing = p_options.terrain_spacing;
	soup_options.progress = p_options.soup_progress;
	core::Result<core::level::CollisionSoup> soup = core::level::build_collision_soup(p_level, p_world, soup_options);
	if (!soup) {
		return soup.error();
	}
	const double seconds_soup = seconds_since(clock);
	std::vector<std::array<int32_t, 2>> keys;
	keys.reserve(soup->tiles.size());
	for (const core::level::SoupTile &tile : soup->tiles) {
		keys.push_back({ tile.x, tile.z });
	}
	core::Result<std::vector<StaticTerrainView>> views = soup_terrain_views(*soup);
	if (!views) {
		return views.error();
	}
	bool stored = false;
	core::Result<std::unique_ptr<StaticWorldShapes::Impl>> impl = cook_static_tiles_and_terrains(
			cache, keys,
			[&](size_t p_tile, std::vector<StaticTriangle> &r_scratch) -> core::Result<std::span<const StaticTriangle>> {
				BF3_TRY(soup_tile_triangles(soup->tiles[p_tile], r_scratch));
				return std::span<const StaticTriangle>(r_scratch);
			},
			*views, build, std::move(runtime), &stored);
	if (!impl) {
		return impl.error();
	}
	StaticWorldShapes::Impl &w = **impl;
	w.stats.seconds_soup = seconds_soup;
	if (world_key && stored) {
		StaticWorldIndex index;
		index.soup_sum = soup->sum;
		index.soup_triangles = soup->triangles;
		for (const StaticWorldShapes::Impl::Tile &tile : w.tiles) {
			StaticWorldEntry e;
			e.x = tile.info.x;
			e.z = tile.info.z;
			e.content = tile.info.content;
			e.triangles = tile.info.triangles;
			e.tags = tile.info.tags;
			index.tiles.push_back(e);
		}
		for (const StaticWorldShapes::Impl::Terrain &terrain : w.terrains) {
			index.terrains.push_back({ terrain.info.object, terrain.info.content });
		}
		const std::vector<uint8_t> blob = encode_static_world_index(index);
		if (cache->put(*world_key, blob).status == core::CacheStatus::Ok) {
			w.stats.cache_bytes += blob.size();
		}
	}
	*soup = core::level::CollisionSoup();
	w.stats.seconds_total = seconds_since(start);
	return StaticWorldBuilder::finish(std::move(*impl));
}

core::Sha256Digest water_planes_digest(std::span<const core::level::WaterPlane> p_water) {
	std::vector<uint8_t> bytes;
	const auto put = [&](uint32_t p_v) {
		for (int i = 0; i < 4; ++i) {
			bytes.push_back(uint8_t(p_v >> (i * 8)));
		}
	};
	const auto put_f = [&](float p_f) {
		uint32_t v = 0;
		std::memcpy(&v, &p_f, 4);
		put(v);
	};
	for (const core::level::WaterPlane &w : p_water) {
		put_f(w.y);
		put(uint32_t(w.polygon.size()));
		for (const std::array<float, 2> &p : w.polygon) {
			put_f(p[0]);
			put_f(p[1]);
		}
	}
	core::Sha256Digest out{};
	core::Hasher hasher(core::HashAlgorithm::Sha256);
	hasher.update(bytes);
	hasher.finish(out.bytes);
	return out;
}

core::Sha256Digest tile_list_digest(std::span<const std::array<int32_t, 2>> p_tiles) {
	std::vector<std::array<int32_t, 2>> tiles(p_tiles.begin(), p_tiles.end());
	std::sort(tiles.begin(), tiles.end());
	tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());
	std::vector<uint8_t> bytes;
	bytes.reserve(tiles.size() * 8);
	for (const std::array<int32_t, 2> &t : tiles) {
		for (int32_t v : t) {
			for (int i = 0; i < 4; ++i) {
				bytes.push_back(uint8_t(uint32_t(v) >> (i * 8)));
			}
		}
	}
	core::Sha256Digest out{};
	core::Hasher hasher(core::HashAlgorithm::Sha256);
	hasher.update(bytes);
	hasher.finish(out.bytes);
	return out;
}

}
