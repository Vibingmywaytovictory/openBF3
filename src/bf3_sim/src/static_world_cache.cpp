// The static world through the derived-data cache (docs/design/m1.md §5.5): the "jolt-tile" and "jolt-world" blobs,
// their keys, and load_static_world_cached (warm: source digest -> world index -> tile restores; cold: soup ->
// content-keyed tile cook -> world index).
//
// "OBF3JTIL" v1 (one tile), little endian:
//   magic, version
//   i32 x, i32 z; content digest (32 bytes); bounds min, max (6 x f32)
//   u32 tag count; per tag: u32 material, u8 surface, body, flags, reserved
//   u32 layer count; per present layer (strictly increasing ids):
//     u8 layer, u32 input triangles, u32 Jolt triangles, u32 stream bytes, the MeshShape's
//     Shape::SaveBinaryState stream (u8 sub type, u64 user data, u32 tree bytes, the tree)
// The stream is written by SaveBinaryState rather than SaveWithChildren: a MeshShape has no children and the
// static world gives it no materials, so the two differ only by Jolt's shape-map and material-list framing.
// Restoring goes through Shape::sRestoreFromBinaryState only after validate_mesh_tree has walked the whole tree
// (see there): Jolt itself trusts every offset in it, so a damaged or hostile tree would otherwise reach a query.
//
// "OBF3JWLD" v1 (one level x mode): u32 tile count; per tile: i32 x, i32 z, content digest, 3 x u32 triangle
// counts, u32 tag count; then u64 soup sum, u64 soup triangles.
#include "static_world_internal.h"

#include "jolt_runtime.h"

#include "bf3/core/cache/blob_io.h"
#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/ids.h"
#include "bf3/core/level/collision_soup.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/source_digest.h"
#include "bf3/core/thread_pool.h"

#include <Jolt/AABBTree/NodeCodec/NodeCodecQuadTreeHalfFloat.h>
#include <Jolt/AABBTree/TriangleCodec/TriangleCodecIndexed8BitPackSOA4Flags.h>
#include <Jolt/Core/StreamIn.h>
#include <Jolt/Core/StreamOut.h>
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

double seconds_since(Clock::time_point p_start) {
	return std::chrono::duration<double>(Clock::now() - p_start).count();
}

constexpr const char *kTileMagic = "OBF3JTIL";
constexpr const char *kWorldMagic = "OBF3JWLD";
constexpr size_t kTagBytes = 8;
constexpr size_t kLayerHeaderBytes = 1 + 4 + 4 + 4;
constexpr size_t kWorldTileBytes = 4 + 4 + 32 + 3 * 4 + 4;

// ---- Jolt streams over our own bytes ------------------------------------------------------------------------

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

// Never reads past its span; a short read zero-fills and sets EOF (Jolt then reports a failed restore).
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

// ---- MeshShape tree validation -------------------------------------------------------------------------------

using NodeCodec = JPH::NodeCodecQuadTreeHalfFloat;
using TriangleCodec = JPH::TriangleCodecIndexed8BitPackSOA4Flags;

template <class T>
T load(std::span<const uint8_t> p_bytes, size_t p_at) {
	T v;
	std::memcpy(&v, p_bytes.data() + p_at, sizeof(T));
	return v;
}

bool finite_float3(const JPH::Float3 &p_v) {
	return std::isfinite(p_v.x) && std::isfinite(p_v.y) && std::isfinite(p_v.z);
}

