#pragma once

#include "bf3/core/level/collision_soup.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/static_world.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace sim_world_test {

using namespace bf3;
using namespace bf3::sim;
namespace level = bf3::core::level;

inline SurfaceTag tag_of(uint32_t p_material, uint8_t p_surface, uint8_t p_body = 0, uint8_t p_flags = 0) {
	SurfaceTag t;
	t.material = p_material;
	t.surface = p_surface;
	t.body = p_body;
	t.flags = p_flags;
	return t;
}

inline void add_floor(std::vector<StaticTriangle> &r_out, float p_x0, float p_z0, float p_x1, float p_z1, float p_y, SurfaceTag p_tag = {},
		StaticLayer p_layer = StaticLayer::Collision) {
	StaticTriangle a;
	a.v = { Vec3{ p_x0, p_y, p_z0 }, Vec3{ p_x0, p_y, p_z1 }, Vec3{ p_x1, p_y, p_z0 } };
	a.tag = p_tag;
	a.layer = p_layer;
	StaticTriangle b;
	b.v = { Vec3{ p_x1, p_y, p_z0 }, Vec3{ p_x0, p_y, p_z1 }, Vec3{ p_x1, p_y, p_z1 } };
	b.tag = p_tag;
	b.layer = p_layer;
	r_out.push_back(a);
	r_out.push_back(b);
}

inline void add_wall_x(std::vector<StaticTriangle> &r_out, float p_x, float p_z0, float p_z1, float p_y0, float p_y1, SurfaceTag p_tag = {},
		StaticLayer p_layer = StaticLayer::Collision) {
	StaticTriangle a;
	a.v = { Vec3{ p_x, p_y0, p_z0 }, Vec3{ p_x, p_y1, p_z0 }, Vec3{ p_x, p_y0, p_z1 } };
	a.tag = p_tag;
	a.layer = p_layer;
	StaticTriangle b;
	b.v = { Vec3{ p_x, p_y1, p_z0 }, Vec3{ p_x, p_y1, p_z1 }, Vec3{ p_x, p_y0, p_z1 } };
	b.tag = p_tag;
	b.layer = p_layer;
	r_out.push_back(a);
	r_out.push_back(b);
}

inline std::vector<StaticTriangle> bumpy_field(float p_half) {
	std::vector<StaticTriangle> out;
	const auto h = [](float p_x, float p_z) { return 0.8f * std::sin(p_x * 0.21f) + 0.6f * std::cos(p_z * 0.17f) + 0.05f * p_x; };
	uint32_t cell = 0;
	for (float x = -p_half; x < p_half; x += 2.0f) {
		for (float z = -p_half; z < p_half; z += 2.0f, ++cell) {
			const Vec3 a{ x, h(x, z), z };
			const Vec3 b{ x + 2.0f, h(x + 2.0f, z), z };
			const Vec3 c{ x, h(x, z + 2.0f), z + 2.0f };
			const Vec3 d{ x + 2.0f, h(x + 2.0f, z + 2.0f), z + 2.0f };
			StaticTriangle t0;
			t0.v = { a, c, b };
			t0.tag = tag_of(cell % 7, uint8_t(cell % 3), 0, uint8_t(cell % 2 == 0 ? level::kSoupTerrain : 0));
			StaticTriangle t1;
			t1.v = { b, c, d };
			t1.tag = t0.tag;
			out.push_back(t0);
			out.push_back(t1);
		}
	}
	return out;
}

inline std::shared_ptr<const StaticWorldShapes> build(const std::vector<StaticTriangle> &p_triangles, const StaticWorldBuildOptions &p_options = {}) {
	core::Result<std::shared_ptr<const StaticWorldShapes>> r = build_static_world(std::span<const StaticTriangle>(p_triangles), p_options);
	REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
	return *r;
}

inline std::unique_ptr<CollisionWorld> world_of(std::shared_ptr<const StaticWorldShapes> p_shapes) {
	core::Result<std::unique_ptr<CollisionWorld>> r = CollisionWorld::create(std::move(p_shapes));
	REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
	return std::move(*r);
}

inline bool bits_equal(const StaticHit &p_a, const StaticHit &p_b) {
	return std::memcmp(p_a.position.data(), p_b.position.data(), sizeof(Vec3)) == 0 && std::memcmp(p_a.normal.data(), p_b.normal.data(), sizeof(Vec3)) == 0 &&
			std::memcmp(&p_a.fraction, &p_b.fraction, sizeof(float)) == 0 && p_a.layer == p_b.layer && p_a.tile == p_b.tile && p_a.tag == p_b.tag;
}

}
