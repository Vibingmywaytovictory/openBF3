#include "bf3/core/res/terrain_decals.h"
#include "bf3/core/superbundle_mount.h"
#include "test_support.h"

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace bf3;
using namespace bf3::core;
using namespace bf3::core::res;

namespace {

struct Blob {
	std::vector<uint8_t> bytes;

	void u8(uint8_t p_v) { bytes.push_back(p_v); }
	void u16(uint16_t p_v) {
		bytes.push_back(uint8_t(p_v));
		bytes.push_back(uint8_t(p_v >> 8));
	}
	void u32(uint32_t p_v) {
		for (int i = 0; i < 4; ++i) {
			bytes.push_back(uint8_t(p_v >> (8 * i)));
		}
	}
	void f32(float p_v) { u32(std::bit_cast<uint32_t>(p_v)); }
	void text(const char *p_text) { bytes.insert(bytes.end(), p_text, p_text + std::strlen(p_text) + 1); }
	void record(const char *p_shader, uint32_t p_first_index, uint32_t p_triangles, float p_inverse_order,
			uint32_t p_first_vertex, uint32_t p_end_vertex) {
		text(p_shader);
		u8(0);
		u32(p_first_index);
		u32(p_triangles);
		for (int i = 0; i < 12; ++i) {
			f32(float(i));
		}
		f32(p_inverse_order);
		u32(p_first_vertex);
		u32(p_end_vertex);
	}
};

constexpr uint16_t kHalfOne = 0x3C00;
constexpr uint16_t kHalfTwo = 0x4000;

Blob three_groups() {
	Blob b;
	b.f32(60.0f);
	b.f32(0.0f);
	b.u32(2);
	b.u8(1);
	b.u32(1);
	b.record("Levels/Test/Road2D", 0, 1, 1.0f / 111.0f, 0, 3);
	b.u32(3 * 24);
	b.u32(3 * 4);
	for (int v = 0; v < 3; ++v) {
		b.f32(10.0f + float(v));
		b.f32(20.0f + float(v));
		b.f32(0.25f * float(v));
		b.f32(4.0f);
		for (int k = 0; k < 4; ++k) {
			b.u16(kHalfOne);
		}
	}
	for (uint32_t i = 0; i < 3; ++i) {
		b.u32(i);
	}
	b.u32(1);
	b.record("Levels/Test/Road3D", 0, 1, 1.0f / 101.0f, 0, 3);
	b.u32(3 * 28);
	b.u32(3 * 4);
	for (int v = 0; v < 3; ++v) {
		b.f32(10.0f + float(v));
		b.f32(130.0f);
		b.f32(20.0f + float(v));
		b.f32(1.0f);
		b.f32(2.0f);
		for (int k = 0; k < 4; ++k) {
			b.u16(kHalfOne);
		}
	}
	for (uint32_t i = 0; i < 3; ++i) {
		b.u32(i);
	}
	b.u32(1);
	b.record("Levels/Test/River", 0, 1, 1.0f / 101.0f, 0, 3);
	b.u32(3 * 36);
	b.u32(3 * 4);
	for (int v = 0; v < 3; ++v) {
		for (const uint16_t h : { kHalfTwo, kHalfOne, kHalfTwo, kHalfOne }) {
			b.u16(h);
		}
		for (const uint16_t h : { uint16_t(0), kHalfOne, uint16_t(0), uint16_t(0) }) {
			b.u16(h);
		}
		for (const uint16_t h : { kHalfOne, uint16_t(0), uint16_t(0), uint16_t(0) }) {
			b.u16(h);
		}
		b.u16(kHalfOne);
		b.u16(kHalfTwo);
		for (int k = 0; k < 4; ++k) {
			b.u16(kHalfOne);
		}
	}
	for (uint32_t i = 0; i < 3; ++i) {
		b.u32(i);
	}
	return b;
}

template <class A, class B> bool same(const A &p_a, const B &p_b) {
	for (size_t i = 0; i < p_b.size(); ++i) {
		if (p_a[i] != p_b[i]) {
			return false;
		}
	}
	return true;
}

struct DecalCounts {
	size_t planar_records = 0, planar_vertices = 0, planar_indices = 0;
	size_t surface_records = 0, surface_vertices = 0, surface_indices = 0;
	size_t water_records = 0, water_vertices = 0, water_indices = 0;
};

void check_level_decals(const char *p_superbundle, const char *p_res, const DecalCounts &p_data) {
	test::Mounted *m = test::shared_mount_or_skip();
	if (m == nullptr) {
		return;
	}
	Result<LevelMount> mount = m->install->mount_superbundle(p_superbundle);
	REQUIRE(mount.ok());
	REQUIRE(mount->load_all_bundles().ok());
	auto res = mount->read_res(p_res);
	REQUIRE_MESSAGE(res.ok(), (res.ok() ? std::string() : res.error().message));
	TerrainDecals decals;
	const ResError parsed = TerrainDecals::parse(res->second, decals);
	REQUIRE_MESSAGE(parsed.ok(), parsed.message());
	std::printf("%s: head %g %g %u %u; planar %zu records %zu vertices %zu indices; surface %zu / %zu / %zu; water %zu "
				"/ %zu / %zu\n",
			p_res, double(decals.head_a), double(decals.head_b), decals.head_c, unsigned(decals.head_d),
			decals.planar.records.size(), decals.planar.vertices.size(), decals.planar.indices.size(),
			decals.surface.records.size(), decals.surface.vertices.size(), decals.surface.indices.size(),
			decals.water.records.size(), decals.water.vertices.size(), decals.water.indices.size());
	CHECK(decals.planar.records.size() == p_data.planar_records);
	CHECK(decals.planar.vertices.size() == p_data.planar_vertices);
	CHECK(decals.planar.indices.size() == p_data.planar_indices);
	CHECK(decals.surface.records.size() == p_data.surface_records);
	CHECK(decals.surface.vertices.size() == p_data.surface_vertices);
	CHECK(decals.surface.indices.size() == p_data.surface_indices);
	CHECK(decals.water.records.size() == p_data.water_records);
	CHECK(decals.water.vertices.size() == p_data.water_vertices);
	CHECK(decals.water.indices.size() == p_data.water_indices);
	for (const TerrainDecalSet *set : { &decals.planar, &decals.surface, &decals.water }) {
		for (const TerrainDecalRecord &r : set->records) {
			CHECK(r.flag == 0);
			CHECK(r.draw_order() > 0);
			CHECK(std::fabs(1.0f / float(r.draw_order() + 1) - r.inverse_order) < 1e-6f);
			CHECK_FALSE(r.shader.empty());
		}
		for (const TerrainDecalVertex &v : set->vertices) {
			CHECK(std::isfinite(v.position[0] + v.position[1] + v.position[2] + v.uv[0] + v.uv[1]));
		}
	}
}

}

