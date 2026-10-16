// The M1 level scene (bf3_render level_scene.h; docs/design/m1.md §11.2-§11.4, §12.2, §14).
//
// Suite "render-scene-unit": scene_mesh_requests against the M0 prepare rule on a synthetic census; the variant step
// (undrawn slots, hidden parts, bake with a mirror) on synthetic meshes; texture role precedence; shader flags; the
// terrain surface on a synthetic heightfield (holes dropped, winding, normals up); hostile arrays. No data.
// Suite "render-scene": Metro TDM from the install (read-only; exits 77 = skipped without one): the plan's counts as
// rules over the census, then SceneLanes cold and warm on a scratch cache root with a counting sink (wall and work
// times per kind, texture count and bytes, peak in-flight bytes), a held-sink run against a small cap, the peak
// working set, and install.unchanged. Prints numbers only (no BF3 content beyond names and counts).
#include <doctest/doctest.h>

#include "level_scene_internal.h"

#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/census.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "bf3/render/level_scene.h"
#include "test_support.h"

#include <windows.h>

#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace bf3::core;
using namespace bf3::render;
namespace si = bf3::render::scene_internal;

namespace {

// The M0 `bf3dump prepare` rule before scene_mesh_requests existed (level_prepare.cpp at 15c12f7): per MeshSet res
// of a kept variant, LODs 0..max(decoded LODs) - 1 when any variant is model kind, else LOD 0.
std::map<std::string, std::vector<uint32_t>> m0_prepare_rule(const level::MeshCensus &p_census) {
	struct Item {
		size_t lods = 0;
		bool model = false;
	};
	std::map<std::string, Item> by_name;
	for (const level::SceneMesh &m : p_census.meshes) {
		if (!m.kept) {
			continue;
		}
		Item &item = by_name[m.name];
		item.lods = std::max(item.lods, m.lods.size());
		item.model = item.model || !m.world_kind;
	}
	std::map<std::string, std::vector<uint32_t>> out;
	for (const auto &[name, item] : by_name) {
		const size_t lods = item.model ? std::max<size_t>(item.lods, 1) : 1;
		for (size_t l = 0; l < lods; ++l) {
			out[name].push_back(uint32_t(l));
		}
	}
	return out;
}

void check_requests_match_m0(const level::MeshCensus &p_census, const std::vector<SceneMeshRequest> &p_requests) {
	const std::map<std::string, std::vector<uint32_t>> expected = m0_prepare_rule(p_census);
	CHECK(p_requests.size() == expected.size());
	std::set<std::string> seen;
	size_t variants = 0;
	for (const SceneMeshRequest &r : p_requests) {
		CHECK(seen.insert(r.res).second);
		const auto found = expected.find(r.res);
		REQUIRE(found != expected.end());
		CHECK(r.request.lods == found->second);
		CHECK(r.request.filter == res::SubsetFilter::Drawn);
		CHECK(r.request.lane_binds == nullptr);
		for (uint32_t v : r.variants) {
			REQUIRE(v < p_census.meshes.size());
			CHECK(p_census.meshes[v].kept);
			CHECK(p_census.meshes[v].name == r.res);
		}
		CHECK(std::is_sorted(r.variants.begin(), r.variants.end()));
		variants += r.variants.size();
	}
	size_t kept = 0;
	for (const level::SceneMesh &m : p_census.meshes) {
		kept += m.kept ? 1 : 0;
	}
	CHECK(variants == kept);
}

level::SceneMesh census_mesh(const char *p_name, bool p_kept, size_t p_lods, bool p_world) {
	level::SceneMesh m;
	m.name = p_name;
	m.kept = p_kept;
	m.lods.assign(p_lods, true);
	m.world_kind = p_world;
	return m;
}

// A quad (two triangles) per part along +X, parts 0..p_parts-1, clockwise for Godot as mesh_prep emits them.
PreparedSurface strip_surface(uint32_t p_material, uint16_t p_parts, bool p_with_parts) {
	PreparedSurface s;
	s.material_index = p_material;
	for (uint16_t p = 0; p < p_parts; ++p) {
		const float x = float(p);
		const float corners[4][3] = { { x, 0, 0 }, { x + 1, 0, 0 }, { x, 0, 1 }, { x + 1, 0, 1 } };
		const int32_t base = int32_t(s.positions.size() / 3);
		for (const auto &c : corners) {
			s.positions.insert(s.positions.end(), { c[0], c[1], c[2] });
			s.normals.insert(s.normals.end(), { 1.0f, 0.0f, 0.0f });
			s.tangents.insert(s.tangents.end(), { 0.0f, 0.0f, 1.0f, 1.0f });
			s.uv0.insert(s.uv0.end(), { c[0], c[2] });
			if (p_with_parts) {
				s.parts.push_back(p);
			}
		}
		s.indices.insert(s.indices.end(), { base, base + 1, base + 2, base + 1, base + 3, base + 2 });
	}
	return s;
}

std::array<float, 3> sub3(const float *p_a, const float *p_b) {
	return { p_a[0] - p_b[0], p_a[1] - p_b[1], p_a[2] - p_b[2] };
}

std::array<float, 3> cross3(const std::array<float, 3> &p_a, const std::array<float, 3> &p_b) {
	return { p_a[1] * p_b[2] - p_a[2] * p_b[1], p_a[2] * p_b[0] - p_a[0] * p_b[2], p_a[0] * p_b[1] - p_a[1] * p_b[0] };
}

double mib(uint64_t p_bytes) {
	return double(p_bytes) / (1024.0 * 1024.0);
}

std::atomic<int> g_root_counter{ 0 };

struct ScratchRoot {
	fs::path root;
	ScratchRoot() {
		wchar_t tmp[MAX_PATH];
		GetTempPathW(MAX_PATH, tmp);
		root = fs::path(tmp) / (L"openbf3-scene-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" +
									   std::to_wstring(g_root_counter++));
	}
	~ScratchRoot() {
		std::error_code ec;
		fs::remove_all(root, ec);
	}
};

// Counts every item and releases its bytes at once.
class CountingSink final : public SceneSink {
public:
	SceneLanes *lanes = nullptr;
	std::mutex mutex;
	std::vector<std::pair<std::string, std::string>> failures;
	std::atomic<uint64_t> meshes{ 0 }, textures{ 0 }, terrains{ 0 }, empty_meshes{ 0 }, largest{ 0 };
	std::vector<uint8_t> mesh_seen;
	std::vector<uint8_t> texture_seen;
	uint64_t terrain_vertices = 0, terrain_triangles = 0, mesh_triangles = 0;
	std::map<std::string, uint64_t> texture_formats;

