// The navmesh through the derived-data cache (nav.h, docs/design/s1.md §5.4): the "nav-tile" and "nav-world" blobs,
// their keys and load_nav_world_cached (warm: source digest -> world index -> tile restores; cold: soup -> tile
// inputs -> content-keyed tile bakes -> world index).
//
// "OBF3NTIL" v1 (one tile), little endian: magic, version; i32 x, i32 z; u32 input triangles, polys, vertices, detail
// triangles, drop links; the input digest (32 bytes); u32 data bytes, the Detour tile data (dtCreateNavMeshData's
// output, pristine; 0 bytes for a tile without a walkable poly).
// "OBF3NWLD" v1 (one level x mode): u32 tile count; per tile: i32 x, i32 z, the input digest, u32 polys, u32 data
// bytes; strictly increasing (x, z).
#include "nav_internal.h"

#include "static_world_internal.h"

#include "bf3/core/cache/blob_io.h"
#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/ids.h"
#include "bf3/core/level/collision_soup.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/source_digest.h"
#include "bf3/core/thread_pool.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>

namespace bf3::sim {

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point p_start) {
	return std::chrono::duration<double>(Clock::now() - p_start).count();
}

constexpr const char *kTileMagic = "OBF3NTIL";
constexpr const char *kWorldMagic = "OBF3NWLD";
constexpr size_t kTileHeaderBytes = 8 + 4 + 4 + 4 + 5 * 4 + 32 + 4;
constexpr size_t kWorldTileBytes = 4 + 4 + 32 + 4 + 4;

std::string bits_hex(float p_value) {
	uint32_t bits;
	std::memcpy(&bits, &p_value, sizeof(bits));
	char buf[16];
	std::snprintf(buf, sizeof(buf), "%08x", bits);
	return buf;
}

std::string layer_text(uint32_t p_mask) {
	return ";layers=" + std::to_string(p_mask);
}

struct WorldEntry {
	NavTileKey tile{};
	core::Sha256Digest input{};
	uint32_t polys = 0;
	uint32_t data_bytes = 0;
};

std::vector<uint8_t> encode_world_index(const std::vector<WorldEntry> &p_tiles) {
	core::BlobWriter w;
	w.magic(kWorldMagic, kNavFormatVersion);
	w.u64(uint64_t(p_tiles.size())); // BlobReader::count reads a u64
	for (const WorldEntry &e : p_tiles) {
		w.i32(e.tile[0]);
		w.i32(e.tile[1]);
		w.raw(e.input.bytes.data(), e.input.bytes.size());
		w.u32(e.polys);
		w.u32(e.data_bytes);
	}
	return w.take();
}

core::Result<std::vector<WorldEntry>> decode_world_index(std::span<const uint8_t> p_blob) {
	core::BlobReader r(p_blob);
	if (r.magic(kWorldMagic) != kNavFormatVersion) {
		return r.ok() ? core::make_error("nav-world: version") : r.error("nav-world");
	}
	const size_t count = r.count(kWorldTileBytes, kMaxNavTiles);
	std::vector<WorldEntry> out;
	out.reserve(count);
	for (size_t i = 0; i < count && r.ok(); ++i) {
		WorldEntry e;
		e.tile[0] = r.i32();
		e.tile[1] = r.i32();
		for (uint8_t &b : e.input.bytes) {
			b = r.u8();
		}
		e.polys = r.u32();
		e.data_bytes = r.u32();
		if (!out.empty() && !(out.back().tile < e.tile)) {
			r.fail("tiles not in (x, z) order");
		}
		out.push_back(e);
	}
	if (!r.ok()) {
		return r.error("nav-world");
	}
	if (!r.at_end()) {
		return core::make_error("nav-world: trailing bytes");
	}
	return out;
}

} // namespace

std::vector<uint8_t> encode_nav_tile(const NavTileInfo &p_info, std::span<const uint8_t> p_data) {
	core::BlobWriter w;
	w.magic(kTileMagic, kNavFormatVersion);
	w.i32(p_info.tile[0]);
	w.i32(p_info.tile[1]);
	w.u32(p_info.input_triangles);
	w.u32(p_info.polys);
	w.u32(p_info.vertices);
	w.u32(p_info.detail_triangles);
	w.u32(p_info.drop_links);
	w.raw(p_info.input.bytes.data(), p_info.input.bytes.size());
	w.u32(uint32_t(p_data.size()));
	w.raw(p_data.data(), p_data.size());
	return w.take();
}

