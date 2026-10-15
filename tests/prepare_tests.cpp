// Converters through the derived-data cache (bf3_core cache/cached_convert.h, gameplay/weapon_blob.h,
// bf3_render mesh_cache.h) and `bf3dump prepare` (bf3_render level_prepare.h).
//
// Suite "prepare-unit": synthetic blobs and items, scratch cache roots in %TEMP%; no data.
// Suite "prepare": the M9 viewmodel meshes and weapons from the install (read-only) into a scratch cache root;
// exits 77 (skipped) without an install. Asserts numbers only.
#include <doctest/doctest.h>

#include "bf3/core/cache/blob_io.h"
#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/cache/entry_format.h"
#include "bf3/core/gameplay/weapon_blob.h"
#include "bf3/core/gameplay/weapon_format.h"
#include "bf3/core/gameplay/weapon_resolver.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/thread_pool.h"
#include "bf3/render/mesh_cache.h"
#include "bf3/render/viewmodel_assets.h"
#include "test_support.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace bf3::core;
using namespace bf3::render;

namespace {

std::atomic<int> g_counter{ 0 };

// %TEMP%\openbf3-prepare-<pid>-<n>\{protected, cache}; the protected folder plays the install for the guard.
struct ScratchRoot {
	fs::path base;
	fs::path root;
	fs::path protected_dir;
	ScratchRoot() {
		wchar_t tmp[MAX_PATH];
		GetTempPathW(MAX_PATH, tmp);
		base = fs::path(tmp) / (L"openbf3-prepare-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" +
									   std::to_wstring(g_counter++));
		root = base / L"cache";
		protected_dir = base / L"install";
		fs::create_directories(protected_dir);
	}
	~ScratchRoot() {
		std::error_code ec;
		fs::remove_all(base, ec);
	}
	CacheOptions options(const PathGuard *p_guard = nullptr) const {
		CacheOptions o;
		o.root = root;
		if (p_guard != nullptr) {
			o.guard = *p_guard;
		}
		o.guard.add_protected_root(protected_dir);
		return o;
	}
	fs::path entry_path(const CacheKey &p_key) const { return root / key_path(p_key); }
	static fs::path key_path(const CacheKey &p_key) { return p_key.relative_path(); }
};

std::vector<uint8_t> read_all(const fs::path &p_path) {
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

bool write_all(const fs::path &p_path, const std::vector<uint8_t> &p_bytes) {
	FILE *f = nullptr;
	if (_wfopen_s(&f, p_path.c_str(), L"wb") != 0 || f == nullptr) {
		return false;
	}
	const bool ok = std::fwrite(p_bytes.data(), 1, p_bytes.size(), f) == p_bytes.size();
	std::fclose(f);
	return ok;
}

// Flips one payload byte of an entry file in place (the cache must be closed).
bool damage_entry(const fs::path &p_file, size_t p_from_end = 1) {
	std::vector<uint8_t> bytes = read_all(p_file);
	if (bytes.size() <= kCacheEntryHeaderSize + p_from_end) {
		return false;
	}
	bytes[bytes.size() - p_from_end] ^= 0x5A;
	return write_all(p_file, bytes);
}

uint32_t next(uint32_t &r_x) {
	r_x ^= r_x << 13;
	r_x ^= r_x >> 17;
	r_x ^= r_x << 5;
	return r_x;
}

float unit(uint32_t &r_x) {
	return float(next(r_x) % 20001) / 10000.0f - 1.0f;
}

PreparedSurface synthetic_surface(uint32_t p_seed, size_t p_vertices, size_t p_triangles, bool p_skinned, bool p_parts) {
	uint32_t x = p_seed * 2654435761u + 7;
	PreparedSurface s;
	s.subset = p_seed % 5;
	s.material_index = p_seed % 3;
	s.material_name = "material_" + std::to_string(p_seed);
	s.category = res::SubsetCategory(p_seed % 3);
	for (size_t i = 0; i < p_vertices; ++i) {
		for (int a = 0; a < 3; ++a) {
			s.positions.push_back(unit(x) * 10.0f);
			s.normals.push_back(unit(x));
		}
		for (int a = 0; a < 4; ++a) {
			s.tangents.push_back(unit(x));
			s.colors.push_back(uint8_t(next(x)));
		}
		s.uv0.push_back(unit(x));
		s.uv0.push_back(unit(x));
		if (p_skinned) {
			for (int a = 0; a < 4; ++a) {
				s.bones.push_back(int32_t(next(x) % 74));
				s.weights.push_back(a == 0 ? 1.0f : 0.0f);
			}
		}
		if (p_parts) {
			s.parts.push_back(uint16_t(next(x) % 12));
		}
	}
	s.positions[0] = std::numeric_limits<float>::quiet_NaN(); // stored bit for bit
	s.normals[1] = -0.0f;
	for (size_t t = 0; t < p_triangles * 3; ++t) {
		s.indices.push_back(int32_t(next(x) % p_vertices));
	}
	s.aabb_min = { -10, -10, -10 };
	s.aabb_max = { 10, 10, 10 };
	return s;
}

PreparedMeshSet synthetic_mesh(bool p_skinned) {
	PreparedMeshSet m;
	m.name = "synthetic/mesh";
	m.lods = { 0, 1, 3 };
	for (uint32_t l = 0; l < 3; ++l) {
		PreparedMesh lod;
		lod.name = m.name;
		lod.skinned = p_skinned;
		for (uint32_t s = 0; s < 2 + l; ++s) {
			lod.surfaces.push_back(synthetic_surface(l * 10 + s, 30 + s * 7, 20 + s, p_skinned, !p_skinned && s == 1));
			lod.vertices += lod.surfaces.back().vertex_count();
			lod.triangles += lod.surfaces.back().indices.size() / 3;
		}
		m.meshes.push_back(std::move(lod));
	}
	m.meshes[1].surfaces[0].uv1 = m.meshes[1].surfaces[0].uv0; // an optional array on one surface only
	m.meshes[2].surfaces[1].category.reset();
	m.meshes[2].surfaces[1].normals.clear();
	m.meshes[2].surfaces[1].tangents.clear();
	return m;
}

bool same_set(const PreparedMeshSet &p_a, const PreparedMeshSet &p_b) {
	if (p_a.name != p_b.name || p_a.lods != p_b.lods || p_a.meshes.size() != p_b.meshes.size()) {
		return false;
	}
	for (size_t i = 0; i < p_a.meshes.size(); ++i) {
		if (!same_prepared(p_a.meshes[i], p_b.meshes[i])) {
			return false;
		}
	}
	return true;
}

gameplay::WeaponDef synthetic_weapon() {
	using namespace gameplay;
	WeaponDef d;
	d.variant = "weapons/test/u_test";
	d.unlock_identifier = 0xDEADBEEF;
	d.blueprint = "weapons/test/test";
	d.blueprint_name = "Weapons/Test/Test";
	d.name_sid = "ID_P_WNAME_TEST";
	d.name_sid_hash = loc_sid_hash(d.name_sid);
	d.active_unlocks = { "weapons/test/u_test", "weapons/test/u_test_extra" };
	d.applied_modifiers = { "u_test: WeaponShotModifier" };
	d.weapon_class = "wc556x45mmNATO";
	d.render_fov = 55.0f;
	d.silenced = true;
	d.firing_data = "weapons/test/test#{00000000-0000-0000-0000-000000000000}";
	d.deploy_time = 0.67f;
	d.fire.logic = FireLogic::Burst;
	d.fire.extra_modes = { FireLogic::Single, FireLogic::Automatic };
	d.fire.rate_of_fire = 800;
	d.fire.reload_type = ReloadType::Magazine;
	d.fire.reload_time = 2.37f;
	d.fire.reload_logic = "rlWeaponSwitchCancelEnabled";
	d.fire.magazine_capacity = 30;
	d.fire.initial_speed = { 0, 0, 650 };
	d.fire.weapon_dispersion[2] = Dispersion{ 1, 2, 3, 4 };
	d.fire.firing_dispersions = 3;
	d.projectile.kind = ProjectileKind::Bullet;
	d.projectile.type = "BulletEntityData";
	d.projectile.gravity = -15;
	d.projectile.start_damage = 25;
	d.projectile.end_damage = std::numeric_limits<float>::quiet_NaN();
	d.projectile.material.present = true;
	d.projectile.material.partition.bytes[3] = 9;
	d.projectile.material.physics_property_index = -3;
	d.projectile.explosion.present = true;
	d.projectile.explosion.blast_radius = 4.5f;
	d.projectile.suppression.max_multiplier = 0.8f;
	d.sway.partition = "weapons/test/test_sway";
	d.sway.state[1][0].move[2] = Dispersion{ 0.5f, 3, 0.1f, 9 };
	d.sway.state[2][1].has_move[1] = true;
	d.sway.state[0][1].recoil.up_per_shot = 0.26f;
	d.sway.transitions[4].cool_down = 0.3f;
	d.sway.suppression[1].lag_pitch = 0.5f;
	SwayGatedModifier g;
	g.unlock = "weapons/test/u_test_bipod";
	g.only_in_supported_shooting = true;
	g.mod[2][1].recoil_angle = 0.25f;
	d.sway.applied.push_back(g);
	g.unlock = "weapons/test/u_test_laser";
	d.sway.conditional.push_back(g);
	d.sway.modifiers_total = 7;
	d.sway.modifiers_unresolved = 1;
	return d;
}

struct Item {
	CacheKey key;
	std::vector<uint8_t> blob;
};

std::vector<Item> synthetic_items(size_t p_count) {
	std::vector<Item> out;
	for (size_t i = 0; i < p_count; ++i) {
		CacheKeyInputs in;
		in.kind = "synthetic";
		in.sources.push_back(sha1_of(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&i), sizeof i)));
		in.converter_version = 1;
		in.engine = std::string(cache_engine_id());
		in.format_version = 1;
		Item item{ *make_cache_key(in), {} };
		uint32_t x = uint32_t(i) * 7919u + 1;
		item.blob.resize(64 + (i % 7) * 1000);
		for (uint8_t &b : item.blob) {
			b = uint8_t(next(x));
		}
		out.push_back(std::move(item));
	}
	return out;
}

