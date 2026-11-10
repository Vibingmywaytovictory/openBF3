#include <doctest/doctest.h>

#include "death_camera.h"

#include "bf3/core/gameplay/death_timeline.h"
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

gs::ChaseTarget target_at(fx::Vec3 p_eye) {
	gs::ChaseTarget t;
	t.eye = p_eye;
	return t;
}

bf3::core::gameplay::ChaseCameraDef killer_chase() {
	bf3::core::gameplay::ChaseCameraDef c;
	c.target_offset = { -0.1f, -0.2f, 0.1f };
	c.target_rotation_offset_deg = 15.0f;
	c.wanted_angle_deg = 5.0f;
	c.wanted_distance = 2.5f;
	c.pill_maximum_collision_radius = 0.4f;
	c.pill_minimum_collision_radius = 0.1f;
	c.pill_minimum_collision_length = 0.1f;
	c.to_wanted_position_scale = { 100.0f, 100.0f, 100.0f };
	c.velocity_drag = 15.0f;
	c.max_velocity = 5.0f;
	c.update_rate = 30.0f;
	c.has_collision = true;
	return c;
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
	TEST_CASE("chase camera frame: the pivot shifts along the eye frame, TargetRotationOffset turns forward toward the "
			  "right axis, the camera sits behind and raised by WantedAngleDeg (R3-C5 TV3, TV4)") {
		const bf3::core::gameplay::ChaseCameraDef c2 = killer_chase();
		const gs::ChaseFrame f = gs::chase_frame(c2, target_at({ 10.0f, 2.0f, 5.0f }));
		CHECK(f.pivot.x == doctest::Approx(9.9f));
		CHECK(f.pivot.y == doctest::Approx(1.8f));
		CHECK(f.pivot.z == doctest::Approx(5.1f));
		CHECK(f.forward.x == doctest::Approx(0.2588f).epsilon(1e-3));
		CHECK(f.forward.z == doctest::Approx(0.9659f).epsilon(1e-3));
		CHECK(f.direction.x == doctest::Approx(-0.2578f).epsilon(1e-3));
		CHECK(f.direction.y == doctest::Approx(0.0872f).epsilon(1e-3));
		CHECK(f.direction.z == doctest::Approx(-0.9622f).epsilon(1e-3));
		gs::ChaseCamera cam(c2);
		cam.activate(target_at({ 10.0f, 2.0f, 5.0f }), nullptr);
		CHECK(cam.position().x == doctest::Approx(10.0f - 0.641f).epsilon(1e-3));
		CHECK(cam.position().y == doctest::Approx(2.0f - 0.017f).epsilon(1e-2));
		CHECK(cam.position().z == doctest::Approx(5.0f - 1.921f).epsilon(1e-3));
		CHECK(cam.velocity().x == 0.0f);

		bf3::core::gameplay::ChaseCameraDef c1;
		c1.target_offset = { 0.0f, -0.1f, 0.0f };
		c1.target_rotation_offset_deg = 180.0f;
		c1.wanted_angle_deg = 15.0f;
		c1.wanted_distance = 3.0f;
		c1.pill_maximum_collision_radius = 0.4f;
		gs::ChaseCamera front(c1);
		front.activate(target_at({ 0.0f, 0.0f, 0.0f }), nullptr);
		CHECK(front.position().x == doctest::Approx(0.0f).epsilon(1e-3));
		CHECK(front.position().y == doctest::Approx(0.573f).epsilon(1e-3));
		CHECK(front.position().z == doctest::Approx(2.511f).epsilon(1e-3));
	}

	TEST_CASE("chase camera dynamics: the spring toward the wanted point with VelocityDrag and the MaxVelocity clamp "
			  "at 1/UpdateRate steps (R2-C5 TV6)") {
		bf3::core::gameplay::ChaseCameraDef c = killer_chase();
		c.target_rotation_offset_deg = 0.0f;
		c.target_offset = {};
		c.wanted_angle_deg = 0.0f;
		gs::ChaseCamera cam(c);
		cam.activate(target_at({ 0.0f, 0.0f, 0.0f }), nullptr);
		const float z0 = cam.position().z;
		CHECK(z0 == doctest::Approx(-2.1f));
		const double h = 1.0 / 30.0;
		cam.step(h, target_at({ 0.0f, 0.0f, 1.0f }), nullptr);
		CHECK(cam.velocity().z == doctest::Approx(3.333f).epsilon(1e-3));
		CHECK(cam.position().z - z0 == doctest::Approx(0.111f).epsilon(1e-2));
		cam.step(h, target_at({ 0.0f, 0.0f, 1.0f }), nullptr);
		CHECK(cam.velocity().z == doctest::Approx(4.630f).epsilon(1e-3));
		cam.step(h, target_at({ 0.0f, 0.0f, 1.0f }), nullptr);
		CHECK(cam.velocity().z == doctest::Approx(4.765f).epsilon(1e-3));
		for (int i = 0; i < 20; ++i) {
			cam.step(h, target_at({ 0.0f, 0.0f, 1.0f }), nullptr);
			CHECK(std::sqrt(cam.velocity().x * cam.velocity().x + cam.velocity().y * cam.velocity().y +
						  cam.velocity().z * cam.velocity().z) <= 5.0001f);
		}
	}

	TEST_CASE("killer camera: the victim keeps his own view until KillerCameraTimer, then a cut to the killer's chase "
			  "camera from black with a 0.25 s fade-in; released when the killer is lost; no killer, no camera "
			  "(R2-C5 TV1, R3-C5 TV5)") {
		gs::DeathCameraParams p;
		p.killer_camera_s = 1.0f;
		p.fade_in_s = 0.25f;
		p.release_if_target_lost = true;
		p.has_chase = true;
		p.chase = killer_chase();
		gs::Bf3DeathCamera cam(p);
		gs::DeathCameraStart s;
		s.killer_slot = 4;
		cam.begin(s, 10.0);
		const gs::ChaseTarget killer = target_at({ 0.0f, 1.6f, 20.0f });
		CHECK(!cam.step(10.5, &killer, nullptr).active);
		CHECK(!cam.step(10.99, &killer, nullptr).active);
		const gs::DeathCameraView cut = cam.step(11.0, &killer, nullptr);
		REQUIRE(cut.active);
		CHECK(cut.black == 1.0f);
		CHECK(cut.camera.trans.z == doctest::Approx(20.0f - 1.921f).epsilon(1e-3));
		CHECK(cam.step(11.125, &killer, nullptr).black == doctest::Approx(0.5f));
		const gs::DeathCameraView in = cam.step(11.25, &killer, nullptr);
		CHECK(in.active);
		CHECK(in.black == 0.0f);
		CHECK(cam.stats().killer_views == 1);
		CHECK(!cam.step(11.3, nullptr, nullptr).active);
		CHECK(!cam.active());
		CHECK(cam.stats().released_lost == 1);

		gs::Bf3DeathCamera suicide(p);
		suicide.begin(gs::DeathCameraStart{}, 0.0);
		CHECK(!suicide.step(2.0, &killer, nullptr).active);
		gs::Bf3DeathCamera off(p);
		gs::DeathCameraStart no;
		no.killer_slot = 4;
		no.killer_camera = false;
		off.begin(no, 0.0);
		CHECK(!off.step(2.0, &killer, nullptr).active);
	}

	TEST_CASE("killer camera from PlayerKillerCamera's data: the soldier camera its target mode picks, the fade out on "
			  "OnTakeControl then the fade in, the target lost holds the view unless ReleaseControlIfTargetLost") {
		bf3::core::gameplay::SoldierCameras cams;
		cams.cameras.resize(3);
		cams.cameras[0].kind = bf3::core::gameplay::SoldierCameraKind::FirstPerson;
		cams.cameras[1].kind = bf3::core::gameplay::SoldierCameraKind::Chase;
		cams.cameras[1].chase.wanted_distance = 3.0f;
		cams.cameras[2].kind = bf3::core::gameplay::SoldierCameraKind::Chase;
		cams.cameras[2].chase = killer_chase();
		bf3::core::gameplay::PlayerCameraDef pc;
		pc.soldier_target_mode = bf3::core::gameplay::CameraTargetMode::Index;
		pc.soldier_camera_index = 2;
		pc.release_if_target_lost = false;
		pc.fade_out_s = 0.5f;
		pc.fade_in_s = 1.0f;
		const bf3::core::gameplay::DeathTimeline death{ 2.0f, 4.0f };
		const gs::DeathCameraParams p = gs::death_camera_params(death, &cams, &pc);
		CHECK(p.killer_camera_s == 2.0f);
		CHECK(p.has_chase);
		CHECK(p.chase.wanted_distance == 2.5f);
		CHECK(p.fade_out_s == 0.5f);
		CHECK(p.fade_in_s == 1.0f);
		CHECK(!p.release_if_target_lost);
		pc.soldier_target_mode = bf3::core::gameplay::CameraTargetMode::ThirdPerson;
		CHECK(gs::death_camera_params(death, &cams, &pc).chase.wanted_distance == 3.0f);
		pc.soldier_target_mode = bf3::core::gameplay::CameraTargetMode::FirstPerson;
		CHECK(!gs::death_camera_params(death, &cams, &pc).has_chase);
		pc.soldier_target_mode = bf3::core::gameplay::CameraTargetMode::Index;
		pc.soldier_camera_index = 3;
		CHECK(!gs::death_camera_params(death, &cams, &pc).has_chase);
		CHECK(!gs::death_camera_params(death, &cams, nullptr).has_chase);

		gs::Bf3DeathCamera cam(p);
		gs::DeathCameraStart s;
		s.killer_slot = 4;
		cam.begin(s, 0.0);
		const gs::ChaseTarget killer = target_at({ 0.0f, 1.6f, 20.0f });
		CHECK(!cam.step(1.9, &killer, nullptr).active);
		const gs::DeathCameraView cut = cam.step(2.0, &killer, nullptr);
		REQUIRE(cut.active);
		CHECK(cut.black == 0.0f);
		CHECK(cam.step(2.25, &killer, nullptr).black == doctest::Approx(0.5f));
		CHECK(cam.step(2.5, &killer, nullptr).black == 1.0f);
		CHECK(cam.step(3.0, &killer, nullptr).black == doctest::Approx(0.5f));
		const gs::DeathCameraView seen = cam.step(3.5, &killer, nullptr);
		CHECK(seen.black == 0.0f);
		const gs::DeathCameraView held = cam.step(3.6, nullptr, nullptr);
		CHECK(held.active);
		CHECK(cam.active());
		CHECK(held.camera.trans.z == seen.camera.trans.z);
		CHECK(cam.stats().released_lost == 0);

		gs::DeathCameraParams none = p;
		none.has_chase = false;
		gs::Bf3DeathCamera no_camera(none);
		no_camera.begin(s, 0.0);
		CHECK(!no_camera.step(3.0, &killer, nullptr).active);
	}

	TEST_CASE("chase camera: a wall behind the killer shortens the pill toward the pivot") {
		std::vector<sim::StaticTriangle> tris;
		cam_quad(tris, { -5.0f, -1.0f, -1.0f }, { -5.0f, 5.0f, -1.0f }, { 5.0f, 5.0f, -1.0f }, { 5.0f, -1.0f, -1.0f });
		bf3::core::Result<std::shared_ptr<const sim::StaticWorldShapes>> shapes = sim::build_static_world(std::span<const sim::StaticTriangle>(tris));
		REQUIRE_MESSAGE(shapes.ok(), (shapes.ok() ? std::string() : shapes.error().message));
		bf3::core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(*shapes);
		REQUIRE_MESSAGE(world.ok(), (world.ok() ? std::string() : world.error().message));
		bf3::core::gameplay::ChaseCameraDef c = killer_chase();
		c.target_rotation_offset_deg = 0.0f;
		c.target_offset = {};
		c.wanted_angle_deg = 0.0f;
		gs::ChaseCamera walled(c);
		walled.activate(target_at({ 0.0f, 1.0f, 1.0f }), world->get());
		CHECK(walled.wanted().z > -1.0f);
		CHECK(walled.wanted().z < 1.0f);
		gs::ChaseCamera open(c);
		open.activate(target_at({ 0.0f, 1.0f, 1.0f }), nullptr);
		CHECK(open.wanted().z == doctest::Approx(1.0f - 2.1f));
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
