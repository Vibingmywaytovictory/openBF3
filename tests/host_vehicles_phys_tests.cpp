#include <doctest/doctest.h>

#include "gunplay_fixture.h"
#include "server_ghosts.h"
#include "tdm_fixture.h"
#include "vehicles/server_vehicles.h"

#include "bf3/host/level_vehicles.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/vehicle_model.h"
#include "bf3/sim/vehicle_types.h"
#include "bf3/sim/vehicle_world.h"
#include "bf3/sim/vehicles/run_over_rules.h"
#include "bf3/sim/vehicles/vehicle_obstacles.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>

using namespace bf3;
namespace tdm = bf3::test::tdm;

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
	TEST_CASE("vehicles: a run-over kills above the vehicle's KillSoldierCollisionSpeedThreshold along the contact "
			  "normal, through the material relation's CollisionDamageMultiplier above its threshold") {
		const sim::vehicles::RunOverRule rule{ 5.0f, false };
		const sim::Vec3 toward{ 1.0f, 0.0f, 0.0f };
		CHECK(sim::vehicles::run_over_speed(rule, { 6.0f, 0.0f, 0.0f }, toward) == doctest::Approx(6.0f));
		CHECK(sim::vehicles::run_over_speed(rule, { 6.0f, 0.0f, 8.0f }, toward) == doctest::Approx(6.0f));
		CHECK(sim::vehicles::run_over_speed(rule, { -6.0f, 0.0f, 0.0f }, toward) == 0.0f);
		CHECK(sim::vehicles::run_over_speed({ 5.0f, true }, { 6.0f, 0.0f, 8.0f }, toward) == doctest::Approx(10.0f));
		CHECK(sim::vehicles::run_over_damage(rule, 5.0f, 1.0f, 0.0f) == 0.0f);
		CHECK(sim::vehicles::run_over_damage(rule, 5.01f, 1.0f, 0.0f) == sim::vehicles::kRunOverKillDamage);
		CHECK(sim::vehicles::run_over_damage(rule, 1.3f, 1.0f, 0.0f) == 0.0f);
		CHECK(sim::vehicles::run_over_damage(rule, 9.0f, 0.5f, 0.0f) == doctest::Approx(500.0f));
		CHECK(sim::vehicles::run_over_damage(rule, 9.0f, 0.5f, 500.0f) == 0.0f);
		CHECK(sim::vehicles::run_over_damage(rule, 9.0f, 0.0f, 0.0f) == 0.0f);
	}

	TEST_CASE("vehicles: a step's run-over adds the hulls' damages, each hull once at its strongest contact and each "
			  "gated by its own threshold, credited to the hull that does the most") {
		const auto contact = [](uint32_t p_slot, float p_speed) {
			sim::ObstacleContact c;
			c.slot = p_slot;
			c.normal = { 1.0f, 0.0f, 0.0f };
			c.velocity = { p_speed, 0.0f, 0.0f };
			return c;
		};
		const auto hulls = [](uint32_t p_slot) -> std::optional<sim::vehicles::RunOverHull> {
			if (p_slot == 1) {
				return sim::vehicles::RunOverHull{ { 5.0f, false }, 0.04f, 0.0f };
			}
			if (p_slot == 2) {
				return sim::vehicles::RunOverHull{ { 3.0f, false }, 0.06f, 0.0f };
			}
			if (p_slot == 3) {
				return sim::vehicles::RunOverHull{ { 5.0f, false }, 0.5f, 600.0f };
			}
			return std::nullopt;
		};
		const sim::ObstacleContact one_hull[] = { contact(1, 6.0f), contact(1, 8.0f), contact(1, 6.0f) };
		const sim::vehicles::RunOverTotal once = sim::vehicles::run_over_total(one_hull, hulls);
		CHECK(once.damage == doctest::Approx(40.0f));
		REQUIRE(once.heaviest.has_value());
		CHECK(*once.heaviest == 1u);

		const sim::ObstacleContact two_hulls[] = { contact(1, 6.0f), contact(2, 4.0f), contact(1, 1.0f),
			contact(3, 9.0f), contact(4, 30.0f) };
		const sim::vehicles::RunOverTotal both = sim::vehicles::run_over_total(two_hulls, hulls);
		CHECK(both.damage == doctest::Approx(100.0f));
		REQUIRE(both.heaviest.has_value());
		CHECK(*both.heaviest == 2u);

		const sim::ObstacleContact slow[] = { contact(1, 4.0f), contact(2, -9.0f), contact(4, 30.0f) };
		const sim::vehicles::RunOverTotal none = sim::vehicles::run_over_total(slow, hulls);
		CHECK(none.damage == 0.0f);
		CHECK_FALSE(none.heaviest.has_value());
	}

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

	TEST_CASE("vehicles: a soldier left inside a parked hull comes out at its nearest face and is not launched") {
		std::unique_ptr<Walker> w = walker_on_floor();
		const float radius = w->motor.params().radius;
		const std::shared_ptr<const sim::VehicleModel> box = box_model(1.0f, 1.5f, 2.5f);
		for (const bool airborne : { false, true }) {
			for (const float inside : { 0.1f, 0.5f, 0.9f, 1.3f }) {
				w->obstacles.clear();
				sim::SoldierState s = airborne
						? w->motor.spawn_state({ 11.0f + radius - inside, 0.02f, 20.0f }, 0, 1, 1000, false)
						: standing(w->motor, 11.0f + radius - inside, 20.0f, sim::Pose::Stand);
				REQUIRE(w->obstacles.place(0, box, body_at({ 10.0f, 0.85f, 20.0f })).ok());
				float fastest = 0.0f, highest = s.position[1];
				for (int t = 0; t < 90; ++t) {
					w->motor.step(s, sim::idle_command(s.tick + 1, 0, 0, sim::Pose::Stand));
					const sim::Vec3 &v = s.velocity;
					fastest = std::max(fastest, std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
					highest = std::max(highest, s.position[1]);
				}
				std::printf(
						"inside hull: %s, %.1f m in, after 90 ticks at (%.2f, %.2f, %.2f), fastest %.2f m/s, highest "
						"%.2f m, alive %d\n",
						airborne ? "spawned" : "standing", double(inside), double(s.position[0]), double(s.position[1]),
						double(s.position[2]), double(fastest), double(highest),
						int((s.flags & sim::soldier_flags::kAlive) != 0));
				CHECK(s.position[0] >= 11.0f + radius - 0.05f);
				CHECK(s.position[0] <= 11.0f + radius + 0.3f);
				CHECK(std::fabs(s.position[2] - 20.0f) < 0.05f);
				CHECK(highest < 0.3f);
				CHECK(fastest < 1.0f);
			}
		}
	}

	TEST_CASE("vehicles: a hull pushing a soldier below its kill speed for 2 s carries him ahead of its face at its "
			  "own speed") {
		std::unique_ptr<Walker> w = walker_on_floor();
		const float radius = w->motor.params().radius;
		const std::shared_ptr<const sim::VehicleModel> box = box_model(1.0f, 1.5f, 2.5f);
		const float speed = 4.5f;
		sim::SoldierState s = standing(w->motor, 0.0f, 0.0f, sim::Pose::Stand);
		sim::VehicleBodyState b = body_at({ -(1.0f + radius + 0.5f), 0.85f, 0.0f });
		b.linear_velocity = { speed, 0.0f, 0.0f };
		float fastest = 0.0f;
		for (int t = 0; t < 60; ++t) {
			REQUIRE(w->obstacles.place(0, box, b).ok());
			w->motor.step(s, sim::idle_command(s.tick + 1, 0, 0, sim::Pose::Stand));
			fastest = std::max(fastest, std::hypot(s.velocity[0], s.velocity[2]));
			b.com_position[0] += speed * sim::kTickSeconds;
		}
		const float face = b.com_position[0] - speed * sim::kTickSeconds + 1.0f;
		std::printf("hull push: a %.1f m/s hull pushed the soldier to %.2f m, %.2f m ahead of its face, fastest "
					"%.2f m/s\n",
				double(speed), double(s.position[0]), double(s.position[0] - face), double(fastest));
		CHECK(s.position[0] > face + radius - 0.1f);
		CHECK(s.position[0] < face + radius + 0.5f);
		CHECK(std::fabs(s.position[2]) < 0.05f);
		CHECK(fastest < speed + 0.5f);
	}

	TEST_CASE("vehicles: a hull driven at a soldier at a constant speed runs him over at every start gap within one "
			  "tick's travel above KillSoldierCollisionSpeedThreshold and never below it, standing, crouched, "
			  "walking or sprinting into it") {
		std::unique_ptr<Walker> w = walker_on_floor();
		const std::shared_ptr<const sim::VehicleModel> box = box_model(1.0f, 1.5f, 2.5f);
		const sim::vehicles::RunOverRule rule{ 5.0f, false };
		const float radius = w->motor.params().radius;
		const uint16_t west = 0xC000;
		struct Victim {
			const char *name;
			sim::Pose pose;
			int8_t forward;
			uint32_t buttons;
		};
		constexpr int kOffsets = 8;
		for (const Victim v : { Victim{ "standing", sim::Pose::Stand, 0, 0 },
					 Victim{ "crouched", sim::Pose::Crouch, 0, 0 }, Victim{ "walking", sim::Pose::Stand, 127, 0 },
					 Victim{ "sprinting", sim::Pose::Stand, 127, sim::buttons::kSprint } }) {
			for (const float speed : { 4.5f, 5.5f, 8.0f, 15.0f }) {
				int kills = 0, touched = 0;
				for (int k = 0; k < kOffsets; ++k) {
					w->obstacles.clear();
					sim::SoldierState s = standing(w->motor, 0.0f, 0.0f, v.pose);
					const float travel = speed * sim::kTickSeconds;
					sim::VehicleBodyState b =
							body_at({ -(1.0f + radius + 0.3f + travel * float(k) / float(kOffsets)), 0.85f, 0.0f });
					b.linear_velocity = { speed, 0.0f, 0.0f };
					REQUIRE(w->obstacles.place(0, box, b).ok());
					bool killed = false, touch = false;
					for (int t = 0; t < 40 && !killed; ++t) {
						w->motor.step(s, toward(s.tick + 1, west, v.forward, v.buttons, v.pose));
						for (const sim::ObstacleContact &c : w->motor.obstacle_contacts()) {
							touch = true;
							killed = killed ||
									sim::vehicles::run_over_speed(rule, c.velocity, c.normal) > rule.kill_speed;
						}
						b.com_position[0] += travel;
						REQUIRE(w->obstacles.place(0, box, b).ok());
					}
					kills += killed ? 1 : 0;
					touched += touch ? 1 : 0;
				}
				std::printf("run-over sweep: %s, %.1f m/s: %d of %d start gaps touched, %d killed\n", v.name,
						double(speed), touched, kOffsets, kills);
				CHECK(touched == kOffsets);
				CHECK(kills == (speed > rule.kill_speed ? kOffsets : 0));
			}
		}
	}
}

