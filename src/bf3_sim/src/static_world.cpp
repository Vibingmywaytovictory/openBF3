// The static level world as Jolt MeshShapes (docs/design/m1.md §5.1-§5.3, §5.5): tiles cooked in parallel, one
// shape per (tile, layer), per-triangle user data = the tile's tag index; the soup and loose-triangle builds and
// the per-tile "jolt-tile" cache loop. The blobs, keys and load_static_world_cached are in static_world_cache.cpp.
#include "static_world_internal.h"

#include "jolt_runtime.h"

#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/digest.h"
#include "bf3/core/level/collision_soup.h"
#include "bf3/core/thread_pool.h"

#include <Jolt/Geometry/IndexedTriangle.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace bf3::sim {

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point p_start) {
	return std::chrono::duration<double>(Clock::now() - p_start).count();
}

uint32_t float_bits(float p_value) {
	uint32_t bits;
	std::memcpy(&bits, &p_value, sizeof(bits));
	return bits;
}

void put_u32(std::vector<uint8_t> &r_out, uint32_t p_value) {
	for (int i = 0; i < 4; ++i) {
		r_out.push_back(uint8_t(p_value >> (i * 8)));
	}
}

uint64_t tag_key(const SurfaceTag &p_tag) {
	return uint64_t(p_tag.material) | (uint64_t(p_tag.surface) << 32) | (uint64_t(p_tag.body) << 40) | (uint64_t(p_tag.flags) << 48) |
			(uint64_t(p_tag.reserved) << 56);
}

struct VertexKey {
	std::array<uint32_t, 3> bits{};
	bool operator==(const VertexKey &) const = default;
};

struct VertexKeyHash {
	size_t operator()(const VertexKey &p_key) const {
		uint64_t h = 0xcbf29ce484222325ull;
		for (uint32_t b : p_key.bits) {
			h = (h ^ b) * 0x100000001b3ull;
		}
		return size_t(h);
	}
};

const StaticTileInfo &empty_tile_info() {
	static const StaticTileInfo info;
	return info;
}

const SurfaceTag &empty_tag() {
	static const SurfaceTag tag;
	return tag;
}

core::Status check_options(const StaticWorldBuildOptions &p_options) {
	if (!std::isfinite(p_options.active_edge_degrees) || p_options.active_edge_degrees < 0.0f || p_options.active_edge_degrees > 180.0f) {
		return core::make_error("static world: active_edge_degrees " + std::to_string(p_options.active_edge_degrees) + " is outside 0..180");
	}
	return core::Status::success();
}

} // namespace

JPH::Vec3 static_tile_origin(int32_t p_x, int32_t p_z) {
	return JPH::Vec3((float(p_x) + 0.5f) * kStaticTileSize, 0.0f, (float(p_z) + 0.5f) * kStaticTileSize);
}

std::string jolt_build_id() {
#if defined(JPH_CROSS_PLATFORM_DETERMINISTIC)
	constexpr int kDeterministic = 1;
#else
	constexpr int kDeterministic = 0;
#endif
#if defined(JPH_DOUBLE_PRECISION)
	constexpr int kDouble = 1;
#else
	constexpr int kDouble = 0;
#endif
	return std::to_string(JPH_VERSION_MAJOR) + "." + std::to_string(JPH_VERSION_MINOR) + "." + std::to_string(JPH_VERSION_PATCH) +
			";det=" + std::to_string(kDeterministic) + ";dp=" + std::to_string(kDouble);
}

// ---- StaticWorldShapes ---------------------------------------------------------------------------------------

StaticWorldShapes::StaticWorldShapes(std::unique_ptr<Impl> p_impl) :
		impl_(std::move(p_impl)) {}

StaticWorldShapes::~StaticWorldShapes() = default;

size_t StaticWorldShapes::tile_count() const {
	return impl_->tiles.size();
}

const StaticTileInfo &StaticWorldShapes::tile(size_t p_index) const {
	return p_index < impl_->tiles.size() ? impl_->tiles[p_index].info : empty_tile_info();
}

const SurfaceTag &StaticWorldShapes::tag(size_t p_tile, uint32_t p_tag) const {
	if (p_tile >= impl_->tiles.size() || p_tag >= impl_->tiles[p_tile].tags.size()) {
		return empty_tag();
	}
	return impl_->tiles[p_tile].tags[p_tag];
}

const StaticWorldStats &StaticWorldShapes::stats() const {
	return impl_->stats;
}

