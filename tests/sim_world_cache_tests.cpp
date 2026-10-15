// The static world's cache path (bf3_sim static_world.h blobs and keys, static_world_internal.h tile loop;
// docs/design/m1.md §5.5, §14). Suite sim-world-unit, synthetic only (no install): tile and world-index blobs
// round-trip bit for bit and refuse hostile bytes (every truncation, trailing bytes, counts, tags, layers, the stream
// header and seeded tree mutations, which are refused or restore into a world whose queries stay in bounds); the
// cache keys change with every input that changes the output; the content-keyed tile cache on scratch roots (cold
// cooks and puts, warm restores identical shapes, damaged and decoder-refused entries are dropped and re-cooked).
#include <doctest/doctest.h>

#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/cache/entry_format.h"
#include "sim_world_support.h"
#include "test_support.h"

#include "static_world_internal.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <vector>

using namespace sim_world_test;

namespace {

namespace fs = std::filesystem;

std::atomic<int> g_scratch_counter{ 0 };

// %TEMP%\openbf3-world-<pid>-<tick>-<n>\{install, cache}; the install folder plays the protected root.
struct WorldScratch {
	fs::path base;
	fs::path root;
	fs::path protected_dir;
	WorldScratch() {
		wchar_t tmp[MAX_PATH];
		GetTempPathW(MAX_PATH, tmp);
		base = fs::path(tmp) / (L"openbf3-world-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" +
									   std::to_wstring(g_scratch_counter++));
		root = base / L"cache";
		protected_dir = base / L"install";
		fs::create_directories(protected_dir);
	}
	~WorldScratch() {
		std::error_code ec;
		fs::remove_all(base, ec);
	}
	core::CacheOptions options() const {
		core::CacheOptions o;
		o.root = root;
		o.guard.add_protected_root(protected_dir);
		return o;
	}
};

std::vector<uint8_t> read_file(const fs::path &p_path) {
	std::vector<uint8_t> out;
	FILE *f = nullptr;
	if (_wfopen_s(&f, p_path.c_str(), L"rb") != 0 || f == nullptr) {
		return out;
	}
	uint8_t buf[65536];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
		out.insert(out.end(), buf, buf + n);
	}
	std::fclose(f);
	return out;
}

bool write_file(const fs::path &p_path, const std::vector<uint8_t> &p_bytes) {
	FILE *f = nullptr;
	if (_wfopen_s(&f, p_path.c_str(), L"wb") != 0 || f == nullptr) {
		return false;
	}
	const bool ok = std::fwrite(p_bytes.data(), 1, p_bytes.size(), f) == p_bytes.size();
	std::fclose(f);
	return ok;
}

std::vector<std::vector<uint8_t>> tile_blobs(const StaticWorldShapes &p_world) {
	std::vector<std::vector<uint8_t>> out;
	for (size_t i = 0; i < p_world.tile_count(); ++i) {
		out.push_back(encode_static_tile_blob(p_world, i));
	}
	return out;
}

core::Result<std::shared_ptr<const StaticWorldShapes>> decode_all(const std::vector<std::vector<uint8_t>> &p_blobs) {
	std::vector<std::span<const uint8_t>> spans;
	for (const std::vector<uint8_t> &b : p_blobs) {
		spans.emplace_back(b);
	}
	return decode_static_tile_blobs(spans);
}

core::Result<std::shared_ptr<const StaticWorldShapes>> decode_one(const std::vector<uint8_t> &p_blob) {
	const std::span<const uint8_t> one(p_blob);
	return decode_static_tile_blobs(std::span<const std::span<const uint8_t>>(&one, 1));
}

