#include "sim_world_support.h"

#include "bf3/sim/weapons/gadgets.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <vector>

namespace {

using namespace bf3;
using namespace bf3::sim;
using namespace bf3::sim::weapons;

constexpr uint16_t kMedkitDef = 3;
constexpr uint16_t kAmmoDef = 4;
constexpr uint16_t kBeaconDef = 5;

GadgetDef medkit_def() {
	GadgetDef g;
	g.weapon = "medkit";
	g.kind = GadgetKind::Supply;
	g.def = kMedkitDef;
	g.healing = SupplyUnitDef{ 3.5f, 12.0f, 1.0f, 0.0f, true };
	g.supply_soldiers = true;
	g.max_count = 1;
	g.max_attach_inclination_deg = 25.0f;
	return g;
}

GadgetDef ammo_def() {
	GadgetDef g = medkit_def();
	g.weapon = "ammo bag";
	g.def = kAmmoDef;
	g.healing = SupplyUnitDef();
	g.ammo = SupplyUnitDef{ 3.5f, 1.0f, 7.0f, 0.0f, true };
	return g;
}

GadgetDef beacon_def() {
	GadgetDef g;
	g.weapon = "beacon";
	g.kind = GadgetKind::SpawnBeacon;
	g.def = kBeaconDef;
	g.ground_ray_m = 3.1f;
	g.ground_flatness = 0.5f;
	return g;
}

std::unique_ptr<CollisionWorld> floor_and_wall() {
	std::vector<StaticTriangle> t;
	sim_world_test::add_floor(t, -20.0f, -20.0f, 20.0f, 20.0f, 0.0f);
	sim_world_test::add_wall_x(t, 2.0f, -20.0f, 20.0f, -1.0f, 6.0f);
	return sim_world_test::world_of(sim_world_test::build(t));
}

GadgetThrow throw_of(
		uint16_t p_def, uint8_t p_owner, const Vec3 &p_origin, const Vec3 &p_direction, const Vec3 &p_velocity) {
	GadgetThrow t;
	t.def = p_def;
	t.owner = p_owner;
	t.team = 1;
	t.tick = 100;
	t.origin = p_origin;
	t.direction = p_direction;
	t.launch_velocity = p_velocity;
	return t;
}

bool settle(GadgetField &r_field, const CollisionWorld &p_world, uint32_t p_ticks) {
	for (uint32_t i = 0; i < p_ticks; ++i) {
		r_field.step(p_world);
		bool all = true;
		for (const Deployable &d : r_field.live()) {
			all = all && d.resting;
		}
		if (all) {
			return true;
		}
	}
	return false;
}

}