	void note(uint64_t p_bytes) {
		uint64_t l = largest.load();
		while (p_bytes > l && !largest.compare_exchange_weak(l, p_bytes)) {
		}
	}
	void mesh(PreparedSceneMesh &&p_mesh, uint64_t p_bytes) override {
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (p_mesh.mesh < mesh_seen.size()) {
				++mesh_seen[p_mesh.mesh];
			}
			mesh_triangles += p_mesh.lod0.triangles;
		}
		if (!si::has_triangles(p_mesh.lod0)) {
			++empty_meshes;
		}
		++meshes;
		note(p_bytes);
		lanes->release_bytes(p_bytes);
	}
	void texture(uint32_t p_texture, PreparedTexture &&p_data, uint64_t p_bytes) override {
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (p_texture < texture_seen.size()) {
				++texture_seen[p_texture];
			}
			++texture_formats[gpu_format_name(p_data.format)];
		}
		++textures;
		note(p_bytes);
		lanes->release_bytes(p_bytes);
	}
	void terrain(uint32_t, PreparedMesh &&p_mesh, uint64_t p_bytes) override {
		{
			std::lock_guard<std::mutex> lock(mutex);
			terrain_vertices += p_mesh.vertices;
			terrain_triangles += p_mesh.triangles;
		}
		++terrains;
		note(p_bytes);
		lanes->release_bytes(p_bytes);
	}
	void failure(const std::string &p_item, const std::string &p_error) override {
		std::lock_guard<std::mutex> lock(mutex);
		failures.emplace_back(p_item, p_error);
	}
};

// Holds the items and releases all of them only once the lanes have stalled (no item for 20 ms), so the lanes
// must stop at the cap: a consumer that lags the lanes, like the Godot side's C1 tasks behind a slow upload.
class HeldSink final : public SceneSink {
public:
	SceneLanes *lanes = nullptr;
	std::mutex mutex;
	std::condition_variable wake;
	std::vector<uint64_t> held;
	uint64_t pushes = 0;
	bool stop = false;
	std::atomic<uint64_t> largest{ 0 }, items{ 0 }, stalls{ 0 };
	std::thread consumer;

	void start() {
		consumer = std::thread([this] {
			std::unique_lock<std::mutex> lock(mutex);
			for (;;) {
				const uint64_t seen = pushes;
				wake.wait_for(lock, std::chrono::milliseconds(20), [&] { return stop || pushes != seen; });
				if (pushes != seen && !stop) {
					continue; // still arriving
				}
				std::vector<uint64_t> release;
				release.swap(held);
				lock.unlock();
				if (!release.empty()) {
					++stalls;
				}
				for (uint64_t bytes : release) {
					lanes->release_bytes(bytes);
				}
				lock.lock();
				if (stop && held.empty()) {
					return;
				}
			}
		});
	}
	void finish() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			stop = true;
		}
		wake.notify_all();
		consumer.join();
	}
	void push(uint64_t p_bytes) {
		uint64_t l = largest.load();
		while (p_bytes > l && !largest.compare_exchange_weak(l, p_bytes)) {
		}
		++items;
		{
			std::lock_guard<std::mutex> lock(mutex);
			held.push_back(p_bytes);
			++pushes;
		}
		wake.notify_all();
	}
	void mesh(PreparedSceneMesh &&, uint64_t p_bytes) override { push(p_bytes); }
	void texture(uint32_t, PreparedTexture &&, uint64_t p_bytes) override { push(p_bytes); }
	void terrain(uint32_t, PreparedMesh &&, uint64_t p_bytes) override { push(p_bytes); }
	void failure(const std::string &, const std::string &) override {}
};

uint64_t peak_working_set() {
	PROCESS_MEMORY_COUNTERS pmc{};
	GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
	return pmc.PeakWorkingSetSize;
}

void print_stats(const char *p_label, const SceneLaneStats &p_s) {
	std::printf("render.scene %s: wall %.3f s; work mesh %.3f s (%llu items, %.1f MiB), texture %.3f s (%llu items, %.1f MiB), terrain %.3f s "
				"(%llu items, %.1f MiB); peak in flight %.1f MiB; blobs %llu hits, %llu converted; %llu failures\n",
			p_label, p_s.wall_seconds, p_s.mesh_seconds, (unsigned long long)p_s.mesh_items, mib(p_s.mesh_bytes), p_s.texture_seconds,
			(unsigned long long)p_s.texture_items, mib(p_s.texture_bytes), p_s.terrain_seconds, (unsigned long long)p_s.terrain_items, mib(p_s.terrain_bytes),
			mib(p_s.peak_inflight_bytes), (unsigned long long)p_s.cache_hits, (unsigned long long)p_s.cache_converted, (unsigned long long)p_s.failures);
}

} // namespace