// Runs every item through cached_convert on all cores; the accept step compares the bytes.
void run_items(Cache *p_cache, const std::vector<Item> &p_items, ConvertTotals &r_totals, std::atomic<int> &r_produced) {
	parallel_for(p_items.size(), [&](size_t i) {
		const Item &item = p_items[i];
		Result<CachedItem> r = cached_convert(
				p_cache, item.key,
				[&](std::span<const uint8_t> p_blob) -> Status {
					if (p_blob.size() != item.blob.size() || std::memcmp(p_blob.data(), item.blob.data(), p_blob.size()) != 0) {
						return make_error("synthetic blob differs");
					}
					return Status::success();
				},
				[&]() -> Result<std::vector<uint8_t>> {
					++r_produced;
					return item.blob;
				});
		if (r) {
			r_totals.add(*r);
		} else {
			r_totals.add_failure();
		}
	});
}

} // namespace

TEST_SUITE("prepare-unit") {
	TEST_CASE("prepare: mesh blob round trip keeps every array bit for bit") {
		for (const bool skinned : { false, true }) {
			const PreparedMeshSet m = synthetic_mesh(skinned);
			const std::vector<uint8_t> blob = encode_mesh_blob(m);
			Result<PreparedMeshSet> back = decode_mesh_blob(blob);
			REQUIRE(back.ok());
			CHECK(same_set(m, *back));
			CHECK(back->vertices() == m.vertices());
			CHECK(back->triangles() == m.triangles());
			CHECK(encode_mesh_blob(*back) == blob);
			CHECK(std::isnan(back->meshes[0].surfaces[0].positions[0]));
			CHECK(std::signbit(back->meshes[0].surfaces[0].normals[1]));
			CHECK(back->meshes[1].surfaces[0].uv1.size() == back->meshes[1].surfaces[0].vertex_count() * 2);
			CHECK(!back->meshes[2].surfaces[1].category.has_value());
			CHECK(back->meshes[2].surfaces[1].tangents.empty());
			CHECK((!skinned) == !back->meshes[0].surfaces[1].parts.empty());
		}
		// One changed float is noticed by the comparison the tests rely on.
		PreparedMeshSet a = synthetic_mesh(true), b = synthetic_mesh(true);
		b.meshes[2].surfaces[2].weights[5] += 1e-6f;
		CHECK(!same_set(a, b));
	}

	TEST_CASE("prepare: truncated and hostile mesh blobs are rejected") {
		const std::vector<uint8_t> blob = encode_mesh_blob(synthetic_mesh(true));
		size_t refused = 0;
		for (size_t n = 0; n < blob.size(); ++n) {
			refused += decode_mesh_blob(std::span<const uint8_t>(blob.data(), n)).ok() ? 0 : 1;
		}
		CHECK(refused == blob.size()); // every proper prefix
		std::vector<uint8_t> longer = blob;
		longer.push_back(0);
		CHECK(!decode_mesh_blob(longer).ok());
		// Every single-byte flip of a small blob: an error or a valid decode, never a crash or a huge allocation.
		PreparedMeshSet small;
		small.name = "s";
		small.lods = { 0 };
		PreparedMesh lod;
		lod.skinned = true;
		lod.surfaces.push_back(synthetic_surface(1, 4, 2, true, false));
		small.meshes.push_back(lod);
		const std::vector<uint8_t> sb = encode_mesh_blob(small);
		size_t accepted = 0;
		for (size_t i = 0; i < sb.size(); ++i) {
			for (const uint8_t mask : { uint8_t(0x01), uint8_t(0x80), uint8_t(0xFF) }) {
				std::vector<uint8_t> m = sb;
				m[i] ^= mask;
				accepted += decode_mesh_blob(m).ok() ? 1 : 0;
			}
		}
		MESSAGE("byte flips of a ", sb.size(), "-byte blob: ", accepted, " of ", sb.size() * 3, " still decode (float bytes)");
		CHECK(accepted < sb.size() * 3);

		// Field-level attacks on the small blob: offsets from the layout in mesh_cache.h.
		// magic 8 | version 4 | name (4 + 1) | skinned 1 | lod count 8 | lod index 4 | surface count 8 | subset 8 |
		// material 4 | material name (4 + 10) | category 1 | vertex count 4 | index count 4 | mask 4 | ...
		const size_t at_lods = 8 + 4 + 5 + 1;
		const size_t at_surfaces = at_lods + 8 + 4;
		const size_t at_vertices = at_surfaces + 8 + 8 + 4 + 4 + small.meshes[0].surfaces[0].material_name.size() + 1;
		auto with = [&](size_t p_at, const void *p_value, size_t p_size) {
			std::vector<uint8_t> m = sb;
			std::memcpy(m.data() + p_at, p_value, p_size);
			return decode_mesh_blob(m);
		};
		const uint64_t huge = uint64_t(1) << 60;
		const uint32_t huge32 = 0xFFFFFFFFu;
		const uint32_t index_count_not_triple = 4;
		const uint32_t unknown_bit = 1u << 20;
		const uint32_t version2 = 2;
		CHECK(!with(at_lods, &huge, 8).ok());
		CHECK(!with(at_surfaces, &huge, 8).ok());
		CHECK(!with(at_vertices, &huge32, 4).ok()); // 4 G vertices: refused before any allocation
		CHECK(!with(at_vertices + 4, &index_count_not_triple, 4).ok());
		CHECK(!with(at_vertices + 8, &unknown_bit, 4).ok());
		CHECK(!with(8, &version2, 4).ok());
		CHECK(!with(0, "OBF3WDEF", 8).ok());
		// An index past the vertices.
		PreparedMeshSet bad = small;
		bad.meshes[0].surfaces[0].indices[2] = 4;
		CHECK(!decode_mesh_blob(encode_mesh_blob(bad)).ok());
		bad = small;
		bad.meshes[0].surfaces[0].bones[3] = -1;
		CHECK(!decode_mesh_blob(encode_mesh_blob(bad)).ok());
	}

	TEST_CASE("prepare: WeaponDef blob round trip, truncation and hostile counts") {
		const gameplay::WeaponDef d = synthetic_weapon();
		const std::vector<uint8_t> blob = gameplay::encode_weapon_def(d);
		Result<gameplay::WeaponDef> back = gameplay::decode_weapon_def(blob);
		REQUIRE(back.ok());
		CHECK(gameplay::encode_weapon_def(*back) == blob);
		CHECK(gameplay::format_weapon(*back, nullptr, nullptr) == gameplay::format_weapon(d, nullptr, nullptr));
		CHECK(back->sway.applied.size() == 1);
		CHECK(back->sway.conditional[0].unlock == "weapons/test/u_test_laser");
		CHECK(back->fire.extra_modes.size() == 2);
		CHECK(back->projectile.material.physics_property_index == -3);
		CHECK(std::isnan(back->projectile.end_damage));
		size_t refused = 0;
		for (size_t n = 0; n < blob.size(); ++n) {
			refused += gameplay::decode_weapon_def(std::span<const uint8_t>(blob.data(), n)).ok() ? 0 : 1;
		}
		CHECK(refused == blob.size());
		std::vector<uint8_t> longer = blob;
		longer.push_back(1);
		CHECK(!gameplay::decode_weapon_def(longer).ok());
		// The active_unlocks count follows the 7 identity fields: make it 2^40.
		const gameplay::WeaponDef empty;
		const std::vector<uint8_t> e = gameplay::encode_weapon_def(empty);
		const size_t at_unlocks = 8 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4; // magic, version, variant, id, blueprint, name, giver, sid, sid hash
		std::vector<uint8_t> m = e;
		const uint64_t huge = uint64_t(1) << 40;
		std::memcpy(m.data() + at_unlocks, &huge, 8);
		CHECK(!gameplay::decode_weapon_def(m).ok());
		m = e;
		m[28 + 2] = 0xFF; // damage_giver_name length 0xFF0000: over the 64 KiB string limit
		CHECK(!gameplay::decode_weapon_def(m).ok());
		// A bool byte that is not 0/1.
		size_t flips_ok = 0;
		for (size_t i = 12; i < blob.size(); ++i) {
			std::vector<uint8_t> f = blob;
			f[i] ^= 0xFF;
			flips_ok += gameplay::decode_weapon_def(f).ok() ? 1 : 0;
		}
		MESSAGE("WeaponDef blob byte flips: ", flips_ok, " of ", blob.size() - 12, " still decode");
		// Dependency lists must be sorted and unique.
		std::vector<gameplay::WeaponDependency> deps{ { 0, "b" }, { 0, "a" } };
		CHECK(!gameplay::decode_weapon_deps(gameplay::encode_weapon_deps(deps)).ok());
		deps = { { 0, "a" }, { 0, "b" }, { 1, "a" } };
		Result<std::vector<gameplay::WeaponDependency>> dd = gameplay::decode_weapon_deps(gameplay::encode_weapon_deps(deps));
		REQUIRE(dd.ok());
		CHECK(*dd == deps);
	}

	TEST_CASE("prepare: keys change with the converter version, a source SHA1 and the request") {
		std::vector<Sha1Digest> sources{ sha1_of(std::vector<uint8_t>{ 1, 2, 3 }), sha1_of(std::vector<uint8_t>{ 4, 5 }) };
		MeshRequest request;
		request.lods = { 0, 1 };
		const CacheKey base = *make_cache_key(mesh_key_inputs(sources, request));
		CHECK(base.kind == "mesh");
		CHECK(*make_cache_key(mesh_key_inputs(sources, request)) == base); // deterministic
		CHECK(!(*make_cache_key(mesh_key_inputs(sources, request, kMeshConverterVersion + 1)) == base));
		std::vector<Sha1Digest> patched = sources;
		patched[1].bytes[7] ^= 1; // a patched chunk
		CHECK(!(*make_cache_key(mesh_key_inputs(patched, request)) == base));
		std::vector<Sha1Digest> swapped{ sources[1], sources[0] };
		CHECK(!(*make_cache_key(mesh_key_inputs(swapped, request)) == base));
		MeshRequest one = request;
		one.lods = { 0 };
		CHECK(!(*make_cache_key(mesh_key_inputs(sources, one)) == base));
		MeshRequest all = request;
		all.filter = res::SubsetFilter::All;
		CHECK(!(*make_cache_key(mesh_key_inputs(sources, all)) == base));
		const std::vector<std::vector<int32_t>> binds{ { 0, 1, 2 } }, binds2{ { 0, 1, 3 } };
		MeshRequest skinned = one, skinned2 = one;
		skinned.lane_binds = &binds;
		skinned2.lane_binds = &binds2;
		const CacheKey ks = *make_cache_key(mesh_key_inputs(sources, skinned));
		CHECK(!(ks == *make_cache_key(mesh_key_inputs(sources, one))));
		CHECK(!(ks == *make_cache_key(mesh_key_inputs(sources, skinned2))));
		CHECK(mesh_key_inputs(sources, request).engine == std::string(cache_engine_id()));
		CHECK(cache_engine_id().find("4.8.") == 0);

		// WeaponDef keys over a partition -> SHA1 table.
		std::map<std::string, Sha1> table;
		for (const char *n : { "weapons/test/u_test", "weapons/test/test", "ui/uiweaponmetadata" }) {
			table[n] = Sha1::from_bytes(sha1_of(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(n), std::strlen(n))).bytes.data());
		}
		gameplay::WeaponCacheSources src;
		EbxDb dummy;
		src.level = &dummy;
		src.level_digest = [&](std::string_view p_name) -> std::optional<Sha1> {
			const auto it = table.find(std::string(p_name));
			return it == table.end() ? std::nullopt : std::optional<Sha1>(it->second);
		};
		src.ui_digest = src.level_digest;
		const std::vector<gameplay::WeaponDependency> deps{ { 0, "weapons/test/test" }, { 0, "weapons/test/u_test" }, { 0, "weapons/test/u_absent" },
			{ 1, "ui/uiweaponmetadata" } };
		const CacheKey wk = *gameplay::weapon_def_key(src, "weapons/test/u_test", deps);
		CHECK(wk.kind == "weapon");
		table["weapons/test/test"].bytes[0] ^= 1; // a patched blueprint
		const CacheKey wk2 = *gameplay::weapon_def_key(src, "weapons/test/u_test", deps);
		CHECK(!(wk2 == wk));
		table["weapons/test/u_absent"].bytes[19] = 1; // a partition looked up as absent appears
		CHECK(!(*gameplay::weapon_def_key(src, "weapons/test/u_test", deps) == wk2));
		CHECK(gameplay::weapon_deps_key(src, "weapons/test/u_test").has_value());
		CHECK(!gameplay::weapon_deps_key(src, "weapons/test/u_none").has_value()); // no SHA1: no key
	}

	TEST_CASE("prepare: twice gives all hits; damaged and undecodable entries are dropped and reconverted") {
		ScratchRoot t;
		const std::vector<Item> items = synthetic_items(48);
		{
			Cache cache;
			REQUIRE(cache.open(t.options()) == CacheStatus::Ok);
			ConvertTotals totals;
			std::atomic<int> produced{ 0 };
			run_items(&cache, items, totals, produced);
			CHECK(totals.converted == 48);
			CHECK(totals.hits == 0);
			CHECK(produced == 48);
			CHECK(cache.stats().puts == 48);
		}
		{
			Cache cache;
			REQUIRE(cache.open(t.options()) == CacheStatus::Ok);
			ConvertTotals totals;
			std::atomic<int> produced{ 0 };
			run_items(&cache, items, totals, produced);
			CHECK(totals.hits == 48);
			CHECK(totals.converted + totals.reused + totals.uncached == 0);
			CHECK(produced == 0);
			CHECK(cache.stats().puts == 0);
		}
		// A flipped payload byte on disk: the payload SHA-256 catches it on first use.
		REQUIRE(damage_entry(t.entry_path(items[5].key)));
		// A blob the cache accepts (valid header and SHA-256) but the converter's decoder refuses.
		{
			Cache cache;
			REQUIRE(cache.open(t.options()) == CacheStatus::Ok);
			REQUIRE(cache.remove(items[9].key) == CacheStatus::Ok);
			const std::vector<uint8_t> junk(100, 0xAB);
			REQUIRE(cache.put(items[9].key, junk).written);
		}
		{
			Cache cache;
			REQUIRE(cache.open(t.options()) == CacheStatus::Ok);
			ConvertTotals totals;
			std::atomic<int> produced{ 0 };
			run_items(&cache, items, totals, produced);
			CHECK(totals.hits == 46);
			CHECK(totals.converted == 2);
			CHECK(totals.dropped_corrupt == 2);
			CHECK(produced == 2);
			CHECK(cache.stats().corrupt_dropped == 1);
		}
		{
			Cache cache;
			REQUIRE(cache.open(t.options()) == CacheStatus::Ok);
			ConvertTotals totals;
			std::atomic<int> produced{ 0 };
			run_items(&cache, items, totals, produced);
			CHECK(totals.hits == 48);
			CHECK(produced == 0);
		}
		// No cache: every item converts, nothing is stored.
		ConvertTotals totals;
		std::atomic<int> produced{ 0 };
		run_items(nullptr, items, totals, produced);
		CHECK(totals.uncached == 48);
		CHECK(produced == 48);
	}

	TEST_CASE("prepare: a cache root under the protected root is refused") {
		ScratchRoot t;
		CacheOptions o = t.options();
		o.root = t.protected_dir / L"bf3cache";
		Cache cache;
		CHECK(cache.open(o) == CacheStatus::Refused);
		// The converters then run uncached.
		const std::vector<Item> items = synthetic_items(3);
		ConvertTotals totals;
		std::atomic<int> produced{ 0 };
		run_items(&cache, items, totals, produced);
		CHECK(totals.uncached == 3);
		CHECK(!fs::exists(o.root));
	}
}