TEST_SUITE("host-vehicles-wreck-scope") {
	TEST_CASE("vehicles: MP_007 ConquestSmall0 with ghost scoping on: a wreck's exploded mark reaches a viewer who "
			  "holds the vehicle out of scope, every snapshot of the kWreckWireTicks after its explosion and no "
			  "other") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		const host::LevelVehicles &lv = *level->shared->vehicles;
		std::optional<size_t> target;
		for (size_t i = 0; i < lv.spawners.size() && !target; ++i) {
			const host::LevelVehicleKind &k = lv.kinds[lv.spawners[i].kind];
			if (k.blueprint == "vehicles/growleritv/growleritv" && k.damage.upside_down_damage > 0.0f) {
				target = i;
			}
		}
		REQUIRE(target.has_value());
		auto vehicles = std::make_shared<host::LevelVehicles>(lv);
		host::LevelVehicleSpawner &rolled = vehicles->spawners[*target];
		rolled.placement.position[1] += 2.0f;
		rolled.placement.rotation = { 0.0f, 0.0f, 1.0f, 0.0f };
		rolled.initial_spawn_delay = 0.0f;
		host::ServerVehicles sv;
		REQUIRE(sv.configure(vehicles, level->shared->static_world, 1.0f).ok());
		sv.configure_scope(1, true);
		host::GhostViewer far;
		far.spawned = true;
		far.position = { rolled.placement.position[0] + 5000.0f, rolled.placement.position[1],
			rolled.placement.position[2] };
		far.eye = far.position;
		uint32_t before = 0, marked = 0, after = 0;
		std::optional<sim::Tick> exploded;
		for (sim::Tick t = 1; t < 6000 && (!exploded || t < *exploded + 2u * host::kWreckWireTicks); ++t) {
			REQUIRE(sv.step(t, 0, [](uint8_t) { return true; }).ok());
			sv.step_health(t, {});
			sv.spawn_due(t, nullptr);
			if (!exploded && sv.stats().exploded > 0) {
				exploded = t;
			}
			for (const net::VehicleStates &m : sv.wire_for(0, far, t)) {
				for (const net::VehicleWire &w : m.vehicles) {
					if (w.spawner != *target) {
						continue;
					}
					if (!exploded) {
						++before;
					} else if (w.exploded && t < *exploded + host::kWreckWireTicks) {
						++marked;
					} else {
						++after;
					}
				}
			}
		}
		REQUIRE(exploded.has_value());
		std::printf(
				"wreck scope: the rolled Growler of spawner %zu exploded at tick %u; the out-of-scope viewer got it "
				"%u times before, %u times with the exploded mark, %u times after; scope held %llu, out of scope "
				"%llu\n",
				*target, unsigned(*exploded), before, marked, after, (unsigned long long)sv.scope_stats().held,
				(unsigned long long)sv.scope_stats().out_of_scope);
		CHECK(before == 0u);
		CHECK(marked == host::kWreckWireTicks);
		CHECK(after == 0u);
		CHECK(sv.scope_stats().out_of_scope > 0u);
	}
}

