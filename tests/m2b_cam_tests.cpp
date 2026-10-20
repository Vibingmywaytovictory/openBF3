#include <doctest/doctest.h>

#include "death_camera.h"

#include "bf3/host/kits.h"
#include "bf3/render/viewmodel_set.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/static_world.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace gs = bf3::godot_side;
namespace fx = bf3::fx;
namespace sim = bf3::sim;

namespace {

float cam_dot(const fx::Vec3 &p_a, const fx::Vec3 &p_b) {
	return p_a.x * p_b.x + p_a.y * p_b.y + p_a.z * p_b.z;
}

fx::Vec3 cam_sub(const fx::Vec3 &p_a, const fx::Vec3 &p_b) {
	return fx::Vec3{ p_a.x - p_b.x, p_a.y - p_b.y, p_a.z - p_b.z };
}

float cam_len(const fx::Vec3 &p_a) {
	return std::sqrt(cam_dot(p_a, p_a));
}

float looks_at(const gs::DeathCameraView &p_v, const fx::Vec3 &p_point) {
	const fx::Vec3 to = cam_sub(p_point, p_v.camera.trans);
	const float l = cam_len(to);
	return l > 0.0f ? cam_dot(p_v.camera.forward, to) / l : -2.0f;
}

bool orthonormal(const fx::Transform &p_t) {
	const float e = 1e-3f;
	return std::fabs(cam_len(p_t.right) - 1.0f) < e && std::fabs(cam_len(p_t.up) - 1.0f) < e && std::fabs(cam_len(p_t.forward) - 1.0f) < e &&
			std::fabs(cam_dot(p_t.right, p_t.up)) < e && std::fabs(cam_dot(p_t.right, p_t.forward)) < e && std::fabs(cam_dot(p_t.up, p_t.forward)) < e;
}

gs::DeathCameraStart start_at_origin(uint8_t p_killer) {
	gs::DeathCameraStart s;
	s.corpse = { 0.0f, 0.0f, 0.0f };
	s.eye = { 0.0f, 1.6f, 0.0f };
	s.yaw_rad = 0.0f;
	s.killer_slot = p_killer;
	s.killer_camera = true;
	return s;
}

void cam_quad(std::vector<sim::StaticTriangle> &r_out, const sim::Vec3 &p_a, const sim::Vec3 &p_b, const sim::Vec3 &p_c, const sim::Vec3 &p_d) {
	sim::StaticTriangle t;
	t.v = { p_a, p_b, p_c };
	r_out.push_back(t);
	t.v = { p_a, p_c, p_d };
	r_out.push_back(t);
}

}