Vec3 StaticWorldShapes::min() const {
	return impl_->min;
}

Vec3 StaticWorldShapes::max() const {
	return impl_->max;
}

std::shared_ptr<const StaticWorldShapes> StaticWorldBuilder::finish(std::unique_ptr<StaticWorldShapes::Impl> p_impl) {
	StaticWorldShapes::Impl &w = *p_impl;
	StaticWorldStats &s = w.stats;
	s.tiles = w.tiles.size();
	s.shapes = 0;
	s.triangles = {};
	s.tags = 0;
	s.shape_bytes = 0;
	bool any = false;
	for (const StaticWorldShapes::Impl::Tile &tile : w.tiles) {
		s.tags += tile.tags.size();
		for (size_t layer = 0; layer < kStaticLayerCount; ++layer) {
			s.triangles[layer] += tile.info.triangles[layer];
			if (tile.shapes[layer] != nullptr) {
				++s.shapes;
				s.shape_bytes += tile.shapes[layer]->GetStats().mSizeBytes;
			}
		}
		if (tile.tags.empty()) {
			continue; // no triangles: no bounds
		}
		for (size_t k = 0; k < 3; ++k) {
			w.min[k] = any ? std::min(w.min[k], tile.info.min[k]) : tile.info.min[k];
			w.max[k] = any ? std::max(w.max[k], tile.info.max[k]) : tile.info.max[k];
		}
		any = true;
	}
	if (s.seconds_total == 0.0) {
		s.seconds_total = s.seconds_soup + s.seconds_cook + s.seconds_restore;
	}
	return std::shared_ptr<const StaticWorldShapes>(new StaticWorldShapes(std::move(p_impl)));
}

// ---- one tile ---------------------------------------------------------------------------------------------

core::Result<StaticTileInput> prepare_static_tile(int32_t p_x, int32_t p_z, std::span<const StaticTriangle> p_triangles) {
	const std::string where = "static tile (" + std::to_string(p_x) + ", " + std::to_string(p_z) + ")";
	StaticTileInput tile;
	tile.info.x = p_x;
	tile.info.z = p_z;

	// Tags, bounds and the content digest over every input triangle, in input order.
	std::unordered_map<uint64_t, uint32_t> tag_index;
	std::vector<uint32_t> &triangle_tags = tile.triangle_tags;
	triangle_tags.resize(p_triangles.size());
	core::Hasher hasher(core::HashAlgorithm::Sha256);
	std::vector<uint8_t> chunk;
	chunk.reserve(64 * 1024);
	constexpr size_t kTriangleBytes = 9 * 4 + 4 + 4 + 1;
	for (size_t i = 0; i < p_triangles.size(); ++i) {
		const StaticTriangle &t = p_triangles[i];
		if (uint8_t(t.layer) >= kStaticLayerCount) {
			return core::make_error(where + ": triangle " + std::to_string(i) + " has layer " + std::to_string(int(t.layer)));
		}
		for (const Vec3 &v : t.v) {
			for (size_t k = 0; k < 3; ++k) {
				if (!std::isfinite(v[k])) {
					return core::make_error(where + ": triangle " + std::to_string(i) + " has a non-finite corner");
				}
				const bool first = i == 0 && &v == &t.v[0];
				tile.info.min[k] = first ? v[k] : std::min(tile.info.min[k], v[k]);
				tile.info.max[k] = first ? v[k] : std::max(tile.info.max[k], v[k]);
			}
		}
		const auto [it, inserted] = tag_index.try_emplace(tag_key(t.tag), uint32_t(tile.tags.size()));
		if (inserted) {
			tile.tags.push_back(t.tag);
		}
		triangle_tags[i] = it->second;

		if (chunk.size() + kTriangleBytes > chunk.capacity()) {
			hasher.update(chunk);
			chunk.clear();
		}
		for (const Vec3 &v : t.v) {
			for (float c : v) {
				put_u32(chunk, float_bits(c));
			}
		}
		put_u32(chunk, t.tag.material);
		chunk.push_back(t.tag.surface);
		chunk.push_back(t.tag.body);
		chunk.push_back(t.tag.flags);
		chunk.push_back(t.tag.reserved);
		chunk.push_back(uint8_t(t.layer));
	}
	hasher.update(chunk);
	if (!hasher.finish(tile.info.content.bytes)) {
		return core::make_error(where + ": SHA-256 failed");
	}
	tile.info.tags = uint32_t(tile.tags.size());
	return tile;
}