TEST_SUITE("host-vehicles-exit") {
	TEST_CASE("vehicles: MP_007 ConquestSmall0: a Growler driver whose nearest exit point lies inside a parked "
			  "Growler gets out at the next exit point where he stands clear of every hull and the level") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		const host::LevelVehicles &lv = *level->shared->vehicles;
		std::optional<size_t> jeep;
		for (size_t i = 0; i < lv.spawners.size() && !jeep; ++i) {
			const host::LevelVehicleKind &k = lv.kinds[lv.spawners[i].kind];
			if (k.blueprint == "vehicles/growleritv/growleritv" && k.exit_points.size() >= 2 && k.seats >= 1) {
				jeep = i;
			}
		}
		REQUIRE(jeep.has_value());
		const host::LevelVehicleKind &kind = lv.kinds[lv.spawners[*jeep].kind];
		auto vehicles = std::make_shared<host::LevelVehicles>(lv);
		host::LevelVehicleSpawner parked = lv.spawners[*jeep];
		parked.initial_spawn_delay = 0.0f;
		const auto turn_by = [](const std::array<float, 4> &p_q, const sim::Vec3 &p_v) {
			const float x = p_q[0], y = p_q[1], z = p_q[2], w = p_q[3];
			const float tx = 2.0f * (y * p_v[2] - z * p_v[1]), ty = 2.0f * (z * p_v[0] - x * p_v[2]),
						tz = 2.0f * (x * p_v[1] - y * p_v[0]);
			return sim::Vec3{ p_v[0] + w * tx + (y * tz - z * ty), p_v[1] + w * ty + (z * tx - x * tz),
				p_v[2] + w * tz + (x * ty - y * tx) };
		};
		const core::Vec3 nearest = *host::vehicle_exit_point(kind, 0);
		const float lateral = std::sqrt(nearest[0] * nearest[0] + nearest[2] * nearest[2]);
		REQUIRE(lateral > 0.5f);
		const float reach = lateral + kind.box.half[0] - 0.1f;
		const sim::Vec3 out = turn_by(
				parked.placement.rotation, { nearest[0] / lateral * reach, 0.0f, nearest[2] / lateral * reach });
		host::LevelVehicleSpawner blocker = parked;
		blocker.placement.position = { parked.placement.position[0] + out[0], parked.placement.position[1] + 0.3f,
			parked.placement.position[2] + out[2] };
		vehicles->spawners = { parked, blocker };
		host::ServerVehicles sv;
		REQUIRE(sv.configure(vehicles, level->shared->static_world, 1.0f).ok());
		for (sim::Tick t = 1; t <= 120; ++t) {
			sv.spawn_due(t, nullptr);
			REQUIRE(sv.step(t, 0, [](uint8_t) { return true; }).ok());
		}
		REQUIRE(sv.vehicles()[0].live);
		REQUIRE(sv.vehicles()[1].live);

		core::Result<std::unique_ptr<sim::CollisionWorld>> world =
				sim::CollisionWorld::create(level->shared->static_world);
		REQUIRE(world.ok());
		sim::SoldierMotor motor;
		REQUIRE(motor.init(**world, level->shared->motor).ok());
		const sim::Vec3 centre = sv.world()->state(sim::VehicleSlot(0)).body.com_position;
		sim::SoldierState s = motor.spawn_state(centre, 0, 1, 1000, false);
		const auto press = [&](sim::Tick p_tick, bool p_use) {
			sim::Command c = sim::idle_command(p_tick, 0, 0, sim::Pose::Stand);
			c.buttons = p_use ? sim::buttons::kUse : 0u;
			return sv.use(0, c, s, motor);
		};
		CHECK_FALSE(press(121, true).has_value());
		REQUIRE(sv.seated(0));
		CHECK_FALSE(press(122, false).has_value());
		const sim::VehicleBodyState body = sv.world()->state(sim::VehicleSlot(0)).body;
		const std::optional<sim::Vec3> exited = press(123, true);
		REQUIRE(exited.has_value());
		CHECK_FALSE(sv.seated(0));
		const sim::MotorParams &p = motor.params();
		const float pad = p.tuning.character_padding;
		const float height = p.poses[size_t(sim::Pose::Stand)].height;
		const sim::Vec3 first = [&] {
			const sim::Vec3 r = turn_by(body.rotation, nearest);
			return sim::Vec3{ body.origin[0] + r[0], body.origin[1] + r[1], body.origin[2] + r[2] };
		}();
		const bool first_blocked = sv.obstacles().overlaps_capsule(first, p.radius + pad, height + 2.0f * pad);
		const bool clear = !sv.obstacles().overlaps_capsule(*exited, p.radius + pad, height + 2.0f * pad);
		std::printf("exit: Growler of spawner %zu, %zu exit points, the nearest at (%.2f %.2f %.2f) is %s; the driver "
					"got out at (%.2f %.2f %.2f), %.2f m from it, clear of the hulls %d, fits the level %d\n",
				*jeep, kind.exit_points.size(), double(nearest[0]), double(nearest[1]), double(nearest[2]),
				first_blocked ? "inside the parked Growler" : "clear", double((*exited)[0]), double((*exited)[1]),
				double((*exited)[2]), double(std::hypot((*exited)[0] - first[0], (*exited)[2] - first[2])), int(clear),
				int(motor.fits_standing(*exited)));
		REQUIRE(first_blocked);
		CHECK(clear);
		CHECK(motor.fits_standing(*exited));
	}
}