TEST_SUITE("prepare") {
	TEST_CASE("prepare: the M9 viewmodel meshes and weapons into a scratch cache root") {
		InstallReader reader;
		if (!reader.locate({})) {
			bf3::test::skip_install_case(reader.describe().c_str());
			return;
		}
		const InstallSnapshot before = reader.snapshot();
		Result<std::shared_ptr<Install>> install = Install::open(reader);
		REQUIRE(install.ok());
		Result<LevelMount> mount = (*install)->mount_superbundle("Win32/Levels/MP_Subway/MP_Subway");
		REQUIRE(mount.ok());
		REQUIRE(mount->load_all_bundles().ok());
		ScratchRoot t;
		const PathGuard guard = reader.write_guard();

		struct Case {
			const char *name;
			std::vector<uint32_t> lods;
		};
		const std::vector<Case> cases{ { "characters/arms/arms1p_bareglove03/arms1p_bareglove03_mesh", { 0 } }, { "weapons/m9/m9_1p_mesh", { 0 } },
			{ "weapons/m9/m9_3p_mesh", { 0, 1, 2, 3, 4 } } };
		std::vector<PreparedMeshSet> fresh;
		std::vector<CacheKey> keys;
		for (const Case &c : cases) {
			Result<MeshHead> head = read_mesh_head(*mount, c.name);
			REQUIRE(head.ok());
			MeshRequest request;
			request.lods = c.lods;
			Result<res::LoadedMeshSet> loaded = res::load_mesh_set(*mount, c.name);
			REQUIRE(loaded.ok());
			Result<PreparedMeshSet> p = prepare_mesh_set(*loaded, request);
			REQUIRE(p.ok());
			fresh.push_back(std::move(*p));
			const std::optional<CacheKey> key = mesh_cache_key(*mount, *head, request);
			REQUIRE(key.has_value());
			keys.push_back(*key);
			// Only some of the chunks: the key covers the res and exactly the chunks of the requested LODs.
			Result<res::LoadedMeshSet> partial = load_mesh_lods(*mount, *head, request.lods);
			REQUIRE(partial.ok());
			CHECK(partial->lod_chunk.size() == size_t(c.lods.back()) + 1);
		}
		CHECK(fresh[1].meshes[0].vertices == 4606);
		CHECK(fresh[2].meshes.size() == 5);
		auto run = [&](CacheOutcome p_expected, bool p_expect_dropped_at_1) {
			Cache cache;
			REQUIRE(cache.open(t.options(&guard)) == CacheStatus::Ok);
			for (size_t i = 0; i < cases.size(); ++i) {
				MeshRequest request;
				request.lods = cases[i].lods;
				Result<CachedMesh> m = load_mesh_cached(&cache, *mount, cases[i].name, request);
				REQUIRE(m.ok());
				CHECK(m->key_known);
				const CacheOutcome expected = (p_expect_dropped_at_1 && i == 1) ? CacheOutcome::Converted : p_expected;
				CHECK(m->item.outcome == expected);
				CHECK(m->item.dropped_corrupt == (p_expect_dropped_at_1 && i == 1));
				CHECK(same_set(m->mesh, fresh[i]));
			}
		};
		run(CacheOutcome::Converted, false);
		run(CacheOutcome::Hit, false);
		REQUIRE(damage_entry(t.entry_path(keys[1]), 4000));
		run(CacheOutcome::Hit, true);
		run(CacheOutcome::Hit, false);

		// The skinned viewmodel meshes through load_viewmodel: cold, warm, and equal to the uncached load.
		ViewmodelSpec spec;
		Result<std::unique_ptr<ViewmodelAssets>> plain = load_viewmodel(reader, spec, nullptr, nullptr);
		REQUIRE(plain.ok());
		for (const CacheOutcome expected : { CacheOutcome::Converted, CacheOutcome::Hit }) {
			Cache cache;
			REQUIRE(cache.open(t.options(&guard)) == CacheStatus::Ok);
			Result<std::unique_ptr<ViewmodelAssets>> a = load_viewmodel(reader, spec, nullptr, &cache);
			REQUIRE(a.ok());
			for (auto [cached, base] : { std::pair{ &(*a)->arms, &(*plain)->arms }, std::pair{ &(*a)->weapon, &(*plain)->weapon } }) {
				CHECK(cached->cache.outcome == expected);
				CHECK(same_prepared(cached->prepared, base->prepared));
			}
			CHECK((*a)->binds.size() == 74);
			MESSAGE("viewmodel meshes ", std::string(cache_outcome_name(expected)), ": arms ", (*a)->arms.seconds_mesh * 1000.0, " ms, M9 ", (*a)->weapon.seconds_mesh * 1000.0,
					" ms (uncached: ", (*plain)->arms.seconds_mesh * 1000.0, " / ", (*plain)->weapon.seconds_mesh * 1000.0, " ms)");
		}

		// Weapons: every MP_Subway variant round-trips its blob; the cached resolver gives the same block cold and warm.
		Result<std::unique_ptr<EbxLibrary>> level = EbxLibrary::open(*install, { "Win32/Levels/MP_Subway/MP_Subway" });
		REQUIRE(level.ok());
		(*level)->load_all();
		Result<std::unique_ptr<EbxLibrary>> ui = EbxLibrary::open(*install, { "Win32/UI" });
		REQUIRE(ui.ok());
		gameplay::WeaponCacheSources sources;
		sources.level = level->get();
		sources.level_digest = gameplay::library_partition_digest(**level);
		sources.ui = ui->get();
		sources.ui_digest = gameplay::library_partition_digest(**ui);
		const std::vector<std::string> variants = gameplay::list_weapon_variants(**level);
		CHECK(variants.size() == 172);
		size_t identical = 0;
		for (const std::string &v : variants) {
			gameplay::WeaponResolveOptions options;
			options.ui = ui->get();
			Result<gameplay::WeaponDef> d = gameplay::resolve_weapon(**level, v, options);
			REQUIRE(d.ok());
			Result<gameplay::WeaponDef> back = gameplay::decode_weapon_def(gameplay::encode_weapon_def(*d));
			REQUIRE(back.ok());
			identical += (gameplay::format_weapon(*back, nullptr, nullptr) == gameplay::format_weapon(*d, nullptr, nullptr) &&
								 gameplay::encode_weapon_def(*back) == gameplay::encode_weapon_def(*d))
					? 1
					: 0;
		}
		CHECK(identical == variants.size());
		for (const char *v : { "weapons/m9/u_m9", "weapons/m16a4/u_m16_burst" }) {
			gameplay::WeaponResolveOptions options;
			options.ui = ui->get();
			Result<gameplay::WeaponDef> direct = gameplay::resolve_weapon(**level, v, options);
			REQUIRE(direct.ok());
			for (const CacheOutcome expected : { CacheOutcome::Converted, CacheOutcome::Hit }) {
				Cache cache;
				REQUIRE(cache.open(t.options(&guard)) == CacheStatus::Ok);
				Result<gameplay::CachedWeapon> w = gameplay::resolve_weapon_cached(&cache, sources, v);
				REQUIRE(w.ok());
				CHECK(w->item.outcome == expected);
				CHECK(w->dependencies > 5);
				CHECK(gameplay::format_weapon(w->def, nullptr, nullptr) == gameplay::format_weapon(*direct, nullptr, nullptr));
				CHECK(w->def.name_sid == direct->name_sid);
			}
		}
		CHECK(diff_snapshots(before, reader.snapshot()).clean());
	}
}
