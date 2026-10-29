#include "bf3/core/level/combat_tiles_cache.h"

#include "bf3/core/cache/blob_io.h"
#include "bf3/core/cache/cache_key.h"
#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/source_digest.h"
#include "bf3/core/level/water_plane.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/core/util/text.h"

#include <bit>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>

namespace bf3::core::level {

namespace {

constexpr std::string_view kMagic = "CMBTILES";
constexpr size_t kMaxName = 256;
constexpr size_t kMaxModes = 64;
constexpr size_t kMaxTiles = size_t(1) << 22;

void put_polygon(BlobWriter &r_w, const std::vector<std::array<float, 2>> &p_polygon) {
	r_w.u64(p_polygon.size());
	r_w.array(p_polygon);
}

bool get_polygon(BlobReader &r_r, std::vector<std::array<float, 2>> &r_polygon) {
	const size_t n = r_r.count(sizeof(std::array<float, 2>), kMaxCombatAreaPoints);
	return r_r.array(n, r_polygon);
}

std::string margin_text(float p_margin) {
	char text[16];
	std::snprintf(text, sizeof(text), "%08x", std::bit_cast<uint32_t>(p_margin));
	return text;
}

}

std::vector<uint8_t> encode_combat_tiles(const LevelCombatTiles &p_tiles) {
	BlobWriter w;
	w.magic(kMagic, kCombatTilesFormatVersion);
	w.u64(p_tiles.modes.size());
	for (const std::string &mode : p_tiles.modes) {
		w.str(mode);
	}
	w.u64(p_tiles.areas.size());
	for (const ModeAreas &a : p_tiles.areas) {
		w.u64(a.combat_areas.size());
		for (const CombatAreaDef &c : a.combat_areas) {
			w.u32(c.trigger);
			w.u32(c.shape);
			w.i32(c.team);
			put_polygon(w, c.polygon);
			w.f32(c.height);
		}
		w.u64(a.objectives.size());
		for (const ModeObjectiveDef &o : a.objectives) {
			w.u8(uint8_t(o.kind));
			w.u32(o.object);
			w.f32(o.position.x);
			w.f32(o.position.y);
			w.f32(o.position.z);
			w.i32(o.team);
			w.str(o.scope_label);
		}
		w.u64(a.unwired_triggers.size());
		w.array(a.unwired_triggers);
	}
	w.u64(p_tiles.tiles.size());
	w.array(p_tiles.tiles);
	w.u64(p_tiles.polygon_tiles);
	w.u64(p_tiles.point_only_tiles);
	w.u64(p_tiles.points);
	w.u64(p_tiles.points_outside_areas);
	w.u64(p_tiles.water.size());
	for (const WaterPlane &p : p_tiles.water) {
		w.f32(p.y);
		put_polygon(w, p.polygon);
	}
	return w.take();
}

Result<LevelCombatTiles> decode_combat_tiles(std::span<const uint8_t> p_bytes) {
	BlobReader r(p_bytes);
	if (r.magic(kMagic) != kCombatTilesFormatVersion && r.ok()) {
		r.fail("format version");
	}
	LevelCombatTiles out;
	out.modes.resize(r.count(4, kMaxModes));
	for (std::string &mode : out.modes) {
		mode = r.str(kMaxName);
	}
	out.areas.resize(r.count(24, kMaxModes));
	for (ModeAreas &a : out.areas) {
		a.combat_areas.resize(r.count(24, kMaxCombatAreas));
		for (CombatAreaDef &c : a.combat_areas) {
			c.trigger = r.u32();
			c.shape = r.u32();
			c.team = r.i32();
			get_polygon(r, c.polygon);
			c.height = r.f32();
		}
		a.objectives.resize(r.count(25, kMaxCombatAreaPoints));
		for (ModeObjectiveDef &o : a.objectives) {
			const uint8_t kind = r.u8();
			if (kind > uint8_t(ModeObjectiveDef::Kind::Mcom)) {
				r.fail("objective kind " + std::to_string(kind));
			}
			o.kind = ModeObjectiveDef::Kind(kind);
			o.object = r.u32();
			o.position.x = r.f32();
			o.position.y = r.f32();
			o.position.z = r.f32();
			o.team = r.i32();
			o.scope_label = r.str(kMaxName);
		}
		r.array(r.count(sizeof(uint32_t), kMaxCombatAreaPoints), a.unwired_triggers);
	}
	r.array(r.count(sizeof(std::array<int32_t, 2>), kMaxTiles), out.tiles);
	out.polygon_tiles = size_t(r.u64());
	out.point_only_tiles = size_t(r.u64());
	out.points = size_t(r.u64());
	out.points_outside_areas = size_t(r.u64());
	out.water.resize(r.count(12, kMaxCombatAreas));
	for (WaterPlane &p : out.water) {
		p.y = r.f32();
		get_polygon(r, p.polygon);
	}
	if (r.ok() && !r.at_end()) {
		r.fail(std::to_string(r.remaining()) + " trailing bytes");
	}
	if (!r.ok()) {
		return r.error("combat tiles blob");
	}
	return out;
}

Result<CombatTilesLoad> load_level_combat_tiles(
		Cache *p_cache, std::shared_ptr<const Install> p_install, const LoadedLevel &p_level, float p_margin) {
	const auto start = std::chrono::steady_clock::now();
	CombatTilesLoad out;
	std::optional<CacheKey> key;
	if (p_cache != nullptr) {
		Result<SourceDigest> digest = level_mount_digest(p_level);
		if (!digest) {
			return error_at("combat tiles " + p_level.info.id, digest.error());
		}
		if (digest->digest) {
			CacheKeyInputs in;
			in.kind = kCombatTilesCacheKind;
			in.converter_version = 1;
			in.engine = std::string(cache_engine_id());
			in.format_version = kCombatTilesFormatVersion;
			in.params = "mount=" + digest->digest->hex() + ";level=" + to_lower_ascii(p_level.info.id) +
					";margin=" + margin_text(p_margin);
			key = make_cache_key(in);
		}
		out.seconds_key = seconds_since(start);
	}
	if (!key) {
		Result<LevelCombatTiles> tiles = level_combat_tiles(p_install, p_level.info.id, p_margin);
		if (!tiles) {
			return tiles.error();
		}
		out.tiles = std::move(*tiles);
		out.seconds = seconds_since(start);
		return out;
	}
	Result<CachedItem> item = cached_convert(
			p_cache, *key,
			[&](std::span<const uint8_t> p_bytes) -> Status {
				Result<LevelCombatTiles> decoded = decode_combat_tiles(p_bytes);
				if (!decoded) {
					return decoded.error();
				}
				out.tiles = std::move(*decoded);
				return {};
			},
			[&]() -> Result<std::vector<uint8_t>> {
				Result<LevelCombatTiles> tiles = level_combat_tiles(p_install, p_level.info.id, p_margin);
				if (!tiles) {
					return tiles.error();
				}
				out.tiles = std::move(*tiles);
				return encode_combat_tiles(out.tiles);
			});
	if (!item) {
		return item.error();
	}
	out.cache_hit = item->outcome == CacheOutcome::Hit;
	out.cached = item->outcome != CacheOutcome::Uncached;
	out.seconds = seconds_since(start);
	return out;
}

}