core::Result<StaticWorldShapes::Impl::Tile> cook_static_tile(StaticTileInput p_input, std::span<const StaticTriangle> p_triangles,
		const StaticWorldBuildOptions &p_options) {
	const std::string where = "static tile (" + std::to_string(p_input.info.x) + ", " + std::to_string(p_input.info.z) + ")";
	if (p_input.triangle_tags.size() != p_triangles.size()) {
		return core::make_error(where + ": the prepared input does not match the triangles");
	}
	const std::vector<uint32_t> &triangle_tags = p_input.triangle_tags;
	StaticWorldShapes::Impl::Tile tile;
	tile.info = p_input.info;
	tile.tags = std::move(p_input.tags);

	// One MeshShape per present layer; vertices relative to the tile origin, deduplicated by exact bit pattern.
	const JPH::Vec3 origin = static_tile_origin(tile.info.x, tile.info.z);
	const float cos_threshold = std::cos(p_options.active_edge_degrees * (3.14159265358979323846f / 180.0f));
	for (size_t layer = 0; layer < kStaticLayerCount; ++layer) {
		if (layer == size_t(StaticLayer::Raycast) && !p_options.include_raycast) {
			continue;
		}
		JPH::VertexList vertices;
		JPH::IndexedTriangleList triangles;
		std::unordered_map<VertexKey, uint32_t, VertexKeyHash> vertex_index;
		for (size_t i = 0; i < p_triangles.size(); ++i) {
			const StaticTriangle &t = p_triangles[i];
			if (size_t(t.layer) != layer) {
				continue;
			}
			uint32_t idx[3];
			for (size_t c = 0; c < 3; ++c) {
				const JPH::Float3 rel(t.v[c][0] - origin.GetX(), t.v[c][1] - origin.GetY(), t.v[c][2] - origin.GetZ());
				const VertexKey key{ { float_bits(rel.x), float_bits(rel.y), float_bits(rel.z) } };
				const auto [it, inserted] = vertex_index.try_emplace(key, uint32_t(vertices.size()));
				if (inserted) {
					vertices.push_back(rel);
				}
				idx[c] = it->second;
			}
			triangles.push_back(JPH::IndexedTriangle(idx[0], idx[1], idx[2], 0, triangle_tags[i]));
		}
		if (triangles.empty()) {
			continue;
		}
		const uint32_t input = uint32_t(triangles.size());
		JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles)); // Sanitize drops degenerate ones
		settings.SetEmbedded();
		settings.mPerTriangleUserData = true;
		settings.mActiveEdgeCosThresholdAngle = cos_threshold;
		if (settings.mIndexedTriangles.empty()) {
			continue; // every triangle was degenerate for Jolt: no shape, counted as 0
		}
		const JPH::ShapeSettings::ShapeResult result = settings.Create();
		if (result.HasError()) {
			return core::make_error(where + " layer " + std::to_string(layer) + ": Jolt: " + std::string(result.GetError().c_str()));
		}
		tile.shapes[layer] = result.Get();
		tile.info.triangles[layer] = input;
	}
	return tile;
}

core::Result<StaticWorldShapes::Impl::Tile> cook_static_tile(int32_t p_x, int32_t p_z, std::span<const StaticTriangle> p_triangles,
		const StaticWorldBuildOptions &p_options) {
	core::Result<StaticTileInput> input = prepare_static_tile(p_x, p_z, p_triangles);
	if (!input) {
		return input.error();
	}
	return cook_static_tile(std::move(*input), p_triangles, p_options);
}

core::Status soup_tile_triangles(const core::level::SoupTile &p_tile, std::vector<StaticTriangle> &r_out) {
	r_out.clear();
	r_out.reserve(p_tile.triangles.size());
	for (const core::level::SoupTriangle &s : p_tile.triangles) {
		if (s.body >= kStaticLayerCount) {
			return core::make_error("static tile (" + std::to_string(p_tile.x) + ", " + std::to_string(p_tile.z) + "): soup body " +
					std::to_string(int(s.body)) + " has no layer");
		}
		StaticTriangle t;
		t.v = s.v;
		t.tag.material = s.material;
		t.tag.surface = s.surface;
		t.tag.body = s.body;
		t.tag.flags = s.flags;
		t.layer = StaticLayer(s.body);
		r_out.push_back(t);
	}
	return core::Status::success();
}