// Every query kind over a world (seeded) as comparable rows: a restored world must answer like its source.
std::vector<StaticHit> query_rows(const CollisionWorld &p_world, uint32_t p_seed, float p_half) {
	std::mt19937 rng(p_seed);
	std::uniform_real_distribution<float> pos(-p_half, p_half);
	std::uniform_real_distribution<float> dir(-1.0f, 1.0f);
	std::vector<StaticHit> rows;
	for (int i = 0; i < 400; ++i) {
		const Vec3 origin{ pos(rng), 4.0f + 2.0f * dir(rng), pos(rng) };
		const Vec3 delta{ 20.0f * dir(rng), -8.0f + 2.0f * dir(rng), 20.0f * dir(rng) };
		StaticHit h;
		h.fraction = p_world.ray(origin, delta, query_mask::kAll, &h) ? h.fraction : -1.0f;
		rows.push_back(h);
		StaticHit s;
		s.fraction = p_world.sweep_capsule(origin, 0.35f, 1.8f, delta, query_mask::kMovement, &s) ? s.fraction : -1.0f;
		rows.push_back(s);
		StaticHit o;
		o.fraction = p_world.overlap_capsule(origin, 0.35f, 1.8f, query_mask::kMovement) ? 1.0f : 0.0f;
		o.tile = p_world.overlap_box(origin, { 0.3f, 0.2f, 1.2f }, dir(rng) * 3.0f, query_mask::kMovement) ? 1 : 0;
		rows.push_back(o);
	}
	return rows;
}

bool same_rows(const std::vector<StaticHit> &p_a, const std::vector<StaticHit> &p_b) {
	if (p_a.size() != p_b.size()) {
		return false;
	}
	for (size_t i = 0; i < p_a.size(); ++i) {
		if (!bits_equal(p_a[i], p_b[i])) {
			return false;
		}
	}
	return true;
}

std::vector<StaticTriangle> field_with_walls() {
	std::vector<StaticTriangle> field = bumpy_field(100.0f); // 16 tiles
	add_wall_x(field, 12.0f, -30.0f, 30.0f, -2.0f, 6.0f, tag_of(40, 4, 2), StaticLayer::Character);
	add_wall_x(field, -12.0f, -30.0f, 30.0f, -2.0f, 6.0f, tag_of(41, 6, 1), StaticLayer::Raycast);
	return field;
}

} // namespace