core::Status decode_nav_tile(std::span<const uint8_t> p_blob, const NavParams &p_params, NavTileInfo &r_info, std::vector<uint8_t> &r_data) {
	core::BlobReader r(p_blob);
	if (r.magic(kTileMagic) != kNavFormatVersion) {
		return r.ok() ? core::make_error("nav-tile: version") : r.error("nav-tile");
	}
	NavTileInfo info;
	info.tile[0] = r.i32();
	info.tile[1] = r.i32();
	info.input_triangles = r.u32();
	info.polys = r.u32();
	info.vertices = r.u32();
	info.detail_triangles = r.u32();
	info.drop_links = r.u32();
	for (uint8_t &b : info.input.bytes) {
		b = r.u8();
	}
	const uint32_t size = r.u32();
	std::vector<uint8_t> data;
	r.array(size, data);
	if (!r.ok()) {
		return r.error("nav-tile");
	}
	if (!r.at_end()) {
		return core::make_error("nav-tile: trailing bytes");
	}
	if (data.empty() != (info.polys == 0)) {
		return core::make_error("nav-tile: data and poly count disagree");
	}
	if (!data.empty()) {
		BF3_TRY(validate_detour_tile(data, p_params, info.tile));
		dtMeshHeader h;
		std::memcpy(&h, data.data(), sizeof(h));
		if (uint32_t(h.polyCount - h.offMeshConCount) != info.polys || uint32_t(h.offMeshConCount) != info.drop_links) {
			return core::make_error("nav-tile: the info does not match the Detour data");
		}
	}
	info.data_bytes = uint32_t(data.size());
	r_info = info;
	r_data = std::move(data);
	return core::Status::success();
}

std::vector<uint8_t> encode_nav_tile_blob(const NavWorld &p_world, size_t p_tile) {
	const NavWorld::Impl &w = p_world.impl();
	if (p_tile >= w.tiles.size()) {
		return {};
	}
	return encode_nav_tile(w.tiles[p_tile], w.data[p_tile]);
}

core::Result<std::shared_ptr<const NavWorld>> decode_nav_tile_blobs(std::span<const std::span<const uint8_t>> p_tiles, const NavParams &p_params) {
	BF3_TRY(check_nav_params(p_params));
	if (p_tiles.size() > kMaxNavTiles) {
		return core::make_error("nav: too many tile blobs");
	}
	struct Decoded {
		NavTileInfo info;
		std::vector<uint8_t> data;
	};
	std::vector<Decoded> tiles(p_tiles.size());
	for (size_t i = 0; i < p_tiles.size(); ++i) {
		BF3_TRY(decode_nav_tile(p_tiles[i], p_params, tiles[i].info, tiles[i].data));
	}
	std::sort(tiles.begin(), tiles.end(), [](const Decoded &p_a, const Decoded &p_b) { return p_a.info.tile < p_b.info.tile; });
	std::vector<NavTileInfo> infos;
	std::vector<std::vector<uint8_t>> data;
	for (Decoded &d : tiles) {
		if (!infos.empty() && infos.back().tile == d.info.tile) {
			return core::make_error("nav: two blobs for one tile");
		}
		infos.push_back(d.info);
		data.push_back(std::move(d.data));
	}
	NavStats stats;
	stats.tiles_hit = infos.size();
	return NavWorldBuilder::finish(p_params, std::move(infos), std::move(data), stats);
}

core::CacheKeyInputs nav_tile_key_inputs(const core::Sha256Digest &p_input, const NavParams &p_params, bool p_link_checked) {
	core::CacheKeyInputs in;
	in.kind = kNavTileCacheKind;
	in.converter_version = kNavConverterVersion;
	in.engine = std::string(core::cache_engine_id());
	in.format_version = kNavFormatVersion;
	in.jolt_version = p_link_checked ? jolt_build_id() : std::string();
	in.params = "input=" + p_input.hex() + ";" + nav_params_text(p_params) + ";" + recast_build_id() + (p_link_checked ? ";links=jolt" : ";links=none");
	return in;
}

core::CacheKeyInputs nav_world_key_inputs(const core::Sha256Digest &p_source_digest, std::string_view p_level, std::string_view p_mode,
		const NavLoadOptions &p_options) {
	const bool link_checked = p_options.bake.link_shapes != nullptr && p_options.bake.params.drop_links;
	core::CacheKeyInputs in;
	in.kind = kNavWorldCacheKind;
	in.converter_version = kNavConverterVersion;
	in.engine = std::string(core::cache_engine_id());
	in.format_version = kNavFormatVersion;
	in.jolt_version = link_checked ? jolt_build_id() : std::string();
	in.params = "source=" + p_source_digest.hex() + ";level=" + core::to_lower_ascii(p_level) + ";mode=" + core::to_lower_ascii(p_mode) +
			";soup=" + std::to_string(core::level::kCollisionSoupVersion) + ";terrain_spacing=" + bits_hex(p_options.world.terrain_spacing) +
			";cover=" + terrain_cover_digest(p_options.world.terrain_cover).hex() +
			(p_options.world.cover_water.empty() ? std::string() : ";water=" + cover_water_digest(p_options.world.cover_water).hex()) +
			";region=" + terrain_cover_digest(p_options.region).hex() + ";" +
			nav_params_text(p_options.bake.params) + ";" + recast_build_id() + layer_text(p_options.bake.layer_mask) +
			(link_checked ? ";links=jolt" : ";links=none");
	return in;
}