TEST_SUITE("gadgets-unit") {
	TEST_CASE("shot frame: the data's launch vector turns with the aim, z forward and y up") {
		const Vec3 v = shot_frame_vector(Vec3{ 1.0f, 0.0f, 0.0f }, Vec3{ 0.0f, 0.0f, 5.0f });
		CHECK(v[0] == doctest::Approx(5.0f));
		CHECK(std::fabs(v[1]) < 1e-5f);
		CHECK(std::fabs(v[2]) < 1e-5f);
		const Vec3 u = shot_frame_vector(Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{ 0.0f, 0.2f, 0.0f });
		CHECK(u[1] == doctest::Approx(0.2f));
	}

	TEST_CASE("throw: a thrown bag falls on the floor and rests; one into a steep wall slides down to the floor") {
		std::unique_ptr<CollisionWorld> w = floor_and_wall();
		GadgetField field({ medkit_def() });
		CHECK(field.deploy(throw_of(kMedkitDef, 1, Vec3{ 0.0f, 1.6f, 0.0f }, Vec3{ 0.0f, 0.0f, 1.0f },
								   Vec3{ 0.0f, 0.0f, 5.0f }),
					  w.get()) == DeployOutcome::Thrown);
		REQUIRE(settle(field, *w, 120));
		REQUIRE(field.live().size() == 1u);
		const Deployable &d = field.live()[0];
		CHECK(std::fabs(d.position[1]) < 0.02f);
		const float t = std::sqrt(2.0f * 1.6f / kGadgetGravity);
		CHECK(d.position[2] == doctest::Approx(5.0f * t).epsilon(0.05));

		GadgetField wall({ medkit_def() });
		CHECK(wall.deploy(throw_of(kMedkitDef, 1, Vec3{ 0.0f, 1.6f, 0.0f }, Vec3{ 1.0f, 0.0f, 0.0f },
								  Vec3{ 0.0f, 0.0f, 10.0f }),
					  w.get()) == DeployOutcome::Thrown);
		REQUIRE(settle(wall, *w, 240));
		CHECK(wall.stats().slides > 0u);
		CHECK(wall.live()[0].position[0] < 2.0f);
		CHECK(std::fabs(wall.live()[0].position[1]) < 0.02f);
	}

	TEST_CASE("throw: MaxCount 1 keeps the newest bag of an owner; other owners keep theirs; the flight ends after 10 "
			  "s") {
		std::unique_ptr<CollisionWorld> w = floor_and_wall();
		GadgetField field({ medkit_def() });
		uint16_t first = 0, second = 0, other = 0;
		field.deploy(
				throw_of(kMedkitDef, 1, Vec3{ 0.0f, 1.0f, 0.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), w.get(), &first);
		field.deploy(
				throw_of(kMedkitDef, 2, Vec3{ 0.0f, 1.0f, -3.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), w.get(), &other);
		field.deploy(
				throw_of(kMedkitDef, 1, Vec3{ 0.0f, 1.0f, -5.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), w.get(), &second);
		CHECK(field.live().size() == 2u);
		CHECK(field.find(first) == nullptr);
		CHECK(field.find(second) != nullptr);
		CHECK(field.find(other) != nullptr);
		CHECK(field.stats().replaced == 1u);
		CHECK(field.remove_owner(2) == 1u);

		std::vector<StaticTriangle> none;
		sim_world_test::add_floor(none, 100.0f, 100.0f, 101.0f, 101.0f, 0.0f);
		std::unique_ptr<CollisionWorld> empty = sim_world_test::world_of(sim_world_test::build(none));
		GadgetField falling({ medkit_def() });
		falling.deploy(
				throw_of(kMedkitDef, 1, Vec3{ 0.0f, 1.0f, 0.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), empty.get());
		for (Tick i = 0; i < kGadgetFlightTicks + 1u; ++i) {
			falling.step(*empty);
		}
		CHECK(falling.live().empty());
		CHECK(falling.stats().lost == 1u);
	}

	TEST_CASE("supply: a resting medkit heals at its SupplyIncSpeed inside its radius, the best bag only, by the team "
			  "and self "
			  "rules") {
		std::unique_ptr<CollisionWorld> w = floor_and_wall();
		GadgetDef strict = medkit_def();
		strict.def = 9;
		strict.team_specific = true;
		strict.exclude_self = true;
		strict.healing.inc_speed = 20.0f;
		GadgetField field({ medkit_def(), strict });
		field.deploy(throw_of(kMedkitDef, 1, Vec3{ 0.0f, 0.5f, 0.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), w.get());
		std::array<SupplyReceiver, 3> r{};
		r[0] = SupplyReceiver{ 1, 1, true, Vec3{ 3.0f * 0.5f, 0.0f, 0.0f } };
		r[1] = SupplyReceiver{ 2, 2, true, Vec3{ 0.0f, 0.0f, 3.4f } };
		r[2] = SupplyReceiver{ 3, 1, true, Vec3{ 0.0f, 0.0f, 3.6f } };
		std::array<SupplyGrant, 3> g{};
		field.supply(r, kTickSeconds, g);
		CHECK(g[0].health == 0.0f);
		REQUIRE(settle(field, *w, 60));
		field.supply(r, kTickSeconds, g);
		CHECK(g[0].health == doctest::Approx(12.0f / 60.0f));
		CHECK(g[1].health == doctest::Approx(12.0f / 60.0f));
		CHECK(g[2].health == 0.0f);
		CHECK(g[0].ammo == 0.0f);

		field.deploy(throw_of(9, 1, Vec3{ 0.0f, 0.5f, 0.5f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), w.get());
		REQUIRE(settle(field, *w, 60));
		r[0].slot = 4;
		field.supply(r, 1.0f, g);
		CHECK(g[0].health == doctest::Approx(20.0f));
		CHECK(g[1].health == doctest::Approx(12.0f));
		r[0].slot = 1;
		field.supply(r, 1.0f, g);
		CHECK(g[0].health == doctest::Approx(12.0f));
	}

	TEST_CASE("supply: a finite bag gives at most its points and refills at SupplyPointsRefillSpeed") {
		std::unique_ptr<CollisionWorld> w = floor_and_wall();
		GadgetDef finite = ammo_def();
		finite.ammo.infinite = false;
		finite.ammo.capacity = 2.0f;
		finite.ammo.refill_speed = 0.5f;
		GadgetField field({ finite });
		field.deploy(throw_of(kAmmoDef, 1, Vec3{ 0.0f, 0.5f, 0.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), w.get());
		REQUIRE(settle(field, *w, 60));
		std::array<SupplyReceiver, 1> r{ SupplyReceiver{ 2, 1, true, Vec3{ 1.0f, 0.0f, 0.0f } } };
		std::array<SupplyGrant, 1> g{};
		field.supply(r, 1.5f, g);
		CHECK(g[0].ammo == doctest::Approx(1.5f));
		field.supply(r, 1.5f, g);
		CHECK(g[0].ammo == doctest::Approx(0.5f + 0.75f));
		r[0].alive = false;
		field.supply(r, 1.0f, g);
		CHECK(g[0].ammo == 0.0f);
	}

	TEST_CASE("beacon: placed on the ground under the deploy point within the ray length; refused with no ground") {
		std::unique_ptr<CollisionWorld> w = floor_and_wall();
		GadgetField field({ beacon_def() });
		GadgetThrow t = throw_of(kBeaconDef, 1, Vec3{ 0.0f, 1.6f, 0.2f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{});
		CHECK(field.deploy(t, w.get()) == DeployOutcome::Placed);
		REQUIRE(field.live().size() == 1u);
		CHECK(field.live()[0].resting);
		CHECK(std::fabs(field.live()[0].position[1]) < 1e-3f);
		CHECK(field.live()[0].position[2] == doctest::Approx(0.2f));
		t.origin = Vec3{ 0.0f, 3.5f, 0.0f };
		CHECK(field.deploy(t, w.get()) == DeployOutcome::NoGround);
		CHECK(field.deploy(t, nullptr) == DeployOutcome::NoGround);
		CHECK(field.live().size() == 1u);
		GadgetThrow not_gadget = t;
		not_gadget.def = 77;
		CHECK(field.deploy(not_gadget, w.get()) == DeployOutcome::NotGadget);
	}

	TEST_CASE("ammo: supply points refill the reserve one pickup per AmmoBagPickupDelayMultiplier, up to the spawn "
			  "reserve") {
		WeaponTable table;
		table.defs.resize(2);
		table.defs[0].magazine_capacity = 30;
		table.defs[0].magazines = 4;
		table.defs[1].magazine_capacity = 1;
		table.defs[1].magazines = 1;
		std::vector<AmmoPickupDef> pickups(2);
		pickups[1].delay_multiplier = 2.5f;
		SoldierWeaponsState w;
		w.slots[0].def = 0;
		w.slots[0].reserve = 120;
		w.slots[5].def = 1;
		w.slots[5].reserve = 1;
		AmmoRefillState s;
		arm_ammo_refill(s, w);
		CHECK(s.caps[0] == 120);
		CHECK(s.caps[5] == 1);
		w.slots[0].reserve = 50;
		w.slots[5].reserve = 0;
		CHECK(refill_ammo(w, table, pickups, 0.5f, s) == 0);
		CHECK(refill_ammo(w, table, pickups, 0.5f, s) == 30);
		CHECK(w.slots[0].reserve == 80);
		CHECK(refill_ammo(w, table, pickups, 1.0f, s) == 30);
		CHECK(refill_ammo(w, table, pickups, 1.0f, s) == 10 + 1);
		CHECK(w.slots[0].reserve == 120);
		CHECK(w.slots[5].reserve == 1);
		CHECK(refill_ammo(w, table, pickups, 1.0f, s) == 0);
	}

	TEST_CASE("ammo: AutoReplenishMagazine restores the spawn reserve AutoReplenishDelay after it dropped") {
		WeaponTable table;
		table.defs.resize(1);
		table.defs[0].magazine_capacity = 1;
		table.defs[0].magazines = 1;
		std::vector<AmmoPickupDef> pickups(1);
		pickups[0].auto_replenish = true;
		pickups[0].auto_replenish_delay = 1.0f;
		SoldierWeaponsState w;
		w.slots[4].def = 0;
		w.slots[4].reserve = 1;
		AmmoRefillState s;
		arm_ammo_refill(s, w);
		w.slots[4].reserve = 0;
		Tick t = 500;
		CHECK(replenish_ammo(w, table, pickups, t, s) == 0);
		for (Tick i = 1; i < kSimHz; ++i) {
			CHECK(replenish_ammo(w, table, pickups, t + i, s) == 0);
		}
		CHECK(replenish_ammo(w, table, pickups, t + kSimHz, s) == 1);
		CHECK(w.slots[4].reserve == 1);
		pickups[0].auto_replenish = false;
		w.slots[4].reserve = 0;
		for (Tick i = 0; i < 3u * kSimHz; ++i) {
			replenish_ammo(w, table, pickups, t + 100u + i, s);
		}
		CHECK(w.slots[4].reserve == 0);
	}
}