TEST_SUITE("sim-world-unit") {
	TEST_CASE("sim.world: tile blobs round-trip bit for bit and the restored world answers like the cooked one") {
		const std::vector<StaticTriangle> field = field_with_walls();
		const auto cooked = build(field);
		const std::vector<std::vector<uint8_t>> blobs = tile_blobs(*cooked);
		REQUIRE(blobs.size() == cooked->tile_count());
		core::Result<std::shared_ptr<const StaticWorldShapes>> restored = decode_all(blobs);
		REQUIRE_MESSAGE(restored.ok(), (restored.ok() ? std::string() : restored.error().message));
		const StaticWorldShapes &r = **restored;
		REQUIRE(r.tile_count() == cooked->tile_count());
		for (size_t i = 0; i < r.tile_count(); ++i) {
			const StaticTileInfo &a = cooked->tile(i);
			const StaticTileInfo &b = r.tile(i);
			CHECK(a.x == b.x);
			CHECK(a.z == b.z);
			CHECK(a.content == b.content);
			CHECK(a.triangles == b.triangles);
			CHECK(a.tags == b.tags);
			CHECK(a.min == b.min);
			CHECK(a.max == b.max);
			CHECK(encode_static_tile_blob(r, i) == blobs[i]);
			for (uint32_t t = 0; t < a.tags; ++t) {
				CHECK(cooked->tag(i, t) == r.tag(i, t));
			}
		}
		CHECK(r.stats().tiles_hit == r.tile_count());
		CHECK(r.stats().shapes == cooked->stats().shapes);
		CHECK(r.stats().triangles == cooked->stats().triangles);
		CHECK(r.stats().shape_bytes == cooked->stats().shape_bytes);
		CHECK(encode_static_tile_blob(r, r.tile_count()).empty());
		CHECK(same_rows(query_rows(*world_of(cooked), 99, 110.0f), query_rows(*world_of(*restored), 99, 110.0f)));
		// Tiles must come in increasing (x, z) order, once each.
		std::vector<std::vector<uint8_t>> swapped = blobs;
		std::swap(swapped[0], swapped[1]);
		CHECK_FALSE(decode_all(swapped).ok());
		swapped = blobs;
		swapped[1] = swapped[0];
		CHECK_FALSE(decode_all(swapped).ok());
		// A world without the Raycast layer round-trips too (that layer has no shape and counts 0).
		StaticWorldBuildOptions no_ray;
		no_ray.include_raycast = false;
		const auto movement = build(field, no_ray);
		core::Result<std::shared_ptr<const StaticWorldShapes>> movement_back = decode_all(tile_blobs(*movement));
		REQUIRE(movement_back.ok());
		CHECK((*movement_back)->stats().triangles[size_t(StaticLayer::Raycast)] == 0);
		CHECK((*movement_back)->stats().triangles == movement->stats().triangles);
	}

	TEST_CASE("sim.world: hostile tile blobs fail cleanly (truncations, trailing bytes, tags, layers, seeded mutations)") {
		const auto cooked = build(bumpy_field(30.0f)); // four tiles around the origin, 450 triangles each
		const std::vector<uint8_t> blob = encode_static_tile_blob(*cooked, 0);
		REQUIRE(blob.size() > 1000);
		REQUIRE(decode_one(blob).ok());
		// Proper prefixes: every one of the first 256 bytes, then every 7th.
		size_t prefixes = 0;
		size_t refused = 0;
		for (size_t n = 0; n < blob.size(); n += (n < 256 ? 1 : 7)) {
			++prefixes;
			refused += decode_one(std::vector<uint8_t>(blob.begin(), blob.begin() + ptrdiff_t(n))).ok() ? 0 : 1;
		}
		CHECK(refused == prefixes);
		std::vector<uint8_t> trailing = blob;
		trailing.push_back(0);
		CHECK_FALSE(decode_one(trailing).ok());
		std::vector<uint8_t> magic = blob;
		magic[0] ^= 1;
		CHECK_FALSE(decode_one(magic).ok());
		std::vector<uint8_t> version = blob;
		version[8] = 99;
		CHECK_FALSE(decode_one(version).ok());

		// OBF3JTIL v1 header: magic 8, version 4, x 4, z 4, content 32, bounds 24, tag count 4, tags 8 each.
		constexpr size_t kTagCountAt = 8 + 4 + 4 + 4 + 32 + 24;
		uint32_t tags;
		std::memcpy(&tags, blob.data() + kTagCountAt, 4);
		REQUIRE(tags >= 2);
		{
			// One tag fewer (cut out of the table): the tree's user data then names a tag past it.
			std::vector<uint8_t> fewer = blob;
			const uint32_t less = tags - 1;
			std::memcpy(fewer.data() + kTagCountAt, &less, 4);
			fewer.erase(fewer.begin() + ptrdiff_t(kTagCountAt + 4 + size_t(less) * 8), fewer.begin() + ptrdiff_t(kTagCountAt + 4 + size_t(tags) * 8));
			const auto r = decode_one(fewer);
			REQUIRE_FALSE(r.ok());
			CHECK(r.error().message.find("tags") != std::string::npos);
			// A hostile tag count cannot allocate: it is checked against the bytes left.
			std::vector<uint8_t> huge = blob;
			const uint32_t big = 0xFFFFFFFFu;
			std::memcpy(huge.data() + kTagCountAt, &big, 4);
			CHECK_FALSE(decode_one(huge).ok());
		}
		const size_t layers_at = kTagCountAt + 4 + size_t(tags) * 8;
		const size_t record_at = layers_at + 4; // u8 layer, u32 input, u32 jolt triangles, u32 stream bytes
		{
			std::vector<uint8_t> many = blob;
			const uint32_t four = 4;
			std::memcpy(many.data() + layers_at, &four, 4);
			CHECK_FALSE(decode_one(many).ok());
			std::vector<uint8_t> bad_layer = blob;
			bad_layer[record_at] = 3;
			CHECK_FALSE(decode_one(bad_layer).ok());
			std::vector<uint8_t> huge = blob;
			const uint32_t big = 0xFFFFFFF0u; // a stream past the blob: refused before anything is allocated
			std::memcpy(huge.data() + record_at + 9, &big, 4);
			CHECK_FALSE(decode_one(huge).ok());
			std::vector<uint8_t> count = blob;
			uint32_t jolt;
			std::memcpy(&jolt, count.data() + record_at + 5, 4);
			++jolt; // no longer the tree's count
			std::memcpy(count.data() + record_at + 5, &jolt, 4);
			CHECK_FALSE(decode_one(count).ok());
			std::vector<uint8_t> zero = blob;
			std::memset(zero.data() + record_at + 5, 0, 4);
			CHECK_FALSE(decode_one(zero).ok());
		}
		// The MeshShape stream: sub type, shape user data, tree size.
		const size_t stream_at = record_at + 13;
		{
			std::vector<uint8_t> sub = blob;
			sub[stream_at] = 0; // EShapeSubType::Sphere
			CHECK_FALSE(decode_one(sub).ok());
			std::vector<uint8_t> user = blob;
			user[stream_at + 1] = 1;
			CHECK_FALSE(decode_one(user).ok());
			std::vector<uint8_t> tree = blob;
			tree[stream_at + 9] ^= 4;
			CHECK_FALSE(decode_one(tree).ok());
		}
		// The tree's root properties: an offset into the headers, past the buffer, and a padding root.
		const size_t tree_at = stream_at + 13;
		constexpr size_t kRootPropertiesAt = 24; // NodeCodecQuadTreeHalfFloat::Header: root bounds min, max, then properties
		{
			std::vector<uint8_t> root = blob;
			const uint32_t inside_header = 1; // offset 4: inside the headers
			std::memcpy(root.data() + tree_at + kRootPropertiesAt, &inside_header, 4);
			CHECK_FALSE(decode_one(root).ok());
			const uint32_t past = 0x0FFFFFFFu; // the largest node offset
			std::memcpy(root.data() + tree_at + kRootPropertiesAt, &past, 4);
			CHECK_FALSE(decode_one(root).ok());
			const uint32_t padding = 0xF0000000u;
			std::memcpy(root.data() + tree_at + kRootPropertiesAt, &padding, 4);
			CHECK_FALSE(decode_one(root).ok());
		}
		// Seeded mutations of the tree bytes: each one is refused, or restores into a world whose queries stay inside
		// the buffer (the asan preset turns an out-of-bounds read into a failure).
		std::mt19937 rng(20261015);
		size_t accepted = 0;
		size_t rejected = 0;
		for (int i = 0; i < 2000; ++i) {
			std::vector<uint8_t> m = blob;
			const int flips = 1 + int(rng() % 4);
			for (int f = 0; f < flips; ++f) {
				const size_t at = tree_at + rng() % (m.size() - tree_at);
				m[at] ^= uint8_t(1u << (rng() % 8));
			}
			const auto r = decode_one(m);
			if (!r.ok()) {
				++rejected;
				continue;
			}
			++accepted;
			(void)query_rows(*world_of(*r), uint32_t(i), 40.0f);
		}
		MESSAGE("tree mutations: ", rejected, " refused, ", accepted, " restored and queried");
		CHECK(rejected > 0);
		CHECK(accepted + rejected == 2000);
	}

	TEST_CASE("sim.world: the world index round-trips and refuses hostile input") {
		StaticWorldIndex index;
		index.soup_sum = 0x2f7addcdc0b29facull;
		index.soup_triangles = 1114171;
		for (int i = 0; i < 5; ++i) {
			StaticWorldEntry e;
			e.x = i - 2;
			e.z = 3 * i;
			e.content.bytes[0] = uint8_t(i);
			e.triangles = { uint32_t(100 * i), uint32_t(i), 7 };
			e.tags = uint32_t(i + 1);
			index.tiles.push_back(e);
		}
		const std::vector<uint8_t> blob = encode_static_world_index(index);
		const core::Result<StaticWorldIndex> back = decode_static_world_index(blob);
		REQUIRE(back.ok());
		CHECK(encode_static_world_index(*back) == blob);
		CHECK(back->soup_sum == index.soup_sum);
		CHECK(back->tiles.size() == 5);
		size_t refused = 0;
		for (size_t n = 0; n < blob.size(); ++n) {
			refused += decode_static_world_index(std::span<const uint8_t>(blob.data(), n)).ok() ? 0 : 1;
		}
		CHECK(refused == blob.size());
		std::vector<uint8_t> trailing = blob;
		trailing.push_back(0);
		CHECK_FALSE(decode_static_world_index(trailing).ok());
		std::vector<uint8_t> count = blob;
		const uint32_t big = 0x7FFFFFFFu;
		std::memcpy(count.data() + 12, &big, 4);
		CHECK_FALSE(decode_static_world_index(count).ok());
		StaticWorldIndex unsorted = index;
		std::swap(unsorted.tiles[1], unsorted.tiles[2]);
		CHECK_FALSE(decode_static_world_index(encode_static_world_index(unsorted)).ok());
		const core::Result<std::shared_ptr<const StaticWorldShapes>> empty = decode_static_tile_blobs(std::span<const std::span<const uint8_t>>());
		REQUIRE(empty.ok());
		CHECK((*empty)->tile_count() == 0);
	}

	TEST_CASE("sim.world: cache keys change with content, build options, Jolt id, converter and the world inputs") {
		core::Sha256Digest content;
		content.bytes[5] = 9;
		const StaticWorldBuildOptions base;
		const auto key = [](const core::CacheKeyInputs &p_inputs) {
			const std::optional<core::CacheKey> k = core::make_cache_key(p_inputs);
			REQUIRE(k.has_value());
			return *k;
		};
		const core::CacheKeyInputs tile = static_tile_key_inputs(content, base);
		CHECK(tile.kind == kStaticTileCacheKind);
		CHECK(tile.jolt_version == jolt_build_id());
		CHECK(tile.converter_version == kStaticWorldConverterVersion);
		CHECK(tile.format_version == kStaticWorldFormatVersion);
		CHECK(tile.engine == core::cache_engine_id());
		CHECK(tile.sources.empty());
		const core::CacheKey k0 = key(tile);
		CHECK(key(static_tile_key_inputs(content, base)) == k0);
		core::Sha256Digest other = content;
		other.bytes[31] ^= 1;
		CHECK_FALSE(key(static_tile_key_inputs(other, base)) == k0);
		StaticWorldBuildOptions edges = base;
		edges.active_edge_degrees = 6.0f;
		CHECK_FALSE(key(static_tile_key_inputs(content, edges)) == k0);
		StaticWorldBuildOptions no_ray = base;
		no_ray.include_raycast = false;
		CHECK_FALSE(key(static_tile_key_inputs(content, no_ray)) == k0);
		StaticWorldBuildOptions threads = base;
		threads.threads = 3; // lanes do not change the output
		CHECK(key(static_tile_key_inputs(content, threads)) == k0);
		core::CacheKeyInputs jolt = tile;
		jolt.jolt_version = "5.6.1;det=1;dp=0";
		CHECK_FALSE(key(jolt) == k0);
		core::CacheKeyInputs converter = tile;
		++converter.converter_version;
		CHECK_FALSE(key(converter) == k0);
		core::CacheKeyInputs engine = tile;
		engine.engine = "4.8.dev8.0000000";
		CHECK_FALSE(key(engine) == k0);

		const StaticWorldLoadOptions load;
		const core::CacheKeyInputs world = static_world_key_inputs(content, "MP_Subway", "TeamDeathMatch0", load);
		CHECK(world.kind == kStaticWorldCacheKind);
		CHECK(world.params.find("soup=" + std::to_string(level::kCollisionSoupVersion)) != std::string::npos);
		const core::CacheKey w0 = key(world);
		CHECK_FALSE(w0 == k0);
		CHECK(key(static_world_key_inputs(content, "mp_subway", "teamdeathmatch0", load)) == w0); // the ids are case-insensitive
		CHECK_FALSE(key(static_world_key_inputs(other, "MP_Subway", "TeamDeathMatch0", load)) == w0);
		CHECK_FALSE(key(static_world_key_inputs(content, "MP_Subway", "ConquestLarge0", load)) == w0);
		CHECK_FALSE(key(static_world_key_inputs(content, "MP_011", "TeamDeathMatch0", load)) == w0);
		StaticWorldLoadOptions spacing = load;
		spacing.terrain_spacing = 4.0f;
		CHECK_FALSE(key(static_world_key_inputs(content, "MP_Subway", "TeamDeathMatch0", spacing)) == w0);
		StaticWorldLoadOptions world_ray = load;
		world_ray.build.include_raycast = false;
		CHECK_FALSE(key(static_world_key_inputs(content, "MP_Subway", "TeamDeathMatch0", world_ray)) == w0);
		StaticWorldLoadOptions soup_threads = load;
		soup_threads.soup_threads = 2;
		soup_threads.build.threads = 5;
		CHECK(key(static_world_key_inputs(content, "MP_Subway", "TeamDeathMatch0", soup_threads)) == w0);
	}

	TEST_CASE("sim.world: tiles go through the cache by content: cold cooks and puts, warm restores identical shapes") {
		WorldScratch t;
		const std::vector<StaticTriangle> field = field_with_walls();
		const auto uncached = build(field);
		const std::vector<std::vector<uint8_t>> reference = tile_blobs(*uncached);
		const auto run = [&](const std::vector<StaticTriangle> &p_field, size_t p_hits, size_t p_cooked) {
			core::Cache cache;
			REQUIRE(cache.open(t.options()) == core::CacheStatus::Ok);
			std::atomic<size_t> done{ 0 };
			StaticWorldBuildOptions options;
			options.tiles_done = &done;
			core::Result<std::shared_ptr<const StaticWorldShapes>> w = build_static_world_cached(&cache, std::span<const StaticTriangle>(p_field), options);
			REQUIRE_MESSAGE(w.ok(), (w.ok() ? std::string() : w.error().message));
			CHECK((*w)->stats().tiles_hit == p_hits);
			CHECK((*w)->stats().tiles_cooked == p_cooked);
			CHECK((*w)->stats().cache_bytes > 0);
			CHECK(done.load() == (*w)->tile_count());
			cache.close();
			return *w;
		};
		const auto cold = run(field, 0, 16);
		const auto warm = run(field, 16, 0);
		CHECK(tile_blobs(*cold) == reference);
		CHECK(tile_blobs(*warm) == reference);
		CHECK(same_rows(query_rows(*world_of(cold), 5, 110.0f), query_rows(*world_of(warm), 5, 110.0f)));
		CHECK(same_rows(query_rows(*world_of(uncached), 5, 110.0f), query_rows(*world_of(warm), 5, 110.0f)));

		// A damaged entry on disk (the cache's SHA-256 catches it) and an entry that passes the cache but not the
		// decoder are both dropped and re-cooked; the next run is all hits.
		const StaticWorldBuildOptions options;
		const std::optional<core::CacheKey> k0 = core::make_cache_key(static_tile_key_inputs(uncached->tile(0).content, options));
		const std::optional<core::CacheKey> k1 = core::make_cache_key(static_tile_key_inputs(uncached->tile(1).content, options));
		REQUIRE(k0.has_value());
		REQUIRE(k1.has_value());
		const fs::path p0 = t.root / k0->relative_path();
		std::vector<uint8_t> bytes = read_file(p0);
		REQUIRE(bytes.size() > core::kCacheEntryHeaderSize + 100);
		bytes[bytes.size() - 50] ^= 0x5A;
		REQUIRE(write_file(p0, bytes));
		{
			core::Cache cache;
			REQUIRE(cache.open(t.options()) == core::CacheStatus::Ok);
			REQUIRE(cache.remove(*k1) == core::CacheStatus::Ok);
			std::vector<uint8_t> hostile = reference[1];
			hostile.push_back(7); // a trailing byte: valid for the cache, refused by the decoder
			REQUIRE(cache.put(*k1, hostile).status == core::CacheStatus::Ok);
			cache.close();
		}
		const auto repaired = run(field, 14, 2);
		CHECK(tile_blobs(*repaired) == reference);
		run(field, 16, 0);
		// A tile whose content changes gets a new key; the others still hit.
		std::vector<StaticTriangle> moved = field;
		moved[0].v[0][1] += 0.001f;
		run(moved, 15, 1);
		// A closed cache: every tile is cooked and nothing is read or written.
		core::Cache closed;
		core::Result<std::shared_ptr<const StaticWorldShapes>> w = build_static_world_cached(&closed, std::span<const StaticTriangle>(field));
		REQUIRE(w.ok());
		CHECK((*w)->stats().tiles_cooked == 16);
		CHECK((*w)->stats().cache_bytes == 0);
	}
}
