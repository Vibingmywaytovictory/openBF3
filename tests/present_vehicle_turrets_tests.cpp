#include <doctest/doctest.h>

#include "bf3/core/gameplay/vehicle_def.h"
#include "bf3/present/vehicle_turrets.h"

#include <array>
#include <cmath>
#include <vector>

using namespace bf3;

namespace {

core::gameplay::VehicleTurretDef turret(int p_parent, int p_axis, const core::Vec3 &p_at) {
	core::gameplay::VehicleTurretDef t;
	t.parent = p_parent;
	t.frame_turret = p_parent;
	t.rotation_axis = p_axis;
	t.mass = 1.0f;
	t.inertia = 1.0f;
	t.frame.trans = p_at;
	return t;
}

sim::vehicles::VehicleArmament tank() {
	core::gameplay::VehicleDef d;
	d.blueprint = "vehicles/test/tank";
	d.turrets.push_back(turret(-1, 1, { 0.0f, 1.5f, 0.0f }));
	d.turrets.push_back(turret(0, 0, { 0.0f, 0.3f, 1.0f }));
	core::gameplay::VehicleTurretDef side = turret(-1, 1, { 1.0f, 2.0f, -1.0f });
	side.frame.right = { 0.0f, 0.0f, -1.0f };
	side.frame.forward = { 1.0f, 0.0f, 0.0f };
	d.turrets.push_back(side);
	core::Result<sim::vehicles::VehicleArmament> a = sim::vehicles::build_vehicle_armament(d);
	REQUIRE(a.ok());
	return *a;
}

void check_near(const core::Vec3 &p_a, const core::Vec3 &p_b) {
	for (size_t k = 0; k < 3; ++k) {
		CHECK(p_a[k] == doctest::Approx(p_b[k]).epsilon(1e-4));
	}
}

}

TEST_SUITE("present-vehicle-turrets-unit") {
	TEST_CASE("turret offsets: nothing moves at zero angles") {
		const sim::vehicles::VehicleArmament a = tank();
		const std::vector<core::Transform> o = present::turret_offsets(a, std::array<float, 3>{});
		REQUIRE(o.size() == 3);
		for (const core::Transform &t : o) {
			check_near(t.transform_point({ 0.3f, 2.0f, 4.0f }), { 0.3f, 2.0f, 4.0f });
		}
	}

	TEST_CASE("turret offsets: a quarter turn of the ring swings the barrel tip from ahead to the right") {
		const sim::vehicles::VehicleArmament a = tank();
		const float half_pi = 1.57079633f;
		const std::vector<core::Transform> o = present::turret_offsets(a, std::array<float, 3>{ half_pi, 0.0f, 0.0f });
		check_near(o[0].transform_point({ 0.0f, 1.8f, 4.0f }), { 4.0f, 1.8f, 0.0f });
		check_near(o[1].transform_point({ 0.0f, 1.8f, 4.0f }), { 4.0f, 1.8f, 0.0f });
		check_near(o[2].transform_point({ 0.0f, 1.8f, 4.0f }), { 0.0f, 1.8f, 4.0f });
	}

	TEST_CASE("turret offsets: every turret's drawn frame is the simulation's turret_world at the same angles") {
		const sim::vehicles::VehicleArmament a = tank();
		const std::array<float, 3> angles{ 0.7f, -0.2f, 1.1f };
		const std::vector<core::Transform> o = present::turret_offsets(a, angles);
		sim::vehicles::ArmamentState rest{};
		sim::vehicles::ArmamentState turned{};
		for (size_t i = 0; i < angles.size(); ++i) {
			turned.bodies[i].angle = angles[i];
		}
		for (size_t i = 0; i < a.turrets.size(); ++i) {
			const core::Transform at_rest = sim::vehicles::turret_world(a, rest, core::Transform::identity(), i);
			const core::Transform now = sim::vehicles::turret_world(a, turned, core::Transform::identity(), i);
			for (const core::Vec3 &p : { core::Vec3{ 0.0f, 0.0f, 0.0f }, core::Vec3{ 0.5f, -0.2f, 3.0f },
						 core::Vec3{ -1.0f, 1.0f, 0.25f } }) {
				check_near(o[i].transform_point(at_rest.transform_point(p)), now.transform_point(p));
			}
		}
	}
}