TEST_SUITE("render-scene-unit") {
	TEST_CASE("render.scene.unit: scene_mesh_requests is the M0 prepare rule, in first-placement order") {
		level::MeshCensus census;
		census.meshes.push_back(census_mesh("b/rock", true, 3, false)); // model, 3 LODs
		census.meshes.push_back(census_mesh("a/bridge", true, 4, true)); // world kind only -> LOD 0
		census.meshes.push_back(census_mesh("b/rock", true, 2, false)); // second variant, fewer LODs
		census.meshes.push_back(census_mesh("c/hidden", false, 4, false)); // not kept
		census.meshes.push_back(census_mesh("d/mixed", true, 1, true)); // world variant ...
		census.meshes.push_back(census_mesh("d/mixed", true, 4, false)); // ... and a model one -> 4 LODs
		census.meshes.push_back(census_mesh("e/nolod", true, 0, false)); // model without decoded LODs -> LOD 0
		const std::vector<SceneMeshRequest> r = scene_mesh_requests(census);
		check_requests_match_m0(census, r);
		REQUIRE(r.size() == 4);
		CHECK(r[0].res == "b/rock");
		CHECK(r[0].request.lods == std::vector<uint32_t>{ 0, 1, 2 });
		CHECK(r[0].variants == std::vector<uint32_t>{ 0, 2 });
		CHECK(r[1].res == "a/bridge");
		CHECK(r[1].request.lods == std::vector<uint32_t>{ 0 });
		CHECK(r[2].res == "d/mixed");
		CHECK(r[2].request.lods == std::vector<uint32_t>{ 0, 1, 2, 3 });
		CHECK(r[3].request.lods == std::vector<uint32_t>{ 0 });
		CHECK(scene_mesh_requests(level::MeshCensus{}).empty());
	}

	TEST_CASE("render.scene.unit: undrawn slots, hidden parts and a mirroring bake") {
		PreparedMesh m;
		m.name = "synthetic";
		m.surfaces.push_back(strip_surface(0, 3, true)); // parts 0, 1, 2
		m.surfaces.push_back(strip_surface(1, 1, false)); // undrawn slot
		m.surfaces.push_back(strip_surface(5, 1, false)); // index past the slot list
		const std::vector<uint32_t> slots = { 7, kSceneUndrawnMaterial };
		const si::Mat3 bake = { { { -1.0f, 0.0f, 0.0f }, { 0.0f, 2.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } } }; // det -2
		PreparedMesh plain = m;
		REQUIRE(si::apply_scene_variant(plain, {}, std::nullopt, slots).ok());
		REQUIRE(plain.surfaces.size() == 1);
		CHECK(plain.vertices == 12);
		CHECK(plain.triangles == 6);
		CHECK(plain.surfaces[0].indices == m.surfaces[0].indices);

		REQUIRE(si::apply_scene_variant(m, { 1 }, bake, slots).ok());
		REQUIRE(m.surfaces.size() == 1);
		const PreparedSurface &s = m.surfaces[0];
		CHECK(s.vertex_count() == 8); // part 1's four vertices compacted away
		CHECK(s.indices.size() == 12);
		CHECK(m.vertices == 8);
		CHECK(m.triangles == 4);
		CHECK(s.parts == std::vector<uint16_t>{ 0, 0, 0, 0, 2, 2, 2, 2 });
		CHECK(s.uv0.size() == 16);
		CHECK(s.tangents.size() == 32);
		for (int32_t i : s.indices) {
			CHECK(i >= 0);
			CHECK(i < 8);
		}
		// Positions x -> -x; the normal (1,0,0) maps through L^-T to (-1,0,0); the tangent keeps (0,0,1), sign flips.
		CHECK(s.positions[0] == doctest::Approx(-0.0f));
		CHECK(s.positions[3] == doctest::Approx(-1.0f));
		CHECK(s.positions[4 * 3] == doctest::Approx(-2.0f)); // part 2's first corner (x = 2)
		CHECK(s.normals[0] == doctest::Approx(-1.0f));
		CHECK(s.normals[1] == doctest::Approx(0.0f));
		CHECK(s.tangents[2] == doctest::Approx(1.0f));
		CHECK(s.tangents[3] == doctest::Approx(-1.0f));
		// Winding swapped: (0, 1, 2) -> (0, 2, 1), so the face keeps its Godot front side after the mirror.
		CHECK(s.indices[0] == 0);
		CHECK(s.indices[1] == 2);
		CHECK(s.indices[2] == 1);
		const auto t = cross3(sub3(&s.positions[size_t(s.indices[1]) * 3], &s.positions[size_t(s.indices[0]) * 3]),
				sub3(&s.positions[size_t(s.indices[2]) * 3], &s.positions[size_t(s.indices[0]) * 3]));
		const auto t0 = cross3(sub3(&plain.surfaces[0].positions[3], &plain.surfaces[0].positions[0]),
				sub3(&plain.surfaces[0].positions[6], &plain.surfaces[0].positions[0]));
		CHECK(t[1] * t0[1] > 0.0f); // same side up as before the bake
		CHECK(s.aabb_min[0] == doctest::Approx(-3.0f));
		CHECK(s.aabb_max[0] == doctest::Approx(0.0f));
		CHECK(s.aabb_max[2] == doctest::Approx(1.0f));

		// Hiding every part leaves nothing to draw.
		PreparedMesh all = plain;
		REQUIRE(si::apply_scene_variant(all, { 0, 1, 2 }, std::nullopt, slots).ok());
		CHECK_FALSE(si::has_triangles(all));
		CHECK(all.surfaces.empty());
	}

	TEST_CASE("render.scene.unit: hostile surfaces fail instead of indexing out of range") {
		const std::vector<uint32_t> slots = { 0 };
		PreparedMesh m;
		m.surfaces.push_back(strip_surface(0, 1, true));
		m.surfaces[0].indices[4] = 99;
		CHECK_FALSE(si::apply_scene_variant(m, { 0 }, std::nullopt, slots).ok());
		PreparedMesh n;
		n.surfaces.push_back(strip_surface(0, 1, true));
		n.surfaces[0].parts.pop_back();
		CHECK_FALSE(si::apply_scene_variant(n, { 0 }, std::nullopt, slots).ok());
		PreparedMesh k;
		k.surfaces.push_back(strip_surface(0, 1, false));
		k.surfaces[0].indices.pop_back();
		CHECK_FALSE(si::apply_scene_variant(k, {}, std::nullopt, slots).ok());
		PreparedMesh q;
		q.surfaces.push_back(strip_surface(0, 1, false));
		q.surfaces[0].indices[0] = -1;
		CHECK_FALSE(si::apply_scene_variant(q, {}, std::nullopt, slots).ok());
	}

	TEST_CASE("render.scene.unit: texture role precedence and shader flags") {
		CHECK(si::parameter_use("Normal").role == TextureRole::Normal);
		CHECK(si::parameter_use("normal").drawn());
		CHECK(si::parameter_use("Diffuse").role == TextureRole::Color);
		CHECK(si::parameter_use("DIFFUSE").drawn());
		CHECK_FALSE(si::parameter_use("SpecularGloss").drawn());
		CHECK(si::parameter_use("SpecularGloss").role == TextureRole::Data);
		CHECK(si::parameter_use("DetailNormal").role == TextureRole::Normal);
		CHECK_FALSE(si::parameter_use("DetailNormal").drawn());
		CHECK(si::parameter_use("MainDiffuse").rank > si::parameter_use("Diffuse").rank);
		CHECK(si::parameter_use("TileNormal").role == TextureRole::Normal);
		CHECK(si::parameter_use("TileNormal").drawn());
		CHECK_FALSE(si::parameter_use("MaskDiffuse").drawn());
		{
			// The plain parameter wins over a fallback wherever it is listed; empty names are skipped.
			MaterialSlot slot;
			slot.textures = { { "TileDiffuse", "t/tile" }, { "MaskNormal", "t/mask_n" }, { "Diffuse", "t/main" }, { "MainNormal", "t/main_n" },
				{ "Normal", "" } };
			const auto [albedo, normal] = si::drawn_parameters(slot);
			CHECK(albedo == 2);
			CHECK(normal == 3);
			MaterialSlot none;
			none.textures = { { "Specular", "t/spec" } };
			CHECK(si::drawn_parameters(none) == std::pair<int32_t, int32_t>{ -1, -1 });
		}
		CHECK(si::merge_role(TextureRole::Color, TextureRole::Normal) == TextureRole::Normal);
		CHECK(si::merge_role(TextureRole::Normal, TextureRole::Color) == TextureRole::Normal);
		CHECK(si::merge_role(TextureRole::Data, TextureRole::Color) == TextureRole::Color);
		CHECK(si::merge_role(TextureRole::Color, TextureRole::Data) == TextureRole::Color);
		CHECK(si::merge_role(TextureRole::Data, TextureRole::Data) == TextureRole::Data);

		CHECK_FALSE(si::shader_flags("shaders/invisible/invisiblecollision").drawn);
		CHECK_FALSE(si::shader_flags("shaders/root/occluder").drawn);
		CHECK_FALSE(si::shader_flags("Shaders/ShadowMesh").drawn);
		const si::ShaderFlags foliage = si::shader_flags("shaders/vegetation/foliage_tree");
		CHECK(foliage.drawn);
		CHECK(foliage.alpha_test);
		CHECK(foliage.double_sided);
		const si::ShaderFlags fence = si::shader_flags("shaders/props/chainlink_fence");
		CHECK(fence.alpha_test);
		CHECK_FALSE(fence.double_sided);
		const si::ShaderFlags plain = si::shader_flags("shaders/root/defaultroot");
		CHECK(plain.drawn);
		CHECK_FALSE(plain.alpha_test);
		CHECK_FALSE(plain.double_sided);
	}

	TEST_CASE("render.scene.unit: terrain surface from a synthetic heightfield") {
		res::HeightGrid g;
		g.width = 4;
		g.height = 3;
		g.origin_xz = { 10.0f, -5.0f };
		g.sample_spacing = { 2.0f, 4.0f };
		g.heights.resize(12);
		for (size_t z = 0; z < 3; ++z) {
			for (size_t x = 0; x < 4; ++x) {
				g.heights[x + z * 4] = 0.5f * float(x) + 0.25f * float(z); // dh/dx = 0.25, dh/dz = 0.0625
			}
		}
		g.holes.assign(12, 0);
		g.holes[3 + 2 * 4] = 1; // corner sample (3, 2): only cell (2, 1) touches it
		Result<PreparedMesh> m = si::terrain_mesh(g, level::LevelTransform::identity(), "synthetic");
		REQUIRE(m.ok());
		REQUIRE(m->surfaces.size() == 1);
		const PreparedSurface &s = m->surfaces[0];
		CHECK(m->triangles == 10); // 6 cells, one with a hole corner
		CHECK(s.indices.size() == 30);
		CHECK(s.vertex_count() == 11); // the hole sample is never used
		CHECK(s.normals.size() == s.positions.size());
		CHECK(s.uv0.empty());
		for (size_t t = 0; t < s.indices.size(); t += 3) {
			const float *a = &s.positions[size_t(s.indices[t]) * 3];
			const float *b = &s.positions[size_t(s.indices[t + 1]) * 3];
			const float *c = &s.positions[size_t(s.indices[t + 2]) * 3];
			// Godot's front faces are clockwise seen from the front: the right-hand normal points down.
			CHECK(cross3(sub3(b, a), sub3(c, a))[1] < 0.0f);
		}
		const float expected_y = 1.0f / std::sqrt(1.0f + 0.25f * 0.25f + 0.0625f * 0.0625f);
		for (size_t v = 0; v < s.vertex_count(); ++v) {
			CHECK(s.normals[v * 3 + 1] > 0.0f);
			CHECK(s.normals[v * 3 + 1] == doctest::Approx(expected_y).epsilon(1e-4));
			CHECK(s.normals[v * 3 + 0] == doctest::Approx(-0.25f * expected_y).epsilon(1e-4));
		}
		CHECK(s.aabb_min[0] == doctest::Approx(10.0f));
		CHECK(s.aabb_max[0] == doctest::Approx(16.0f));
		CHECK(s.aabb_min[2] == doctest::Approx(-5.0f));
		CHECK(s.aabb_max[2] == doctest::Approx(3.0f));

		// A placed terrain: positions through the transform, normals rotated.
		level::LevelTransform t;
		t.trans = { 100.0f, 7.0f, 0.0f };
		Result<PreparedMesh> moved = si::terrain_mesh(g, t, "moved");
		REQUIRE(moved.ok());
		CHECK(moved->surfaces[0].positions[0] == doctest::Approx(110.0f));
		CHECK(moved->surfaces[0].positions[1] == doctest::Approx(7.0f));

		// A crop to x <= 12, z <= -5..-1: 1 x 1 cells.
		Result<PreparedMesh> cropped = si::terrain_mesh(g, level::LevelTransform::identity(), "crop", std::array<float, 4>{ 10.0f, -5.0f, 12.0f, -1.0f });
		REQUIRE(cropped.ok());
		CHECK(cropped->triangles == 2);
		CHECK(cropped->vertices == 4);
		// A crop outside the grid draws nothing.
		Result<PreparedMesh> outside = si::terrain_mesh(g, level::LevelTransform::identity(), "out", std::array<float, 4>{ 500.0f, 500.0f, 600.0f, 600.0f });
		REQUIRE(outside.ok());
		CHECK(outside->surfaces.empty());

		// Hostile grids.
		res::HeightGrid bad = g;
		bad.heights.pop_back();
		CHECK_FALSE(si::terrain_mesh(bad, level::LevelTransform::identity(), "bad").ok());
		res::HeightGrid tiny;
		tiny.width = 1;
		tiny.height = 1;
		tiny.heights = { 0.0f };
		tiny.holes = { 0 };
		CHECK_FALSE(si::terrain_mesh(tiny, level::LevelTransform::identity(), "tiny").ok());
		res::HeightGrid nan = g;
		nan.sample_spacing = { NAN, 1.0f };
		CHECK_FALSE(si::terrain_mesh(nan, level::LevelTransform::identity(), "nan").ok());
		res::HeightGrid huge = g;
		huge.width = res::kMaxGridSide + 1;
		CHECK_FALSE(si::terrain_mesh(huge, level::LevelTransform::identity(), "huge").ok());
	}

	TEST_CASE("render.scene.unit: terrain depth follows the soup's spacing rule") {
		res::HeightfieldTree tree;
		tree.samples_per_side = 65; // 64 interior cells with border 0
		tree.border_width = 0;
		res::HeightfieldNode root;
		root.bounds_min = { 0.0f, 0.0f, 0.0f };
		root.bounds_max = { 1024.0f, 100.0f, 1024.0f };
		tree.nodes.push_back(root);
		// max_level() reads the node ids; one level-3 node allows depths 0..3.
		res::HeightfieldNode deep;
		deep.id.level = 3;
		tree.nodes.push_back(deep);
		CHECK(si::terrain_depth(tree, 16.0f) == 0); // 1024 / 64 = 16 m
		CHECK(si::terrain_depth(tree, 4.0f) == 2); // 1024 / 256 = 4 m
		CHECK(si::terrain_depth(tree, 1.0f) == 3); // capped by the tree's depth
		res::HeightfieldTree empty;
		CHECK(si::terrain_depth(empty, 4.0f) == 0);
	}
}