// ---- the tile loop ----------------------------------------------------------------------------------------

namespace {

// One tile through its "jolt-tile" entry: decode a hit (it must be this input's tile), else cook and put.
core::Result<StaticWorldShapes::Impl::Tile> cached_static_tile(core::Cache &r_cache, int32_t p_x, int32_t p_z, std::span<const StaticTriangle> p_triangles,
		const StaticWorldBuildOptions &p_options, core::CachedItem &r_item) {
	core::Result<StaticTileInput> input = prepare_static_tile(p_x, p_z, p_triangles);
	if (!input) {
		return input.error();
	}
	const std::optional<core::CacheKey> key = core::make_cache_key(static_tile_key_inputs(input->info.content, p_options));
	if (!key) {
		return core::make_error("static tile: no cache key");
	}
	std::optional<StaticWorldShapes::Impl::Tile> tile;
	const auto accept = [&](std::span<const uint8_t> p_blob) -> core::Status {
		core::Result<StaticWorldShapes::Impl::Tile> decoded = decode_static_tile(p_blob);
		if (!decoded) {
			return decoded.error();
		}
		const StaticTileInfo &a = decoded->info;
		const StaticTileInfo &b = input->info;
		bool same = a.x == b.x && a.z == b.z && a.content == b.content && a.min == b.min && a.max == b.max && decoded->tags == input->tags;
		for (size_t layer = 0; layer < kStaticLayerCount && same; ++layer) {
			const bool wanted = layer != size_t(StaticLayer::Raycast) || p_options.include_raycast;
			same = wanted || decoded->shapes[layer] == nullptr;
		}
		if (!same) {
			return core::make_error("jolt-tile entry describes another tile");
		}
		tile = std::move(*decoded);
		return core::Status::success();
	};
	const auto produce = [&]() -> core::Result<std::vector<uint8_t>> {
		core::Result<StaticWorldShapes::Impl::Tile> cooked = cook_static_tile(std::move(*input), p_triangles, p_options);
		if (!cooked) {
			return cooked.error();
		}
		tile = std::move(*cooked);
		return encode_static_tile(*tile);
	};
	core::Result<core::CachedItem> item = core::cached_convert(&r_cache, *key, accept, produce);
	if (!item) {
		return item.error();
	}
	r_item = *item;
	if (!tile) {
		return core::make_error("static tile: the cache gave no tile");
	}
	return std::move(*tile);
}

} // namespace

core::Result<std::unique_ptr<StaticWorldShapes::Impl>> cook_static_tiles(core::Cache *p_cache, const std::vector<std::array<int32_t, 2>> &p_keys,
		const StaticTileSource &p_source, const StaticWorldBuildOptions &p_options, std::shared_ptr<void> p_runtime, bool *r_stored) {
	const Clock::time_point start = Clock::now();
	core::Cache *cache = (p_cache != nullptr && p_cache->is_open()) ? p_cache : nullptr;
	std::vector<std::optional<core::Result<StaticWorldShapes::Impl::Tile>>> cooked(p_keys.size());
	std::vector<core::CachedItem> items(p_keys.size());
	core::parallel_for(
			p_keys.size(),
			[&](size_t p_index) {
				std::vector<StaticTriangle> scratch;
				const core::Result<std::span<const StaticTriangle>> triangles = p_source(p_index, scratch);
				if (!triangles.ok()) {
					cooked[p_index].emplace(triangles.error());
				} else if (cache != nullptr) {
					cooked[p_index].emplace(cached_static_tile(*cache, p_keys[p_index][0], p_keys[p_index][1], *triangles, p_options, items[p_index]));
				} else {
					cooked[p_index].emplace(cook_static_tile(p_keys[p_index][0], p_keys[p_index][1], *triangles, p_options));
				}
				if (p_options.tiles_done != nullptr) {
					p_options.tiles_done->fetch_add(1, std::memory_order_relaxed);
				}
			},
			p_options.threads);
	auto impl = std::make_unique<StaticWorldShapes::Impl>();
	impl->runtime = std::move(p_runtime);
	impl->tiles.reserve(p_keys.size());
	bool stored = cache != nullptr;
	for (size_t i = 0; i < cooked.size(); ++i) {
		if (!cooked[i]->ok()) {
			return cooked[i]->error();
		}
		impl->tiles.push_back(std::move(cooked[i]->value()));
		if (cache == nullptr) {
			++impl->stats.tiles_cooked;
			continue;
		}
		const core::CachedItem &item = items[i];
		impl->stats.cache_bytes += item.payload_bytes;
		if (item.outcome == core::CacheOutcome::Hit) {
			++impl->stats.tiles_hit;
		} else {
			++impl->stats.tiles_cooked;
		}
		stored = stored && item.outcome != core::CacheOutcome::Uncached;
	}
	if (r_stored != nullptr) {
		*r_stored = stored;
	}
	impl->stats.seconds_cook = seconds_since(start);
	return impl;
}

