#include <doctest/doctest.h>

#include "gunplay_fixture.h"

#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/vehicle_model.h"
#include "bf3/sim/vehicle_types.h"
#include "bf3/sim/vehicle_world.h"
#include "bf3/sim/vehicles/vehicle_obstacles.h"

#include <cmath>
#include <memory>

using namespace bf3;

namespace {

std::shared_ptr<const sim::VehicleModel> box_model(float p_half_x, float p_height, float p_half_z) {
	auto m = std::make_shared<sim::VehicleModel>();
	sim::ChassisPiece piece;
	for (const float x : { -p_half_x, p_half_x }) {
		for (const float y : { 0.0f, p_height }) {
			for (const float z : { -p_half_z, p_half_z }) {
				piece.points.push_back({ x, y, z });
			}
		}
	}
	piece.convex_radius = 0.05f;
	m->chassis.push_back(piece);
	m->center_of_mass = { 0.0f, 0.5f * p_height, 0.0f };
	m->mass = 2000.0f;
	return m;
}

sim::VehicleBodyState body_at(const sim::Vec3 &p_com) {
	sim::VehicleBodyState b;
	b.com_position = p_com;
	return b;
}

struct Walker {
	std::unique_ptr<sim::CollisionWorld> world;
	sim::SoldierMotor motor;
	sim::VehicleObstacles obstacles;
};

std::unique_ptr<Walker> walker_on_floor() {
	const std::shared_ptr<const host::SharedLevel> level = test::gunplay::synthetic_floor();
	REQUIRE(level != nullptr);
	core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(level->static_world);
	REQUIRE(world.ok());
	auto w = std::make_unique<Walker>();
	w->world = std::move(*world);
	REQUIRE(w->motor.init(*w->world, level->motor).ok());
	w->motor.set_obstacles(&w->obstacles);
	return w;
}

sim::SoldierState standing(const sim::SoldierMotor &p_motor, float p_x, float p_z, sim::Pose p_pose) {
	sim::SoldierState s =
			p_motor.spawn_state({ p_x, p_motor.params().tuning.character_padding, p_z }, 0, 1, 1000, false);
	for (int i = 0; i < 90; ++i) {
		p_motor.step(s, sim::idle_command(s.tick + 1, 0, 0, p_pose));
	}
	return s;
}

sim::Command toward(sim::Tick p_tick, uint16_t p_yaw, int8_t p_forward, uint32_t p_buttons, sim::Pose p_pose) {
	sim::Command c = sim::idle_command(p_tick, p_yaw, 0, p_pose);
	c.move_forward = p_forward;
	c.buttons = p_buttons;
	return c;
}

}

TEST_SUITE("host-vehicles-phys-unit") {
	TEST_CASE("vehicles: VehicleObstacles places a chassis at its body state; a soldier's motor step into it reports "
			  "the hull's slot, the normal out of the hull and the hull's velocity at the contact") {
		sim::VehicleObstacles o;
		CHECK(o.count() == 0u);
		CHECK_FALSE(o.place(0, nullptr, body_at({})).ok());
		CHECK_FALSE(o.place(sim::kMaxVehicleSlots, box_model(1.0f, 1.5f, 2.5f), body_at({})).ok());
		sim::VehicleBodyState nan = body_at({});
		nan.linear_velocity[1] = NAN;
		CHECK_FALSE(o.place(0, box_model(1.0f, 1.5f, 2.5f), nan).ok());

		std::unique_ptr<Walker> w = walker_on_floor();
		const float radius = w->motor.params().radius;
		const sim::SoldierState far = standing(w->motor, 13.5f, 20.0f, sim::Pose::Stand);
		const sim::SoldierState near = standing(w->motor, 11.0f + radius + 0.05f, 20.0f, sim::Pose::Stand);
		const sim::SoldierState beyond = standing(w->motor, 12.5f + radius + 0.15f, 20.0f, sim::Pose::Stand);
		const std::shared_ptr<const sim::VehicleModel> box = box_model(1.0f, 1.5f, 2.5f);
		sim::VehicleBodyState b = body_at({ 10.0f, 0.85f, 20.0f });
		b.linear_velocity = { 3.0f, 0.0f, 0.0f };
		b.angular_velocity = { 0.0f, 1.0f, 0.0f };
		REQUIRE(w->obstacles.place(4, box, b).ok());
		CHECK(w->obstacles.count() == 1u);
		CHECK(w->obstacles.holds(4));
		CHECK_FALSE(w->obstacles.holds(3));

		const uint16_t west = 0xC000;
		const auto step_west = [&](sim::SoldierState p_s) {
			w->motor.step(p_s, toward(p_s.tick + 1, west, 127, 0, sim::Pose::Stand));
			return p_s;
		};
		const auto walk_west = [&](sim::SoldierState p_s) {
			for (int i = 0; i < 20; ++i) {
				w->motor.step(p_s, toward(p_s.tick + 1, west, 127, 0, sim::Pose::Stand));
				if (!w->motor.obstacle_contacts().empty()) {
					break;
				}
			}
			return p_s;
		};
		step_west(far);
		CHECK(w->motor.obstacle_contacts().empty());
		const sim::SoldierState pushed = step_west(near);
		REQUIRE(w->motor.obstacle_contacts().size() >= 1u);
		const sim::ObstacleContact side = w->motor.obstacle_contacts()[0];
		CHECK(side.slot == 4u);
		CHECK(side.normal[0] == doctest::Approx(1.0f).epsilon(0.01));
		CHECK(std::fabs(side.normal[1]) < 0.01f);
		CHECK(side.position[0] == doctest::Approx(11.0f).epsilon(0.01));
		const float arm_x = side.position[0] - 10.0f, arm_z = side.position[2] - 20.0f;
		CHECK(side.velocity[0] == doctest::Approx(3.0f + arm_z).epsilon(0.01));
		CHECK(side.velocity[2] == doctest::Approx(-arm_x).epsilon(0.01));
		CHECK(pushed.position[0] > near.position[0]);

		const float s = std::sqrt(0.5f);
		b.rotation = { 0.0f, s, 0.0f, s };
		b.linear_velocity = {};
		b.angular_velocity = {};
		walk_west(beyond);
		CHECK(w->motor.obstacle_contacts().empty());
		REQUIRE(w->obstacles.place(4, box, b).ok());
		walk_west(beyond);
		REQUIRE(w->motor.obstacle_contacts().size() >= 1u);
		CHECK(w->motor.obstacle_contacts()[0].normal[0] == doctest::Approx(1.0f).epsilon(0.01));

		w->obstacles.remove(4);
		CHECK(w->obstacles.count() == 0u);
		walk_west(beyond);
		CHECK(w->motor.obstacle_contacts().empty());
		REQUIRE(w->obstacles.place(4, box, b).ok());
		CHECK(w->obstacles.count() == 1u);
		w->obstacles.clear();
		CHECK(w->obstacles.count() == 0u);
		w->motor.set_obstacles(nullptr);
		REQUIRE(w->obstacles.place(4, box, b).ok());
		walk_west(beyond);
		CHECK(w->motor.obstacle_contacts().empty());
	}
}