TEST_SUITE("m2b-cam-unit") {
	TEST_CASE("death camera: the corpse view without a killer, continuity, the killer lost, hostile inputs") {
		const gs::DeathCameraParams p{};

		{
			gs::Bf3DeathCamera cam;
			const fx::Vec3 killer{ 20.0f, 0.0f, 0.0f };
			cam.begin(start_at_origin(0xFF), 10.0);
			const gs::DeathCameraView v0 = cam.step(10.0, &killer, nullptr);
			REQUIRE(v0.active);
			CHECK(cam_len(cam_sub(v0.camera.trans, fx::Vec3{ 0.0f, 1.6f, 0.0f })) < 1e-4f);
			CHECK(v0.camera.forward.z > 0.99f);
			const gs::DeathCameraView v1 = cam.step(10.0 + p.orbit_seconds + 0.01, &killer, nullptr);
			CHECK(!v1.on_killer);
			CHECK(v1.camera.trans.z == doctest::Approx(-p.back_m).epsilon(1e-3));
			CHECK(v1.camera.trans.y == doctest::Approx(p.up_m).epsilon(1e-3));
			CHECK(looks_at(v1, fx::Vec3{ 0.0f, p.eye_height_m, 0.0f }) > 0.999f);
			CHECK(orthonormal(v1.camera));
			const gs::DeathCameraView v2 = cam.step(13.0, &killer, nullptr);
			CHECK(!v2.on_killer);
			CHECK(!v2.finished);
			CHECK(cam.step(14.0, &killer, nullptr).finished);
			CHECK(cam.active());
			CHECK(cam.stats().killer_views == 0);

			cam.begin(start_at_origin(0xFF), 0.0);
			fx::Vec3 prev = cam.step(0.0, nullptr, nullptr).camera.trans;
			for (int f = 1; f <= 240; ++f) {
				const fx::Vec3 now = cam.step(f / 60.0, nullptr, nullptr).camera.trans;
				CHECK(cam_len(cam_sub(now, prev)) < 0.25f);
				prev = now;
			}
		}

		{
			gs::Bf3DeathCamera cam;
			gs::DeathCameraStart s = start_at_origin(5);
			s.killer_camera = false;
			const fx::Vec3 killer{ 20.0f, 0.0f, 0.0f };
			cam.begin(s, 0.0);
			const gs::DeathCameraView v = cam.step(3.0, &killer, nullptr);
			CHECK(!v.on_killer);
			CHECK(looks_at(v, fx::Vec3{ 0.0f, p.eye_height_m, 0.0f }) > 0.999f);
		}

		{
			gs::Bf3DeathCamera cam;
			const fx::Vec3 killer{ 0.0f, 0.0f, -20.0f };
			cam.begin(start_at_origin(5), 0.0);
			fx::Vec3 prev = cam.step(0.0, &killer, nullptr).camera.trans;
			for (int f = 1; f <= 200; ++f) {
				const gs::DeathCameraView v = cam.step(f / 60.0, &killer, nullptr);
				CHECK(cam_len(cam_sub(v.camera.trans, prev)) < 0.3f);
				CHECK(orthonormal(v.camera));
				prev = v.camera.trans;
			}
			const fx::Vec3 aim{ killer.x, killer.y + p.killer_height_m, killer.z };
			const gs::DeathCameraView lost = cam.step(3.5, nullptr, nullptr);
			CHECK(lost.on_killer);
			CHECK(looks_at(lost, aim) > 0.999f);

			CHECK(lost.camera.trans.z > 0.0f);
			const fx::Vec3 nan{ std::nanf(""), 0.0f, 0.0f };
			const uint64_t bad = cam.stats().bad_inputs;
			const gs::DeathCameraView ignored = cam.step(3.6, &nan, nullptr);
			CHECK(ignored.active);
			CHECK(cam.stats().bad_inputs == bad + 1);
			CHECK(looks_at(ignored, aim) > 0.999f);
			CHECK(cam.stats().killer_views == 1);

			const gs::DeathCameraView ended = cam.step(std::numeric_limits<double>::quiet_NaN(), &killer, nullptr);
			CHECK(!ended.active);
			CHECK(!cam.active());
			CHECK(cam.stats().bad_inputs == bad + 2);
			cam.begin(start_at_origin(5), 0.0);
			cam.end();
			CHECK(!cam.step(1.0, &killer, nullptr).active);
			CHECK(cam.stats().views == 2);
		}

		{
			gs::DeathCameraParams h;
			h.seconds = std::numeric_limits<float>::quiet_NaN();
			h.orbit_seconds = -1.0f;
			h.back_m = std::numeric_limits<float>::infinity();
			h.camera_radius_m = -3.0f;
			h.up_m = 2.0f;
			gs::Bf3DeathCamera cam(h);
			CHECK(cam.params().seconds == p.seconds);
			CHECK(cam.params().orbit_seconds == p.orbit_seconds);
			CHECK(cam.params().back_m == p.back_m);
			CHECK(cam.params().camera_radius_m == p.camera_radius_m);
			CHECK(cam.params().up_m == 2.0f);

			gs::DeathCameraStart s = start_at_origin(0xFF);
			s.yaw_rad = std::numeric_limits<float>::infinity();
			cam.begin(s, 0.0);
			CHECK(!cam.active());
			CHECK(cam.stats().bad_inputs == 1);
		}
	}

	TEST_CASE("death camera: a wall behind the corpse pulls the camera in") {

		std::vector<sim::StaticTriangle> tris;
		cam_quad(tris, { -5.0f, -1.0f, -1.0f }, { -5.0f, 5.0f, -1.0f }, { 5.0f, 5.0f, -1.0f }, { 5.0f, -1.0f, -1.0f });
		bf3::core::Result<std::shared_ptr<const sim::StaticWorldShapes>> shapes = sim::build_static_world(std::span<const sim::StaticTriangle>(tris));
		REQUIRE_MESSAGE(shapes.ok(), (shapes.ok() ? std::string() : shapes.error().message));
		bf3::core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(*shapes);
		REQUIRE_MESSAGE(world.ok(), (world.ok() ? std::string() : world.error().message));
		const gs::DeathCameraParams p{};
		gs::Bf3DeathCamera cam;
		cam.begin(start_at_origin(0xFF), 0.0);
		const gs::DeathCameraView v = cam.step(2.0, nullptr, world->get());
		REQUIRE(v.active);
		CHECK(v.camera.trans.z > -1.0f);
		CHECK(v.camera.trans.z < -0.5f);
		CHECK(cam.stats().pulled_in == 1);
		CHECK(looks_at(v, fx::Vec3{ 0.0f, p.eye_height_m, 0.0f }) > 0.999f);

		gs::Bf3DeathCamera open;
		open.begin(start_at_origin(0xFF), 0.0);
		CHECK(open.step(2.0, nullptr, nullptr).camera.trans.z == doctest::Approx(-p.back_m).epsilon(1e-3));
	}
}