TEST_SUITE("res_decals_unit") {
	TEST_CASE("terrain decals: the three vertex layouts decode") {
		const Blob b = three_groups();
		TerrainDecals d;
		const ResError e = TerrainDecals::parse(b.bytes, d);
		REQUIRE_MESSAGE(e.ok(), e.message());
		CHECK(d.head_a == 60.0f);
		CHECK(d.head_c == 2);
		CHECK(d.head_d == 1);
		REQUIRE(d.planar.records.size() == 1);
		CHECK(d.planar.records[0].shader == "Levels/Test/Road2D");
		CHECK(d.planar.records[0].draw_order() == 110);
		CHECK(same(d.planar.records[0].bounds_min, Vec3{ 0.0f, 1.0f, 2.0f }));
		CHECK(same(d.planar.records[0].patch_max, Vec3{ 9.0f, 10.0f, 11.0f }));
		REQUIRE(d.planar.vertices.size() == 3);
		CHECK(same(d.planar.vertices[1].position, Vec3{ 11.0f, 0.0f, 21.0f }));
		CHECK(same(d.planar.vertices[2].uv, Vec2{ 0.5f, 4.0f }));
		CHECK(same(d.planar.vertices[0].mask, Vec4{ 1.0f, 1.0f, 1.0f, 1.0f }));
		CHECK(d.planar.indices == std::vector<uint32_t>{ 0, 1, 2 });
		REQUIRE(d.surface.vertices.size() == 3);
		CHECK(same(d.surface.vertices[2].position, Vec3{ 12.0f, 130.0f, 22.0f }));
		CHECK(same(d.surface.vertices[0].uv, Vec2{ 1.0f, 2.0f }));
		CHECK(d.surface.records[0].draw_order() == 100);
		REQUIRE(d.water.vertices.size() == 3);
		CHECK(same(d.water.vertices[0].position, Vec3{ 2.0f, 1.0f, 2.0f }));
		CHECK(same(d.water.vertices[0].normal, Vec3{ 0.0f, 1.0f, 0.0f }));
		CHECK(same(d.water.vertices[0].tangent, Vec3{ 1.0f, 0.0f, 0.0f }));
		CHECK(same(d.water.vertices[0].uv, Vec2{ 1.0f, 2.0f }));
	}

	TEST_CASE("terrain decals: an empty group has no buffers, and bad input fails") {
		Blob empty;
		empty.f32(60.0f);
		empty.f32(0.0f);
		empty.u32(2);
		empty.u8(0);
		empty.u32(0);
		empty.u32(0);
		empty.u32(0);
		TerrainDecals d;
		CHECK(TerrainDecals::parse(empty.bytes, d).ok());
		CHECK(d.planar.records.empty());
		const Blob whole = three_groups();
		for (size_t cut = 0; cut < whole.bytes.size(); cut += 7) {
			const std::vector<uint8_t> part(whole.bytes.begin(), whole.bytes.begin() + cut);
			CHECK_FALSE(TerrainDecals::parse(part, d).ok());
		}
		std::vector<uint8_t> trailing = whole.bytes;
		trailing.push_back(0);
		CHECK_FALSE(TerrainDecals::parse(trailing, d).ok());
		for (size_t at = 13; at < whole.bytes.size(); at += 5) {
			std::vector<uint8_t> flipped = whole.bytes;
			flipped[at] ^= 0xFF;
			TerrainDecals other;
			(void)TerrainDecals::parse(flipped, other);
		}
	}
}