core::Result<std::shared_ptr<const NavWorld>> load_nav_world_cached(core::Cache *p_cache, const core::level::LoadedLevel &p_level,
		const core::level::World &p_world, const core::level::CollisionSoup *p_soup, const StaticWorldShapes &p_shapes, const NavLoadOptions &p_options) {
	(void)p_shapes; // the link checks use p_options.bake.link_shapes (the same world, shared ownership for the lanes)
	const Clock::time_point start = Clock::now();
	const NavBakeOptions &bake = p_options.bake;
	BF3_TRY(check_nav_params(bake.params));
	std::vector<NavTileKey> region = p_options.region;
	std::sort(region.begin(), region.end());
	region.erase(std::unique(region.begin(), region.end()), region.end());
	core::Cache *cache = (p_cache != nullptr && p_cache->is_open()) ? p_cache : nullptr;
	const bool link_checked = bake.link_shapes != nullptr && bake.params.drop_links;

	std::optional<core::CacheKey> world_key;
	if (cache != nullptr) {
		const core::Result<core::level::SourceDigest> digest = core::level::level_source_digest(p_level);
		if (digest && digest->digest) {
			world_key = core::make_cache_key(nav_world_key_inputs(*digest->digest, p_level.info.id, p_level.mode, p_options));
		}
	}
	// Warm: the index names every tile's input digest.
	if (world_key) {
		core::GetResult got = cache->get(*world_key);
		if (got.status == core::CacheStatus::Ok) {
			core::Result<std::vector<WorldEntry>> index = decode_world_index(got.entry.payload());
			got.entry.reset();
			if (!index) {
				cache->remove(*world_key);
			} else {
				const Clock::time_point restore = Clock::now();
				std::vector<std::optional<std::pair<NavTileInfo, std::vector<uint8_t>>>> tiles(index->size());
				std::atomic<bool> failed{ false };
				core::parallel_for(
						index->size(),
						[&](size_t p_i) {
							if (failed.load(std::memory_order_relaxed)) {
								return;
							}
							const WorldEntry &e = (*index)[p_i];
							const std::optional<core::CacheKey> key = core::make_cache_key(nav_tile_key_inputs(e.input, bake.params, link_checked));
							if (!key) {
								failed = true;
								return;
							}
							core::GetResult tile = cache->get(*key);
							if (tile.status != core::CacheStatus::Ok) {
								failed = true;
								return;
							}
							NavTileInfo info;
							std::vector<uint8_t> data;
							const core::Status ok = decode_nav_tile(tile.entry.payload(), bake.params, info, data);
							tile.entry.reset();
							if (!ok || info.tile != e.tile || info.input != e.input || info.polys != e.polys || info.data_bytes != e.data_bytes) {
								if (!ok) {
									cache->remove(*key);
								}
								failed = true;
								return;
							}
							tiles[p_i].emplace(info, std::move(data));
						},
						bake.threads);
				if (!failed.load()) {
					std::vector<NavTileInfo> infos;
					std::vector<std::vector<uint8_t>> data;
					for (auto &t : tiles) {
						infos.push_back(t->first);
						data.push_back(std::move(t->second));
					}
					NavStats stats;
					stats.world_hit = true;
					stats.tiles_hit = infos.size();
					stats.seconds_restore = seconds_since(restore);
					core::Result<std::shared_ptr<const NavWorld>> world = NavWorldBuilder::finish(bake.params, std::move(infos), std::move(data), stats);
					if (world) {
						return world;
					}
				}
			}
		}
	}

	// Cold: the soup (the caller's, else built here), the inputs, the content-keyed tiles, then the index.
	std::optional<core::level::CollisionSoup> own_soup;
	const core::level::CollisionSoup *soup = p_soup;
	NavStats stats;
	if (soup == nullptr) {
		core::level::SoupOptions so;
		so.threads = p_options.world.soup_threads;
		so.terrain_spacing = p_options.world.terrain_spacing;
		so.terrain_cover = p_options.world.terrain_cover;
		so.cover_water = p_options.world.cover_water;
		so.progress = p_options.world.soup_progress;
		core::Result<core::level::CollisionSoup> built = core::level::build_collision_soup(p_level, p_world, so);
		if (!built) {
			return built.error();
		}
		own_soup = std::move(*built);
		soup = &*own_soup;
	}
	const Clock::time_point input_start = Clock::now();
	core::Result<std::vector<NavTileInput>> inputs = nav_tile_inputs_threads(*soup, region, bake.params, bake.layer_mask, bake.threads);
	if (!inputs) {
		return inputs.error();
	}
	stats.seconds_input = seconds_since(input_start);
	for (const NavTileInput &in : *inputs) {
		stats.far_triangles += in.far_triangles;
	}
	own_soup.reset();
	bool stored = false;
	core::Result<std::shared_ptr<const NavWorld>> world = bake_nav_inputs(std::move(*inputs), bake, stats, cache, &stored);
	if (!world) {
		return world.error();
	}
	if (world_key && stored) {
		std::vector<WorldEntry> index;
		for (size_t i = 0; i < (*world)->tile_count(); ++i) {
			const NavTileInfo &t = (*world)->tile(i);
			index.push_back({ t.tile, t.input, t.polys, t.data_bytes });
		}
		cache->put(*world_key, encode_world_index(index));
	}
	(void)start;
	return world;
}

} // namespace bf3::sim