TEST_SUITE("render-scene") {
	TEST_CASE("render.scene: Metro TDM plan and scene lanes cold and warm") {
		InstallReader reader;
		if (!reader.locate({})) {
			bf3::test::skip_install_case(reader.describe().c_str());
			return;
		}
		const InstallSnapshot before = reader.snapshot();
		Result<std::shared_ptr<Install>> install = Install::open(reader);
		REQUIRE(install.ok());
		using Clock = std::chrono::steady_clock;
		const auto since = [](Clock::time_point p_t) { return std::chrono::duration<double>(Clock::now() - p_t).count(); };

		Clock::time_point t0 = Clock::now();
		Result<std::unique_ptr<level::LoadedLevel>> loaded = level::load_level(*install, "MP_Subway", "TeamDeathMatch0");
		REQUIRE(loaded.ok());
		const level::LoadedLevel &lvl = **loaded;
		Result<std::unique_ptr<level::World>> world = level::World::traverse(lvl);
		REQUIRE(world.ok());
		const double seconds_load = since(t0);
		t0 = Clock::now();
		Result<level::MeshCensus> census = level::mesh_census(lvl, **world);
		REQUIRE(census.ok());
		const double seconds_census = since(t0);
		CHECK(census->placements == 10873);
		CHECK(census->scene_meshes == 842);
		CHECK(census->scene_instances == 10571);

		Result<LevelScenePlan> plan = plan_level_scene(lvl, **world, *census);
		REQUIRE(plan.ok());
		const LevelScenePlan &p = *plan;
		std::printf("render.scene plan: load + traverse %.3f s, census %.3f s, plan %.3f s\n", seconds_load, seconds_census, p.seconds);

		// Requests: the prepare rule (619 MeshSets on Metro TDM, README "prepare").
		check_requests_match_m0(*census, p.requests);
		CHECK(p.requests.size() == 619);

		// Meshes and instances as rules over the census: instances = census instances - start-hidden placements;
		// meshes = kept variants with at least one shown placement.
		size_t kept = 0, start_hidden = 0;
		std::vector<size_t> shown(census->meshes.size(), 0);
		for (const level::SceneMesh &m : census->meshes) {
			kept += m.kept ? 1 : 0;
		}
		for (const level::SceneInstance &inst : census->instances) {
			Result<bool> hidden = level::starts_hidden(**world, (*world)->objects[inst.object]);
			REQUIRE(hidden.ok());
			if (*hidden) {
				++start_hidden;
			} else {
				++shown[inst.mesh];
			}
		}
		size_t expected_meshes = 0;
		for (size_t v = 0; v < census->meshes.size(); ++v) {
			expected_meshes += census->meshes[v].kept && shown[v] > 0 ? 1 : 0;
		}
		std::printf("render.scene plan: %zu requests, %zu meshes (%zu kept variants, %zu with a shown placement), %zu instances (%zu census - %zu "
					"start-hidden), %zu materials, %zu textures, %zu terrains, %zu failures\n",
				p.requests.size(), p.meshes.size(), kept, expected_meshes, p.instances.size(), census->instances.size(), start_hidden, p.materials.size(),
				p.textures.size(), p.terrains.size(), p.failures.size());
		CHECK(kept == 840);
		CHECK(census->instances.size() == 10569);
		CHECK(start_hidden == 64); // bf3dump level: 64 start-hidden mesh placements, all skyline
		CHECK(p.meshes.size() == expected_meshes);
		CHECK(p.meshes.size() <= 840);
		CHECK(p.instances.size() == census->instances.size() - start_hidden);
		CHECK(p.terrains.size() == 1);
		for (const auto &[item, error] : p.failures) {
			std::printf("render.scene plan failure: %s: %s\n", item.c_str(), error.c_str());
		}
		CHECK(p.failures.empty());
		// Every instance names a plan mesh, every plan mesh's instance count adds up, every slot a material.
		std::vector<uint32_t> counted(p.meshes.size(), 0);
		for (const SceneInstancePlan &inst : p.instances) {
			REQUIRE(inst.mesh < p.meshes.size());
			++counted[inst.mesh];
		}
		size_t undrawn_slots = 0, alpha = 0, two_sided = 0, world_meshes = 0, baked = 0, hidden_parts = 0;
		for (size_t m = 0; m < p.meshes.size(); ++m) {
			CHECK(counted[m] == p.meshes[m].instances);
			CHECK(p.meshes[m].request < p.requests.size());
			world_meshes += p.meshes[m].world_kind ? 1 : 0;
			baked += p.meshes[m].bake ? 1 : 0;
			hidden_parts += p.meshes[m].hidden_parts.empty() ? 0 : 1;
			for (uint32_t s : p.meshes[m].surface_materials) {
				if (s == kSceneUndrawnMaterial) {
					++undrawn_slots;
				} else {
					CHECK(s < p.materials.size());
				}
			}
		}
		std::map<std::string, size_t> shaders, parameters;
		std::set<std::string> all_named;
		for (const SceneMaterialPlan &m : p.materials) {
			alpha += m.alpha_test ? 1 : 0;
			two_sided += m.double_sided ? 1 : 0;
			++shaders[m.slot.shader];
			REQUIRE(m.textures.size() == m.slot.textures.size());
			for (size_t i = 0; i < m.textures.size(); ++i) {
				++parameters[m.slot.textures[i].first];
				all_named.insert(m.slot.textures[i].second);
				if (m.textures[i] >= 0) {
					REQUIRE(size_t(m.textures[i]) < p.textures.size());
					CHECK(p.textures[size_t(m.textures[i])].name == m.slot.textures[i].second);
				}
			}
		}
		size_t with_albedo = 0, with_normal = 0;
		for (const SceneMaterialPlan &m : p.materials) {
			if (m.albedo >= 0) {
				++with_albedo;
				REQUIRE(size_t(m.albedo) < p.textures.size());
				CHECK(p.textures[size_t(m.albedo)].role != TextureRole::Data);
				// M1 picked the albedo among the slot's own textures; the L1 resolver (README "L1 look") may take it
				// from the shader graph in the level shader database (textures the slot does not name), so the check
				// is: a slot texture, or a texture the L1 resolver chose (m.look set with a colour).
				CHECK((std::find(m.textures.begin(), m.textures.end(), m.albedo) != m.textures.end() ||
						(m.look.has_value() && m.look->color.has_value())));
			}
			if (m.normal >= 0) {
				++with_normal;
				REQUIRE(size_t(m.normal) < p.textures.size());
				CHECK(p.textures[size_t(m.normal)].role == TextureRole::Normal);
			}
		}
		size_t no_textures = 0;
		std::map<std::string, size_t> unlit_sets; // parameter lists of the textured materials without an albedo
		for (const SceneMaterialPlan &m : p.materials) {
			if (m.slot.textures.empty()) {
				++no_textures;
			} else if (m.albedo < 0) {
				std::string set;
				for (const auto &[name, texture] : m.slot.textures) {
					set += name + " ";
				}
				++unlit_sets[set];
			}
		}
		{
			size_t untextured_meshes = 0, untextured_without_entry = 0, meshes_without_entry = 0, untextured_instances = 0;
			std::map<std::string, size_t> untextured_shaders;
			for (const SceneMeshPlan &m : p.meshes) {
				const bool entry = census->meshes[m.census_mesh].entry.has_value();
				meshes_without_entry += entry ? 0 : 1;
				bool any = false;
				for (uint32_t s : m.surface_materials) {
					if (s != kSceneUndrawnMaterial && p.materials[s].slot.textures.empty()) {
						any = true;
						++untextured_shaders[p.materials[s].slot.shader];
					}
				}
				if (any) {
					++untextured_meshes;
					untextured_instances += m.instances;
					untextured_without_entry += entry ? 0 : 1;
				}
			}
			// Metro: every variant has an entry, but many entries list materials with empty TextureParameters (their
			// textures are not in the EBX the census reads; docs §11.3), so those slots draw untextured in M1.
			std::printf("render.scene plan: %zu meshes without a variation entry; %zu meshes (%zu instances) have a slot that names no texture, %zu of "
						"them without an entry; %zu distinct shaders on such slots\n",
					meshes_without_entry, untextured_meshes, untextured_instances, untextured_without_entry, untextured_shaders.size());
			CHECK(meshes_without_entry == 0);
		}
		std::printf("render.scene plan: %zu materials name no texture; without an albedo:", no_textures);
		for (const auto &[set, count] : unlit_sets) {
			std::printf(" [%s] %zu", set.c_str(), count);
		}
		std::printf("\n");
		std::printf("render.scene plan: %zu of %zu materials have an albedo, %zu a normal map\n", with_albedo, p.materials.size(), with_normal);
		size_t normal_textures = 0;
		for (const SceneTexturePlan &t : p.textures) {
			normal_textures += t.role == TextureRole::Normal ? 1 : 0;
		}
		std::printf("render.scene plan: %zu world-kind meshes, %zu baked, %zu with hidden parts; %zu undrawn slots; %zu materials alpha-tested, %zu "
					"double-sided; %zu textures drawn (%zu normal maps) of %zu named by the materials\n",
				world_meshes, baked, hidden_parts, undrawn_slots, alpha, two_sided, p.textures.size(), normal_textures, all_named.size());
		std::printf("render.scene plan: texture parameters:");
		for (const auto &[name, count] : parameters) {
			std::printf(" %s %zu", name.c_str(), count);
		}
		std::printf("\nrender.scene plan: %zu distinct shaders; the 12 most used:\n", shaders.size());
		std::vector<std::pair<size_t, std::string>> by_use;
		for (const auto &[name, count] : shaders) {
			by_use.emplace_back(count, name);
		}
		std::sort(by_use.rbegin(), by_use.rend());
		for (size_t i = 0; i < by_use.size() && i < 12; ++i) {
			const si::ShaderFlags f = si::shader_flags(by_use[i].second);
			std::printf("  %4zu %s%s%s\n", by_use[i].first, by_use[i].second.c_str(), f.alpha_test ? " [alpha]" : "", f.double_sided ? " [2-sided]" : "");
		}
		const SceneEnvironment &env = p.environment;
		std::printf("render.scene plan: environment from_data %d: sun dir (%.3f %.3f %.3f) colour (%.3f %.3f %.3f) energy %.2f; sky (%.3f %.3f %.3f) "
					"ground (%.3f %.3f %.3f); fog %.0f..%.0f m (%.3f %.3f %.3f)\n",
				int(env.from_data), env.sun_direction[0], env.sun_direction[1], env.sun_direction[2], env.sun_color[0], env.sun_color[1], env.sun_color[2],
				env.sun_energy, env.sky_color[0], env.sky_color[1], env.sky_color[2], env.ground_color[0], env.ground_color[1], env.ground_color[2],
				env.fog_start, env.fog_end, env.fog_color[0], env.fog_color[1], env.fog_color[2]);
		CHECK(env.from_data);

		t0 = Clock::now();
		const uint64_t estimate = estimate_scene_bytes(p, lvl.mount());
		std::printf("render.scene estimate_scene_bytes: %.1f MiB in %.3f s\n", mib(estimate), since(t0));
		CHECK(estimate > 0);

		// Lanes cold then warm on a scratch root.
		ScratchRoot scratch;
		Cache cache;
		CacheOptions options;
		options.root = scratch.root;
		options.guard = reader.write_guard();
		std::string detail;
		REQUIRE_MESSAGE(cache.open(std::move(options), &detail) == CacheStatus::Ok, detail);
		size_t requests_used = 0;
		{
			std::set<uint32_t> used;
			for (const SceneMeshPlan &m : p.meshes) {
				used.insert(m.request);
			}
			requests_used = used.size();
		}
		SceneLaneOptions lane_options;
		SceneLaneStats cold_stats, warm_stats;
		for (int pass = 0; pass < 2; ++pass) {
			SceneLanes lanes;
			CountingSink sink;
			sink.lanes = &lanes;
			sink.mesh_seen.assign(p.meshes.size(), 0);
			sink.texture_seen.assign(p.textures.size(), 0);
			REQUIRE(lanes.start(p, lvl.mount(), &cache, lane_options, sink).ok());
			Result<SceneLaneStats> stats = lanes.wait();
			REQUIRE(stats.ok());
			CHECK(lanes.done());
			print_stats(pass == 0 ? "cold" : "warm", *stats);
			for (const auto &[item, error] : sink.failures) {
				std::printf("render.scene lane failure: %s: %s\n", item.c_str(), error.c_str());
			}
			std::printf("render.scene %s: terrain %llu vertices, %llu triangles; meshes %llu triangles (LOD 0); texture formats:", pass == 0 ? "cold" : "warm",
					(unsigned long long)sink.terrain_vertices, (unsigned long long)sink.terrain_triangles, (unsigned long long)sink.mesh_triangles);
			for (const auto &[format, count] : sink.texture_formats) {
				std::printf(" %s %llu", format.c_str(), (unsigned long long)count);
			}
			std::printf("; largest item %.1f MiB\n", mib(sink.largest));
			CHECK(stats->failures == sink.failures.size());
			CHECK(stats->mesh_items + stats->texture_items + stats->terrain_items + stats->failures >= p.textures.size() + p.terrains.size());
			CHECK(stats->texture_items + std::count_if(sink.failures.begin(), sink.failures.end(),
												   [&](const auto &f) {
													   return std::any_of(p.textures.begin(), p.textures.end(), [&](const SceneTexturePlan &t) { return t.name == f.first; });
												   }) ==
					p.textures.size());
			CHECK(sink.empty_meshes == 0);
			CHECK(stats->terrain_items == 1);
			CHECK(stats->mesh_items == p.meshes.size()); // every plan mesh prepared
			CHECK(stats->peak_inflight_bytes <= std::max(lane_options.max_inflight_bytes, sink.largest.load()));
			CHECK(stats->texture_bytes > 0);
			CHECK(*std::max_element(sink.mesh_seen.begin(), sink.mesh_seen.end()) == 1);
			if (pass == 0) {
				CHECK(stats->cache_converted == requests_used);
				CHECK(stats->cache_hits == 0);
				cold_stats = *stats;
			} else {
				CHECK(stats->cache_hits == requests_used); // warm: only mesh hits
				CHECK(stats->cache_converted == 0);
				warm_stats = *stats;
			}
		}
		CHECK(warm_stats.texture_items == cold_stats.texture_items);
		CHECK(warm_stats.texture_bytes == cold_stats.texture_bytes);
		CHECK(warm_stats.mesh_bytes == cold_stats.mesh_bytes);

		// A consumer that releases only when the lanes stall, and a 64 MiB cap: in-flight bytes stay under the cap (one
		// larger item may pass alone) and the lanes resume after each release.
		{
			SceneLanes lanes;
			HeldSink sink;
			sink.lanes = &lanes;
			sink.start();
			SceneLaneOptions small = lane_options;
			small.max_inflight_bytes = 64ull << 20;
			t0 = Clock::now();
			REQUIRE(lanes.start(p, lvl.mount(), &cache, small, sink).ok());
			Result<SceneLaneStats> stats = lanes.wait();
			sink.finish();
			REQUIRE(stats.ok());
			std::printf("render.scene held sink, 64 MiB cap: %llu items in %.3f s, %llu stalls at the cap, peak in flight %.1f MiB, largest item "
						"%.1f MiB\n",
					(unsigned long long)sink.items.load(), since(t0), (unsigned long long)sink.stalls.load(), mib(stats->peak_inflight_bytes),
					mib(sink.largest));
			CHECK(stats->peak_inflight_bytes <= std::max(small.max_inflight_bytes, sink.largest.load()));
			CHECK(stats->peak_inflight_bytes > small.max_inflight_bytes / 2); // the lanes really ran into the cap
			CHECK(sink.stalls >= 3); // ~370 MiB through a 64 MiB window
			CHECK(sink.items == stats->mesh_items + stats->texture_items + stats->terrain_items);
		}
		// Cancel: a started run that is cancelled at once ends and joins.
		{
			SceneLanes lanes;
			CountingSink sink;
			sink.lanes = &lanes;
			sink.mesh_seen.assign(p.meshes.size(), 0);
			sink.texture_seen.assign(p.textures.size(), 0);
			REQUIRE(lanes.start(p, lvl.mount(), &cache, lane_options, sink).ok());
			CHECK_FALSE(lanes.start(p, lvl.mount(), &cache, lane_options, sink).ok()); // only once
			lanes.cancel();
			Result<SceneLaneStats> stats = lanes.wait();
			REQUIRE(stats.ok());
			CHECK(stats->mesh_items < p.meshes.size());
		}

		// The whole terrain, for comparison with the cropped one the lanes drew.
		{
			LevelScenePlan whole = p;
			whole.instances.clear();
			Result<PreparedMesh> full = prepare_scene_terrain(lvl.mount(), whole, 0);
			REQUIRE(full.ok());
			std::printf("render.scene terrain uncropped: %zu vertices, %zu triangles (%.1f MiB)\n", full->vertices, full->triangles,
					mib(si::prepared_bytes(*full)));
		}
		std::printf("render.scene peak working set %.1f MiB\n", mib(peak_working_set()));
		cache.close();
		const SnapshotDiff diff = diff_snapshots(before, reader.snapshot());
		std::printf("install.unchanged %s\n", diff.clean() ? "PASS" : diff.summary().c_str());
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}
}