TEST_SUITE("res_decals") {
	TEST_CASE("terrain decals: MP_007 roads, quad decals and river from the install") {
		check_level_decals("Win32/Levels/MP_007/MP_007", "levels/mp_007/terrain/mp_007.decals",
				DecalCounts{ 281, 6724, 16971, 238, 32816, 137985, 42, 758, 1944 });
	}

	TEST_CASE("terrain decals: MP_Subway from the install") {
		check_level_decals("Win32/Levels/MP_Subway/MP_Subway", "levels/mp_subway/terrain/mp_subway_terrain.decals",
				DecalCounts{ 44, 4257, 11784, 46, 6887, 20682, 25, 210, 399 });
	}

	TEST_CASE("terrain decals: MP_018 from the install") {
		check_level_decals("Win32/Levels/MP_018/MP_018", "levels/mp_018/terrain/terrain_mp018.decals",
				DecalCounts{ 503, 7678, 18414, 503, 37834, 161898, 556, 2423, 3642 });
	}

	TEST_CASE("terrain decals: MP_001 has no water group") {
		check_level_decals("Win32/Levels/MP_001/MP_001", "levels/mp_001/terrain/mp001_terrain/mp001_terrain.decals",
				DecalCounts{ 29, 864, 1830, 29, 1439, 3846, 0, 0, 0 });
	}
}