TEST_SUITE("m2b-cam-unit") {
	TEST_CASE("defib viewmodel: add_assault_gadget puts slot 2 into the assault kits' item set, M2a's set unchanged") {
		const std::span<const bf3::render::ViewmodelKit> documented = bf3::render::documented_viewmodel_kits();
		std::vector<bf3::render::ViewmodelKit> kits(documented.begin(), documented.end());
		REQUIRE(kits.size() == 8);
		const auto has = [](const bf3::render::ViewmodelSetSpec &p_spec, std::string_view p_unlock) {
			return std::count_if(p_spec.items.begin(), p_spec.items.end(), [&](const bf3::render::ViewmodelItemSpec &p_i) { return p_i.unlock == p_unlock; });
		};
		const std::string defib(bf3::host::kTdmAssaultGadget);
		const bf3::render::ViewmodelSetSpec us_m2a = bf3::render::team_viewmodel_set(kits, 1);
		const bf3::render::ViewmodelSetSpec ru_m2a = bf3::render::team_viewmodel_set(kits, 2);
		CHECK(has(us_m2a, defib) == 0);
		CHECK(us_m2a.items.size() == 7);
		CHECK(bf3::render::add_assault_gadget(kits, "") == 0);
		CHECK(bf3::render::add_assault_gadget(kits, defib) == 2);
		for (const bf3::render::ViewmodelKit &k : kits) {
			CHECK((k.gadget == defib) == (k.kit.find("assault") != std::string::npos));
		}
		const bf3::render::ViewmodelSetSpec us = bf3::render::team_viewmodel_set(kits, 1);
		const bf3::render::ViewmodelSetSpec ru = bf3::render::team_viewmodel_set(kits, 2);
		CHECK(has(us, defib) == 1);
		CHECK(has(ru, defib) == 1);
		CHECK(us.items.size() == us_m2a.items.size() + 1);
		CHECK(ru.items.size() == ru_m2a.items.size() + 1);
		CHECK(us.items[4].unlock == defib);
		CHECK(bf3::render::viewmodel_kits_difference(kits, documented).empty());
	}
}