// Jolt's walks (NodeCodec::DecodingContext::WalkTree and the cast visitors' distance stacks) keep the pending
// children of every level on a fixed stack of NodeCodec::StackSize entries: a node d levels below the root sits at
// slot 3d and pushes 4, and only a debug assert guards the bound. 40 levels leave that stack 4 slots of headroom;
// Jolt's builder makes trees of about log4(triangles / 8) levels (a 30 k-triangle tile: about 8).
constexpr uint32_t kMaxTreeDepth = 40;
constexpr uint32_t kPaddingCount = NodeCodec::TRIANGLE_COUNT_MASK; // a child slot without data
// MeshShape's private layout constants in Jolt 5.6 (MeshShape.h: NumTriangleBits, MaxTrianglesPerLeaf,
// FLAGS_MATERIAL_MASK). The cache keys carry jolt_build_id(), so a Jolt bump re-cooks every tile, and sim.world.unit's
// blob round trip fails if the layout moves.
constexpr uint32_t kMeshTriangleBits = 3;
constexpr uint32_t kMeshMaxTrianglesPerLeaf = 1u << kMeshTriangleBits;
constexpr uint8_t kMeshMaterialMask = (1u << 5) - 1;
constexpr uint32_t kSubShapeIdBits = JPH::SubShapeID::MaxBits;

// Walks every node and triangle block of a MeshShape tree as MeshShape (Jolt 5.6) lays it out, before any Jolt
// code reads it: the header and triangle header; every node and block inside the buffer, each visited once (no
// cycles, no sharing) and no deeper than kMaxTreeDepth; leaf triangle counts 1..MaxTrianglesPerLeaf; block ids
// that fit the header's block-id bits (sub-shape ids decode through them); every vertex index of every block slot
// (padding slots included: the decoders gather all four) inside the buffer; per-triangle user data present, inside
// the buffer and below p_tags; no material bits (the static world gives its meshes no materials). r_triangles is
// the triangle count.
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
	if (!finite_float3(header.mRootBoundsMin) || !finite_float3(header.mRootBoundsMax) || !finite_float3(triangle_header.mOffset) ||
			!finite_float3(triangle_header.mScale)) {
		return core::make_error("mesh tree: non-finite header bounds");
	}
	const uint32_t block_bits = header.mBlockIDBits;
	if (block_bits > uint32_t(NodeCodec::OFFSET_BITS) || block_bits + kMeshTriangleBits > kSubShapeIdBits) {
		return core::make_error("mesh tree: " + std::to_string(block_bits) + " block id bits");
	}
	std::vector<uint8_t> seen(size / 4 + 1, 0); // per 4-byte slot: a node or block starts there
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
			continue; // never visited by Jolt's walks
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
		// A triangle leaf: header, ceil(count / 4) blocks, the user data words.
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

// The MeshShape stream: u8 sub type, u64 user data, u32 tree bytes, the tree (Shape / MeshShape SaveBinaryState).
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

// A u32 count of elements that take p_element_bytes each in the blob: fails (nothing sized) past the bytes left.
size_t counted(core::BlobReader &r_reader, size_t p_element_bytes) {
	const uint32_t n = r_reader.u32();
	if (r_reader.ok() && n > r_reader.remaining() / p_element_bytes) {
		r_reader.fail("count " + std::to_string(n) + " larger than the blob can hold");
		return 0;
	}
	return r_reader.ok() ? n : 0;
}

bool finite_vec(const Vec3 &p_v) {
	return std::isfinite(p_v[0]) && std::isfinite(p_v[1]) && std::isfinite(p_v[2]);
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

} // namespace