// ---- builds -----------------------------------------------------------------------------------------------

core::Result<std::shared_ptr<const StaticWorldShapes>> build_static_world_cached(core::Cache *p_cache, std::span<const StaticTriangle> p_triangles,
		const StaticWorldBuildOptions &p_options) {
	BF3_TRY(check_options(p_options));
	std::shared_ptr<void> runtime = jolt_runtime_acquire();
	// Tile every triangle by the soup's rule (millimetre-quantised centroid, floor of / 64 m), keep input order.
	struct Keyed {
		std::array<int32_t, 2> tile;
		uint32_t index;
	};
	if (p_triangles.size() > core::level::kMaxSoupTriangles) {
		return core::make_error("static world: " + std::to_string(p_triangles.size()) + " triangles exceed the soup limit");
	}
	std::vector<Keyed> keyed;
	keyed.reserve(p_triangles.size());
	for (size_t i = 0; i < p_triangles.size(); ++i) {
		std::array<int64_t, 9> q;
		if (!core::level::soup_quantise(p_triangles[i].v, q)) {
			return core::make_error("static world: triangle " + std::to_string(i) + " has a non-finite or out-of-range corner");
		}
		keyed.push_back({ core::level::soup_tile_of(q), uint32_t(i) });
	}
	std::stable_sort(keyed.begin(), keyed.end(), [](const Keyed &p_a, const Keyed &p_b) { return p_a.tile < p_b.tile; });
	std::vector<std::array<int32_t, 2>> keys;
	std::vector<size_t> first; // keyed range of tile k: [first[k], first[k + 1])
	for (size_t i = 0; i < keyed.size(); ++i) {
		if (i == 0 || keyed[i].tile != keyed[i - 1].tile) {
			keys.push_back(keyed[i].tile);
			first.push_back(i);
		}
	}
	first.push_back(keyed.size());
	core::Result<std::unique_ptr<StaticWorldShapes::Impl>> impl = cook_static_tiles(
			p_cache, keys,
			[&](size_t p_tile, std::vector<StaticTriangle> &r_scratch) -> core::Result<std::span<const StaticTriangle>> {
				r_scratch.clear();
				for (size_t i = first[p_tile]; i < first[p_tile + 1]; ++i) {
					r_scratch.push_back(p_triangles[keyed[i].index]);
				}
				return std::span<const StaticTriangle>(r_scratch);
			},
			p_options, std::move(runtime));
	if (!impl) {
		return impl.error();
	}
	return StaticWorldBuilder::finish(std::move(*impl));
}

core::Result<std::shared_ptr<const StaticWorldShapes>> build_static_world(std::span<const StaticTriangle> p_triangles,
		const StaticWorldBuildOptions &p_options) {
	return build_static_world_cached(nullptr, p_triangles, p_options);
}

core::Result<std::shared_ptr<const StaticWorldShapes>> build_static_world(const core::level::CollisionSoup &p_soup, const StaticWorldBuildOptions &p_options) {
	BF3_TRY(check_options(p_options));
	std::shared_ptr<void> runtime = jolt_runtime_acquire();
	std::vector<std::array<int32_t, 2>> keys;
	keys.reserve(p_soup.tiles.size());
	for (const core::level::SoupTile &tile : p_soup.tiles) {
		keys.push_back({ tile.x, tile.z });
	}
	core::Result<std::unique_ptr<StaticWorldShapes::Impl>> impl = cook_static_tiles(
			nullptr, keys,
			[&](size_t p_tile, std::vector<StaticTriangle> &r_scratch) -> core::Result<std::span<const StaticTriangle>> {
				BF3_TRY(soup_tile_triangles(p_soup.tiles[p_tile], r_scratch));
				return std::span<const StaticTriangle>(r_scratch);
			},
			p_options, std::move(runtime));
	if (!impl) {
		return impl.error();
	}
	return StaticWorldBuilder::finish(std::move(*impl));
}

} // namespace bf3::sim