// ---- tile blobs ---------------------------------------------------------------------------------------------

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
	if (!finite_vec(info.min) || !finite_vec(info.max)) {
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

// ---- world index ----------------------------------------------------------------------------------------------

std::vector<uint8_t> encode_static_world_index(const StaticWorldIndex &p_index) {
	core::BlobWriter w;
	w.magic(kWorldMagic, kStaticWorldFormatVersion);
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
	return w.take();
}

core::Result<StaticWorldIndex> decode_static_world_index(std::span<const uint8_t> p_blob) {
	core::BlobReader r(p_blob);
	const uint32_t version = r.magic(kWorldMagic);
	if (r.ok() && version != kStaticWorldFormatVersion) {
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

// ---- keys -------------------------------------------------------------------------------------------------------

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
	in.format_version = kStaticWorldFormatVersion;
	in.jolt_version = jolt_build_id();
	in.params = "source=" + p_source_digest.hex() + ";level=" + core::to_lower_ascii(p_level) + ";mode=" + core::to_lower_ascii(p_mode) +
			";soup=" + std::to_string(core::level::kCollisionSoupVersion) + ";terrain=1;terrain_spacing=" + float_bits_hex(p_options.terrain_spacing) + ";" +
			build_params(p_options.build);
	// S1 (docs/design/s1.md §5.2): a terrain cover changes the soup; an empty one leaves M1's key text as it was.
	if (!p_options.terrain_cover.empty()) {
		in.params += ";cover=" + terrain_cover_digest(p_options.terrain_cover).hex();
		if (!p_options.cover_water.empty()) {
			in.params += ";water=" + cover_water_digest(p_options.cover_water).hex();
		}
	}
	return in;
}

// ---- load ---------------------------------------------------------------------------------------------------------

namespace {

// The warm path: every tile the index names, restored from its "jolt-tile" entry on the build lanes. nullopt when
// any is missing or does not decode (a damaged one is removed); the caller then takes the cold path.
std::optional<std::unique_ptr<StaticWorldShapes::Impl>> restore_world(core::Cache &r_cache, const StaticWorldIndex &p_index,
		const StaticWorldBuildOptions &p_options, std::shared_ptr<void> p_runtime) {
	const Clock::time_point start = Clock::now();
	std::vector<std::optional<StaticWorldShapes::Impl::Tile>> tiles(p_index.tiles.size());
	std::vector<uint64_t> bytes(p_index.tiles.size(), 0);
	std::atomic<bool> failed{ false };
	core::parallel_for(
			p_index.tiles.size(),
			[&](size_t p_at) {
				if (failed.load(std::memory_order_relaxed)) {
					return;
				}
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
						r_cache.remove(*key); // passed the cache's checks but not the decoder
					}
					failed = true;
					return;
				}
				bytes[p_at] = got.entry.payload().size();
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
		impl->stats.cache_bytes += bytes[i];
	}
	impl->stats.tiles_hit = impl->tiles.size();
	impl->stats.world_hit = true;
	impl->stats.seconds_restore = seconds_since(start);
	return impl;
}

} // namespace

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

	// Warm: the source digest names the world index, which names every tile.
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
				build.tiles_done->store(0, std::memory_order_relaxed); // the cold path counts again
			}
		}
	}

	// Cold: the soup, then every tile through its content-keyed entry, then the index.
	Clock::time_point clock = Clock::now();
	core::level::SoupOptions soup_options;
	soup_options.threads = p_options.soup_threads;
	soup_options.terrain_spacing = p_options.terrain_spacing;
	soup_options.progress = p_options.soup_progress;
	soup_options.terrain_cover = p_options.terrain_cover; // S1 (empty for M1)
	soup_options.cover_water = p_options.cover_water; // S1 water rule (empty for M1)
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
	bool stored = false;
	core::Result<std::unique_ptr<StaticWorldShapes::Impl>> impl = cook_static_tiles(
			cache, keys,
			[&](size_t p_tile, std::vector<StaticTriangle> &r_scratch) -> core::Result<std::span<const StaticTriangle>> {
				BF3_TRY(soup_tile_triangles(soup->tiles[p_tile], r_scratch));
				return std::span<const StaticTriangle>(r_scratch);
			},
			build, std::move(runtime), &stored);
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
		const std::vector<uint8_t> blob = encode_static_world_index(index);
		if (cache->put(*world_key, blob).status == core::CacheStatus::Ok) {
			w.stats.cache_bytes += blob.size();
		}
	}
	*soup = core::level::CollisionSoup(); // free the triangles before the shapes are handed out
	w.stats.seconds_total = seconds_since(start);
	return StaticWorldBuilder::finish(std::move(*impl));
}

// ---- S1: the terrain cover's digest -------------------------------------------------------------------------

core::Sha256Digest cover_water_digest(std::span<const core::level::WaterPlane> p_water) {
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

core::Sha256Digest terrain_cover_digest(std::span<const std::array<int32_t, 2>> p_tiles) {
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

} // namespace bf3::sim
