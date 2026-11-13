#include "sim_world_support.h"

#include "bf3/core/math.h"
#include "bf3/sim/weapons/damage.h"
#include "bf3/sim/weapons/gadgets.h"
#include "bf3/sim/weapons/guidance.h"
#include "bf3/sim/weapons/locking.h"
#include "bf3/sim/weapons/spread_recoil.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <optional>
#include <span>
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

GadgetDef claymore_def() {
	GadgetDef g;
	g.weapon = "claymore";
	g.kind = GadgetKind::ExplosionPack;
	g.def = 6;
	g.max_count = 2;
	g.max_attach_inclination_deg = 45.0f;
	g.health = 50.0f;
	g.destructible = true;
	g.receives_explosion_damage = true;
	g.time_to_live_on_owner_gone_s = 1.0f;
	g.pack.soldier_detonation = true;
	g.pack.soldier_radius = 3.0f;
	g.pack.soldier_angle_deg = 60.0f;
	g.pack.soldier_min_speed = 0.5f;
	g.pack.soldier_delay_s = 3.0f;
	g.pack.use_angle = true;
	return g;
}

GadgetDef c4_def() {
	GadgetDef g = claymore_def();
	g.weapon = "c4";
	g.def = 7;
	g.max_count = 6;
	g.max_attach_inclination_deg = 180.0f;
	g.time_to_live_on_owner_gone_s = 0.0f;
	g.pack = ExplosionPackDef();
	g.pack.remote_detonator = true;
	return g;
}

GadgetDef mine_def() {
	GadgetDef g = claymore_def();
	g.weapon = "m15";
	g.def = 8;
	g.max_count = 6;
	g.pack = ExplosionPackDef();
	g.pack.vehicle_detonation = true;
	g.pack.vehicle_radius = 2.0f;
	g.pack.vehicle_delay_s = 4.0f;
	return g;
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

	TEST_CASE("life: a bag goes TimeToLive after its deploy and TimeToLiveOnPlayerDeath after its owner left; 0 "
			  "keeps it; a bag whose owner left is not the slot's next owner's bag") {
		std::unique_ptr<CollisionWorld> w = floor_and_wall();
		GadgetDef timed = medkit_def();
		timed.time_to_live_s = 2.0f;
		GadgetDef forever = medkit_def();
		forever.def = 9;
		GadgetDef orphan_timed = medkit_def();
		orphan_timed.def = 10;
		orphan_timed.time_to_live_on_owner_gone_s = 1.0f;
		GadgetField field({ timed, forever, orphan_timed, beacon_def() });
		uint16_t a = 0, b = 0, c = 0, beacon = 0;
		field.deploy(throw_of(kMedkitDef, 1, Vec3{ 0.0f, 1.0f, 0.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), w.get(), &a);
		field.deploy(throw_of(9, 2, Vec3{ 0.0f, 1.0f, -3.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), w.get(), &b);
		field.deploy(throw_of(10, 2, Vec3{ 0.0f, 1.0f, -6.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), w.get(), &c);
		field.deploy(
				throw_of(kBeaconDef, 2, Vec3{ -3.0f, 1.0f, 0.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), w.get(), &beacon);
		REQUIRE(settle(field, *w, 120));
		REQUIRE(field.live().size() == 4u);
		const Tick deployed = 100;
		const Tick two_s = 2u * kSimHz;
		CHECK(field.expire(deployed + two_s) == 0u);
		CHECK(field.find(a) != nullptr);
		CHECK(field.expire(deployed + two_s + 1u) == 1u);
		CHECK(field.find(a) == nullptr);

		CHECK(field.owner_gone(2, 1000) == 1u);
		CHECK(field.find(beacon) == nullptr);
		CHECK(field.stats().orphaned == 2u);
		CHECK(field.expire(1000 + kSimHz) == 0u);
		CHECK(field.expire(1000 + kSimHz + 1u) == 1u);
		CHECK(field.find(c) == nullptr);
		CHECK(field.expire(1000000) == 0u);
		REQUIRE(field.find(b) != nullptr);
		CHECK(field.find(b)->owner_gone);
		CHECK(field.stats().expired == 2u);

		CHECK(field.remove_owner(2) == 0u);
		uint16_t next = 0;
		field.deploy(throw_of(9, 2, Vec3{ 3.0f, 1.0f, 0.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), w.get(), &next);
		CHECK(field.find(b) != nullptr);
		CHECK(field.find(next) != nullptr);
		CHECK(field.stats().replaced == 0u);
	}

	TEST_CASE("damage: a destructible bag loses its Health per hit and goes at 0; IsDestructible and "
			  "ReceivesExplosionDamage gate it; a blast in its radius destroys it, one behind a wall does not") {
		std::unique_ptr<CollisionWorld> w = floor_and_wall();
		GadgetDef bag = medkit_def();
		bag.health = 50.0f;
		bag.destructible = true;
		bag.receives_explosion_damage = true;
		GadgetDef hard = bag;
		hard.def = 9;
		hard.destructible = false;
		GadgetDef shot_only = bag;
		shot_only.def = 10;
		shot_only.receives_explosion_damage = false;
		GadgetField field({ bag, hard, shot_only });
		uint16_t a = 0, b = 0, c = 0;
		field.deploy(throw_of(kMedkitDef, 1, Vec3{ 0.0f, 1.0f, 0.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), w.get(), &a);
		field.deploy(throw_of(9, 1, Vec3{ 0.0f, 1.0f, -3.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), w.get(), &b);
		field.deploy(throw_of(10, 1, Vec3{ 0.0f, 1.0f, -6.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), w.get(), &c);
		REQUIRE(settle(field, *w, 120));
		REQUIRE(field.find(a) != nullptr);
		CHECK(field.find(a)->health == 50.0f);
		CHECK_FALSE(field.damage(a, 30.0f, false));
		CHECK(field.find(a)->health == doctest::Approx(20.0f));
		CHECK(field.damage(a, 20.0f, false));
		CHECK(field.find(a) == nullptr);
		CHECK_FALSE(field.damage(b, 500.0f, false));
		CHECK(field.find(b) != nullptr);
		CHECK_FALSE(field.damage(c, 500.0f, true));
		CHECK(field.find(c) != nullptr);
		CHECK(field.damage(c, 50.0f, false));
		CHECK(field.stats().destroyed == 2u);

		ExplosionSimDef grenade;
		grenade.present = true;
		grenade.blast_damage = 100.0f;
		grenade.inner_radius = 1.0f;
		grenade.radius = 5.0f;
		uint16_t near = 0, far = 0, hidden = 0;
		field.deploy(
				throw_of(kMedkitDef, 1, Vec3{ 0.0f, 1.0f, 0.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), w.get(), &near);
		field.deploy(
				throw_of(kMedkitDef, 2, Vec3{ -3.5f, 1.0f, 0.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), w.get(), &far);
		field.deploy(
				throw_of(kMedkitDef, 3, Vec3{ 3.0f, 1.0f, 0.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), w.get(), &hidden);
		REQUIRE(settle(field, *w, 120));
		CHECK(field.blast(grenade, BlastFalloff::Linear, Vec3{ 1.0f, 0.0f, 0.0f }, Vec3{ 0.0f, 1.0f, 0.0f }, w.get()) ==
				1u);
		CHECK(field.find(near) == nullptr);
		REQUIRE(field.find(far) != nullptr);
		CHECK(field.find(far)->health == doctest::Approx(50.0f - 100.0f * 0.5f / 4.0f));
		REQUIRE(field.find(hidden) != nullptr);
		CHECK(field.find(hidden)->health == 50.0f);
		CHECK(field.blast(grenade, BlastFalloff::Linear, Vec3{ 1.0f, 0.0f, 0.0f }, Vec3{ 0.0f, 1.0f, 0.0f }, nullptr) ==
				1u);
		CHECK(field.find(hidden) == nullptr);
	}

	TEST_CASE("packs: R2-G4 claymore and AT mine triggers: the claymore's half-angle, radius, strict speed and team "
			  "tests; the mine's radius to the box centre, MinSpeedForMineActivation, moving toward it, the driver's "
			  "team and empty vehicles") {
		Deployable mine;
		mine.team = 1;
		mine.facing = Vec3{ 0.0f, 0.0f, 1.0f };
		const ExplosionPackDef clay = claymore_def().pack;
		const float r = 2.0f;
		const auto soldier = [&](float p_degrees, float p_speed, uint8_t p_team) {
			const float a = p_degrees * 3.14159265f / 180.0f;
			return PackSoldier{ 3, p_team, Vec3{ r * std::sin(a), 0.0f, r * std::cos(a) }, p_speed };
		};
		CHECK(soldier_trips(clay, mine, soldier(30.0f, 1.0f, 2)));
		CHECK_FALSE(soldier_trips(clay, mine, soldier(70.0f, 1.0f, 2)));
		CHECK_FALSE(soldier_trips(clay, mine, soldier(30.0f, 0.4f, 2)));
		CHECK_FALSE(soldier_trips(clay, mine, soldier(30.0f, 0.5f, 2)));
		CHECK_FALSE(soldier_trips(clay, mine, soldier(30.0f, 1.0f, 1)));
		CHECK_FALSE(soldier_trips(clay, mine, PackSoldier{ 3, 2, Vec3{ 0.0f, 0.0f, 3.5f }, 1.0f }));
		CHECK(soldier_trips(clay, mine, PackSoldier{ 3, 2, Vec3{ 0.0f, 0.0f, 0.0f }, 1.0f }));

		ExplosionPackDef at = mine_def().pack;
		const auto vehicle = [](uint8_t p_driver, float p_vz) {
			return PackVehicle{ p_driver, Vec3{ 0.0f, 0.5f, -1.5f }, Vec3{ 0.0f, 0.4f, -1.5f },
				Vec3{ 0.0f, 0.0f, p_vz }, 1.0f };
		};
		CHECK(vehicle_trips(at, mine, vehicle(2, 5.0f)));
		CHECK_FALSE(vehicle_trips(at, mine, vehicle(2, -5.0f)));
		CHECK_FALSE(vehicle_trips(at, mine, vehicle(2, 0.5f)));
		CHECK_FALSE(vehicle_trips(at, mine, vehicle(1, 5.0f)));
		CHECK(vehicle_trips(at, mine, vehicle(0, 3.0f)));
		PackVehicle far = vehicle(2, 5.0f);
		far.centre = Vec3{ 0.0f, 0.0f, -2.5f };
		CHECK_FALSE(vehicle_trips(at, mine, far));
	}

	TEST_CASE("packs: a resting claymore checks soldiers every 0.2 s once SoldierDetonationActivationDelay has run; an "
			  "AT mine checks vehicles the same way; a remote C4 goes on its owner's detonator only; a destroyed pack "
			  "detonates instead of vanishing; an orphan pack keeps TimeToLiveOnPlayerDeath") {
		std::unique_ptr<CollisionWorld> w = floor_and_wall();
		GadgetField field({ claymore_def(), c4_def(), mine_def() });
		uint16_t clay = 0, c4a = 0, c4b = 0, other = 0, at = 0;
		field.deploy(throw_of(6, 1, Vec3{ 0.0f, 0.5f, 0.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), w.get(), &clay);
		field.deploy(throw_of(7, 1, Vec3{ -5.0f, 0.5f, 0.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), w.get(), &c4a);
		field.deploy(throw_of(7, 1, Vec3{ -7.0f, 0.5f, 0.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), w.get(), &c4b);
		field.deploy(throw_of(7, 2, Vec3{ -9.0f, 0.5f, 0.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), w.get(), &other);
		field.deploy(throw_of(8, 1, Vec3{ 5.0f, 0.5f, 5.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), w.get(), &at);
		REQUIRE(settle(field, *w, 120));
		REQUIRE(field.live().size() == 5u);
		const Deployable *placed = field.find(clay);
		REQUIRE(placed != nullptr);
		CHECK(placed->facing[2] == doctest::Approx(1.0f));
		const PackSoldier walker{ 9, 2, core::vec::add(placed->position, Vec3{ 0.0f, 0.0f, 1.0f }), 4.0f };
		const PackVehicle rolling{ 2, core::vec::add(field.find(at)->position, Vec3{ 0.0f, 0.5f, -1.0f }),
			core::vec::add(field.find(at)->position, Vec3{ 0.0f, 0.0f, -1.0f }), Vec3{ 0.0f, 0.0f, 5.0f }, 1.0f };
		Tick clay_tick = 0, at_tick = 0;
		for (Tick t = 1; t <= 200 && (clay_tick == 0 || at_tick == 0); ++t) {
			field.arm_packs(
					std::span<const PackSoldier>(&walker, 1), std::span<const PackVehicle>(&rolling, 1), kSwaySeconds);
			for (const Deployable &d : field.live()) {
				if (d.detonating && d.id == clay && clay_tick == 0) {
					clay_tick = t;
				}
				if (d.detonating && d.id == at && at_tick == 0) {
					at_tick = t;
				}
			}
		}
		MESSAGE("claymore tripped at tick " << clay_tick << ", AT mine at tick " << at_tick);
		CHECK(clay_tick % 6u == 0u);
		CHECK(clay_tick >= 90u);
		CHECK(clay_tick <= 96u);
		CHECK(at_tick % 6u == 0u);
		CHECK(at_tick >= 120u);
		CHECK(at_tick <= 126u);
		CHECK(field.stats().soldier_trips == 1u);
		CHECK(field.stats().vehicle_trips == 1u);

		CHECK(field.remote_detonate(1, 6) == 0u);
		CHECK(field.remote_detonate(1, 7) == 2u);
		CHECK(field.remote_detonate(1, 7) == 0u);
		const std::vector<Deployable> boom = field.take_detonating();
		CHECK(boom.size() == 4u);
		CHECK(field.find(c4a) == nullptr);
		CHECK(field.find(c4b) == nullptr);
		REQUIRE(field.find(other) != nullptr);
		CHECK(field.damage(other, 10.0f, true) == false);
		CHECK(field.damage(other, 40.0f, false) == true);
		REQUIRE(field.find(other) != nullptr);
		CHECK(field.find(other)->detonating);
		CHECK(field.take_detonating().size() == 1u);
		CHECK(field.live().empty());

		uint16_t orphan = 0, kept = 0;
		field.deploy(throw_of(6, 3, Vec3{ 0.0f, 0.5f, -5.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), w.get(), &orphan);
		field.deploy(throw_of(7, 3, Vec3{ 0.0f, 0.5f, -8.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{}), w.get(), &kept);
		REQUIRE(settle(field, *w, 120));
		CHECK(field.owner_gone(3, 1000) == 0u);
		CHECK(field.remote_detonate(3, 7) == 0u);
		CHECK(field.expire(1000 + kSimHz + 1u) == 1u);
		CHECK(field.find(orphan) == nullptr);
		CHECK(field.expire(1000000) == 0u);
		CHECK(field.find(kept) != nullptr);
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

TEST_SUITE("gadgets-unit") {
	TEST_CASE("impulse: R-G4's explosion impulse is (1 - r / BlastRadius) x BlastImpulse floored at ShockwaveImpulse, "
			  "toward the box centre, x clamp(cross-section, 4, 10) / 10; a wreck throws its crew at 0.3 x "
			  "BlastImpulse into the draws' positive octant (R-G3 step 10)") {
		ExplosionSimDef x;
		x.present = true;
		x.radius = 5.0f;
		x.impulse = 1000.0f;
		x.shockwave_impulse = 50.0f;
		CHECK(blast_impulse(x, 0.0) == doctest::Approx(1000.0));
		CHECK(blast_impulse(x, 2.5) == doctest::Approx(500.0));
		CHECK(blast_impulse(x, 4.9) == doctest::Approx(50.0));
		CHECK(blast_impulse(x, 6.0) == doctest::Approx(50.0));

		ImpulseBox box;
		box.half = Vec3{ 1.0f, 1.5f, 0.5f };
		box.centre = Vec3{ 0.0f, 0.0f, 2.5f };
		CHECK(impulse_size_factor(box, Vec3{ 0.0f, 0.0f, 1.0f }) == doctest::Approx(0.6f));
		const Vec3 near = explosion_impulse(x, Vec3{}, box.centre, box);
		CHECK(near[0] == doctest::Approx(0.0f));
		CHECK(near[1] == doctest::Approx(0.0f));
		CHECK(near[2] == doctest::Approx(300.0f));
		box.centre = Vec3{ 0.0f, 0.0f, 6.0f };
		const Vec3 far = explosion_impulse(x, Vec3{}, box.centre, box);
		CHECK(far[2] == doctest::Approx(30.0f));
		box.centre = Vec3{ -6.0f, 0.0f, 0.0f };
		box.axes = { Vec3{ 0.0f, 0.0f, 1.0f }, Vec3{ 0.0f, 1.0f, 0.0f }, Vec3{ -1.0f, 0.0f, 0.0f } };
		const Vec3 turned = explosion_impulse(x, Vec3{}, box.centre, box);
		CHECK(turned[0] == doctest::Approx(-30.0f));
		CHECK(std::fabs(turned[2]) < 1e-4f);

		ImpulseBox unit;
		unit.half = Vec3{ 0.5f, 0.5f, 0.5f };
		CHECK(impulse_size_factor(unit, Vec3{ 1.0f, 0.0f, 0.0f }) == doctest::Approx(0.4f));
		ImpulseBox wall;
		wall.half = Vec3{ 3.0f, 3.0f, 3.0f };
		CHECK(impulse_size_factor(wall, Vec3{ 0.0f, 1.0f, 0.0f }) == doctest::Approx(1.0f));

		const Vec3 thrown = crew_throw_impulse(x, Vec3{ 1.0f, 1.0f, 1.0f });
		for (const float c : thrown) {
			CHECK(c == doctest::Approx(300.0f / std::sqrt(3.0f)));
		}
		const Vec3 tiny = crew_throw_impulse(x, Vec3{ 1e-9f, 1e-9f, 1e-9f });
		CHECK(tiny[0] == 0.0f);
		CHECK(tiny[1] == doctest::Approx(300.0f));
		CHECK(tiny[2] == 0.0f);
	}
}

TEST_SUITE("gadgets-unit") {
	TEST_CASE("life: an owner's death leaves his explosion packs for their TimeToLiveOnPlayerDeath and keeps his bags; "
			  "only a departure marks them left") {
		GadgetDef pack = medkit_def();
		pack.weapon = "pack";
		pack.kind = GadgetKind::ExplosionPack;
		pack.def = 7;
		pack.max_attach_inclination_deg = 180.0f;
		pack.time_to_live_on_owner_gone_s = 1.0f;
		pack.pack.remote_detonator = true;
		std::unique_ptr<CollisionWorld> w = floor_and_wall();
		GadgetField field({ medkit_def(), pack });
		for (const uint16_t def : { kMedkitDef, uint16_t(7) }) {
			GadgetThrow t;
			t.def = def;
			t.owner = 2;
			t.team = 1;
			t.origin = Vec3{ 0.0f, 0.5f, 0.0f };
			t.direction = Vec3{ 0.0f, -1.0f, 0.0f };
			REQUIRE(field.deploy(t, w.get()) == DeployOutcome::Thrown);
		}
		CHECK(field.orphan_packs(2, 100) == 1u);
		CHECK(field.orphan_packs(2, 100) == 0u);
		for (const Deployable &d : field.live()) {
			CHECK(d.owner_gone == (d.def == 7));
			CHECK_FALSE(d.owner_left);
		}
		CHECK(field.remote_detonate(2, 7) == 0u);
		CHECK(field.expire(100 + kSimHz - 1) == 0u);
		CHECK(field.expire(100 + kSimHz + 1) == 1u);
		REQUIRE(field.live().size() == 1u);
		CHECK(field.live()[0].def == kMedkitDef);
		field.owner_gone(2, 200);
		CHECK(field.live()[0].owner_left);
	}
}

TEST_SUITE("gadgets-unit") {
	TEST_CASE("life: an owner's packs left behind by his death still count toward MaxCount, the oldest replaced, and "
			  "are marked left when he then leaves") {
		GadgetDef mine = medkit_def();
		mine.weapon = "mine";
		mine.kind = GadgetKind::ExplosionPack;
		mine.def = 8;
		mine.max_count = 2;
		mine.max_attach_inclination_deg = 180.0f;
		std::unique_ptr<CollisionWorld> w = floor_and_wall();
		GadgetField field({ mine });
		GadgetThrow t;
		t.def = 8;
		t.owner = 4;
		t.team = 2;
		t.origin = Vec3{ 0.0f, 0.5f, 0.0f };
		t.direction = Vec3{ 0.0f, -1.0f, 0.0f };
		uint16_t first = 0, second = 0, third = 0;
		REQUIRE(field.deploy(t, w.get(), &first) == DeployOutcome::Thrown);
		REQUIRE(field.deploy(t, w.get(), &second) == DeployOutcome::Thrown);
		CHECK(field.orphan_packs(4, 50) == 2u);
		REQUIRE(field.deploy(t, w.get(), &third) == DeployOutcome::Thrown);
		CHECK(field.find(first) == nullptr);
		CHECK(field.find(second) != nullptr);
		CHECK(field.stats().replaced == 1u);
		CHECK(field.live().size() == 2u);
		field.owner_gone(4, 60);
		for (const Deployable &d : field.live()) {
			CHECK(d.owner_gone);
			CHECK(d.owner_left);
		}
		t.owner = 4;
		uint16_t fresh = 0;
		REQUIRE(field.deploy(t, w.get(), &fresh) == DeployOutcome::Thrown);
		CHECK(field.live().size() == 3u);
	}
}

namespace {

constexpr SimTime kUpdate = 2 * kTimeUnitsPerTick;

LockingControllerSimDef lock_controller(double p_lock_s, double p_release_s, double p_new_target_s, double p_sample_s) {
	LockingControllerSimDef c;
	c.present = true;
	c.lock_time = seconds_to_units(p_lock_s);
	c.release_time = seconds_to_units(p_release_s);
	c.release_on_new_target_time = seconds_to_units(p_new_target_s);
	c.sample_rate = seconds_to_units(p_sample_s);
	c.ray_length = 500.0f;
	c.acceptance_angle_deg = 10.0f;
	c.angle_constant = 5.0f;
	c.distance_constant = 0.1f;
	c.lock_on_empty_vehicles = true;
	c.zoom_locks[0] = LockType::Always;
	c.zoom_lock_count = 1;
	return c;
}

LockCandidate candidate_at(uint32_t p_id, const Vec3 &p_position) {
	LockCandidate c;
	c.id = p_id;
	c.team = 2;
	c.position = p_position;
	c.aim = core::vec::add(p_position, Vec3{ 0.0f, 1.0f, 0.0f });
	c.radar = 10.0f;
	c.heat = -1.0f;
	return c;
}

Vec3 off_axis(float p_degrees, float p_distance) {
	const float r = p_degrees * 3.14159265f / 180.0f;
	return Vec3{ std::sin(r) * p_distance, 0.0f, std::cos(r) * p_distance };
}

uint32_t updates_until(const LockingControllerSimDef &p_def, LockState &r_state, const LockAim &p_aim,
		std::span<const LockCandidate> p_candidates, const std::function<bool(const LockState &)> &p_done,
		uint32_t p_max) {
	for (uint32_t i = 1; i <= p_max; ++i) {
		step_lock(p_def, r_state, p_aim, kUpdate, p_candidates, nullptr);
		if (p_done(r_state)) {
			return i;
		}
	}
	return 0;
}

ProjectileSimDef missile(float p_engine, float p_max_speed, double p_ignition_s, double p_engine_s) {
	ProjectileSimDef d;
	d.kind = ProjectileKind::Missile;
	d.engine_acceleration = p_engine;
	d.max_speed = p_max_speed;
	d.engine_ignition = seconds_to_units(p_ignition_s);
	d.engine_time_to_live = seconds_to_units(p_engine_s);
	d.guidance.present = true;
	d.guidance.max_turn_deg_per_s = 30.0f;
	return d;
}

double degrees_between(const DVec3 &p_a, const DVec3 &p_b) {
	const double la = std::sqrt(p_a[0] * p_a[0] + p_a[1] * p_a[1] + p_a[2] * p_a[2]);
	const double lb = std::sqrt(p_b[0] * p_b[0] + p_b[1] * p_b[1] + p_b[2] * p_b[2]);
	const double c = (p_a[0] * p_b[0] + p_a[1] * p_b[1] + p_a[2] * p_b[2]) / (la * lb);
	return std::acos(std::clamp(c, -1.0, 1.0)) * 180.0 / 3.14159265358979323846;
}

double speed_of(const DVec3 &p_v) {
	return std::sqrt(p_v[0] * p_v[0] + p_v[1] * p_v[1] + p_v[2] * p_v[2]);
}

constexpr double kUpdateSeconds = 1.0 / 30.0;

double at_update(int p_n) {
	return units_to_seconds(p_n * kUpdate);
}

}

TEST_SUITE("gadgets-unit") {
	TEST_CASE(
			"lock: R2-G5 B timing: a held target fills LockTime 2 s at SampleRate 0.1 s (amount 0.5 after 1 s, locked "
			"at 2 s), is kept ReleaseTime 1 s after it leaves and then falls 1 per LockTime; a better target takes "
			"over once the release timer is below ReleaseTime - ReleaseOnNewTargetTime") {
		const LockingControllerSimDef def = lock_controller(2.0, 1.0, 0.5, 0.1);
		LockAim aim;
		aim.team = 1;
		const std::vector<LockCandidate> one{ candidate_at(7, off_axis(4.0f, 100.0f)) };
		LockState s;
		CHECK(updates_until(def, s, aim, one, [](const LockState &p_s) { return p_s.samples == 1; }, 10) == 3u);
		CHECK(s.target == 7u);
		CHECK(s.timer == 0);
		CHECK_FALSE(s.locked);
		CHECK(updates_until(def, s, aim, one, [](const LockState &p_s) { return p_s.samples == 11; }, 100) == 30u);
		CHECK(s.amount == doctest::Approx(0.5f));
		CHECK_FALSE(s.locked);
		CHECK(updates_until(def, s, aim, one, [](const LockState &p_s) { return p_s.locked; }, 100) == 30u);
		CHECK(s.amount == 1.0f);
		CHECK(s.release < 0);
		CHECK(updates_until(def, s, aim, one, [](const LockState &p_s) { return p_s.samples == 22; }, 10) == 3u);
		CHECK(s.release == def.release_time);
		const std::vector<LockCandidate> none;
		CHECK(updates_until(def, s, aim, none, [](const LockState &p_s) { return !p_s.locked; }, 100) == 33u);
		CHECK(s.amount == doctest::Approx(0.95f));
		CHECK(s.target == 7u);
		CHECK(updates_until(
					  def, s, aim, none, [](const LockState &p_s) { return p_s.target == kNoLockTarget; }, 200) == 60u);

		LockState t;
		updates_until(def, t, aim, one, [](const LockState &p_s) { return p_s.samples == 22; }, 200);
		REQUIRE(t.locked);
		REQUIRE(t.release == def.release_time);
		const std::vector<LockCandidate> two{ one[0], candidate_at(9, off_axis(0.5f, 100.0f)) };
		CHECK(updates_until(def, t, aim, two, [](const LockState &p_s) { return p_s.target == 9u; }, 100) == 18u);
		CHECK(t.timer == 0);
		CHECK_FALSE(t.locked);
	}

	TEST_CASE(
			"lock: R-G5 and R3-G5 E candidates: inside AcceptanceAngle and RayLength, beyond 10 m, in front, of "
			"another team, occupied or LockOnEmptyVehicles, a signature above 1e-6 for the zoom level's LockType; the "
			"score weighs angle by AngleConstant and range by DistanceConstant, AcceptanceAngle 0 takes the nearest") {
		LockingControllerSimDef def = lock_controller(1.5, 1.0, 0.5, 0.06);
		LockAim aim;
		aim.team = 1;
		const auto pick = [&](const std::vector<LockCandidate> &p_c, LockType p_type) -> uint32_t {
			const LockCandidate *c = pick_lock_candidate(def, p_type, aim, p_c, nullptr);
			return c == nullptr ? kNoLockTarget : c->id;
		};
		CHECK(pick({ candidate_at(1, off_axis(8.0f, 100.0f)) }, LockType::Radar) == 1u);
		CHECK(pick({ candidate_at(1, off_axis(12.0f, 100.0f)) }, LockType::Radar) == kNoLockTarget);
		CHECK(pick({ candidate_at(1, off_axis(1.0f, 9.0f)) }, LockType::Radar) == kNoLockTarget);
		CHECK(pick({ candidate_at(1, off_axis(1.0f, 501.0f)) }, LockType::Radar) == kNoLockTarget);
		CHECK(pick({ candidate_at(1, off_axis(180.0f, 100.0f)) }, LockType::Always) == kNoLockTarget);
		LockCandidate mate = candidate_at(1, off_axis(1.0f, 100.0f));
		mate.team = 1;
		CHECK(pick({ mate }, LockType::Radar) == kNoLockTarget);
		CHECK(pick({ candidate_at(1, off_axis(1.0f, 100.0f)) }, LockType::Heat) == kNoLockTarget);
		CHECK(pick({ candidate_at(1, off_axis(1.0f, 100.0f)) }, LockType::LaserPainted) == kNoLockTarget);
		LockCandidate painted = candidate_at(1, off_axis(1.0f, 100.0f));
		painted.painted = true;
		CHECK(pick({ painted }, LockType::LaserPainted) == 1u);
		LockCandidate locked_out = candidate_at(1, off_axis(1.0f, 100.0f));
		locked_out.lockable = false;
		CHECK(pick({ locked_out }, LockType::Always) == kNoLockTarget);
		def.lock_on_empty_vehicles = false;
		CHECK(pick({ candidate_at(1, off_axis(1.0f, 100.0f)) }, LockType::Radar) == kNoLockTarget);
		LockCandidate crewed = candidate_at(1, off_axis(1.0f, 100.0f));
		crewed.occupied = true;
		CHECK(pick({ crewed }, LockType::Radar) == 1u);
		def.lock_on_empty_vehicles = true;
		CHECK(pick({ candidate_at(1, off_axis(2.0f, 200.0f)), candidate_at(2, off_axis(4.0f, 50.0f)) },
					  LockType::Radar) == 1u);
		def.angle_constant = 0.0f;
		CHECK(pick({ candidate_at(1, off_axis(2.0f, 200.0f)), candidate_at(2, off_axis(4.0f, 50.0f)) },
					  LockType::Radar) == 2u);
		def.acceptance_angle_deg = 0.0f;
		CHECK(pick({ candidate_at(1, off_axis(60.0f, 200.0f)), candidate_at(2, off_axis(80.0f, 50.0f)) },
					  LockType::Radar) == 2u);

		LockingControllerSimDef stinger = lock_controller(1.25, 0.5, 1.5, 0.06);
		stinger.zoom_locks = { LockType::Count, LockType::Heat };
		stinger.zoom_lock_count = 2;
		const std::vector<LockCandidate> tank{ candidate_at(3, off_axis(1.0f, 100.0f)) };
		for (uint8_t zoom = 0; zoom < 2; ++zoom) {
			aim.zoom = zoom;
			LockState s;
			CHECK(updates_until(
						  stinger, s, aim, tank, [](const LockState &p_s) { return p_s.target != kNoLockTarget; },
						  300) == 0u);
		}
		LockingControllerSimDef javelin = lock_controller(1.5, 1.0, 0.5, 0.06);
		javelin.zoom_locks = { LockType::Never, LockType::Radar };
		javelin.zoom_lock_count = 2;
		LockState j;
		aim.zoom = 1;
		CHECK(updates_until(javelin, j, aim, tank, [](const LockState &p_s) { return p_s.locked; }, 300) > 0u);
		aim.zoom = 0;
		step_lock(javelin, j, aim, kUpdate, tank, nullptr);
		CHECK(j.target == kNoLockTarget);
		CHECK_FALSE(j.locked);
	}

	TEST_CASE("lock: R-G5 rule 2 measures the cone to the bounding-box centre and the range to the transform") {
		const LockingControllerSimDef def = lock_controller(1.5, 1.0, 0.5, 0.06);
		LockAim aim;
		aim.team = 1;
		const auto pick = [&](const LockCandidate &p_c) {
			const std::vector<LockCandidate> one{ p_c };
			const LockCandidate *c = pick_lock_candidate(def, LockType::Radar, aim, one, nullptr);
			return c == nullptr ? kNoLockTarget : c->id;
		};
		LockCandidate centre_in = candidate_at(1, off_axis(12.0f, 100.0f));
		centre_in.aim = off_axis(8.0f, 100.0f);
		CHECK(pick(centre_in) == 1u);
		LockCandidate centre_out = candidate_at(2, off_axis(8.0f, 100.0f));
		centre_out.aim = off_axis(12.0f, 100.0f);
		CHECK(pick(centre_out) == kNoLockTarget);
		LockCandidate near_origin = candidate_at(3, off_axis(1.0f, 9.0f));
		near_origin.aim = off_axis(1.0f, 11.0f);
		CHECK(pick(near_origin) == kNoLockTarget);
		LockCandidate behind_origin = candidate_at(4, off_axis(1.0f, -12.0f));
		behind_origin.aim = off_axis(1.0f, 12.0f);
		CHECK(pick(behind_origin) == 4u);
	}

	TEST_CASE("lock: LockOnVisibleTargetsOnly casts a ray 3 m beyond the target's centre; a wall before the centre "
			  "hides it, one in the last 3 m does not") {
		std::unique_ptr<CollisionWorld> w = floor_and_wall();
		LockingControllerSimDef def = lock_controller(1.5, 1.0, 0.5, 0.06);
		def.visible_only = true;
		LockAim aim;
		aim.team = 1;
		aim.origin = Vec3{ 16.0f, 1.0f, 0.0f };
		aim.forward = Vec3{ -1.0f, 0.0f, 0.0f };
		const std::vector<LockCandidate> front{ candidate_at(1, Vec3{ 4.0f, 0.0f, 0.0f }) };
		const std::vector<LockCandidate> behind{ candidate_at(2, Vec3{ -5.0f, 0.0f, 0.0f }) };
		LockStepStats stats;
		const LockCandidate *a = pick_lock_candidate(def, LockType::Radar, aim, front, w.get(), &stats);
		CHECK((a != nullptr && a->id == 1u));
		CHECK(stats.rays == 1u);
		CHECK(pick_lock_candidate(def, LockType::Radar, aim, behind, w.get()) == nullptr);
		def.visible_only = false;
		const LockCandidate *b = pick_lock_candidate(def, LockType::Radar, aim, behind, w.get());
		CHECK((b != nullptr && b->id == 2u));
	}

	TEST_CASE("guidance: R2-G4 A test vectors: thrust to MaxSpeed, MaxTurnAngle degrees per second of pure pursuit at "
			  "constant speed, burnout drag and gravity, the engine timeline, the proximity fuse") {
		const DVec3 at{ 0.0, 0.0, 0.0 };
		ProjectileSimDef d = missile(100.0f, 200.0f, 0.0, 10.0);
		MissileGuide powered;
		powered.state = MissileState::Powered;
		const MissileStep a =
				step_guided_missile(d, powered, at, DVec3{ 0.0, 0.0, 50.0 }, 1.0, kUpdateSeconds, 0, std::nullopt, 1);
		CHECK(speed_of(a.velocity) == doctest::Approx(53.33333));
		CHECK(a.engine);
		const MissileStep b =
				step_guided_missile(d, powered, at, DVec3{ 0.0, 0.0, 199.0 }, 1.0, kUpdateSeconds, 0, std::nullopt, 1);
		CHECK(speed_of(b.velocity) == doctest::Approx(200.0));

		d.engine_acceleration = 0.0f;
		MissileGuide homing;
		homing.state = MissileState::Homing;
		homing.target = 5;
		const std::optional<GuidanceTarget> side =
				GuidanceTarget{ Vec3{ 1000.0f, 0.0f, 0.0f }, Vec3{ 1000.0f, 0.0f, 0.0f } };
		const MissileStep c =
				step_guided_missile(d, homing, at, DVec3{ 0.0, 0.0, 100.0 }, 1.0, kUpdateSeconds, 0, side, 1);
		CHECK(c.guide.state == MissileState::Homing);
		CHECK(degrees_between(c.velocity, DVec3{ 0.0, 0.0, 1.0 }) == doctest::Approx(1.0));
		CHECK(speed_of(c.velocity) == doctest::Approx(100.0));
		const std::optional<GuidanceTarget> close =
				GuidanceTarget{ Vec3{ 8.7265f, 0.0f, 1000.0f }, Vec3{ 8.7265f, 0.0f, 1000.0f } };
		const MissileStep e =
				step_guided_missile(d, homing, at, DVec3{ 0.0, 0.0, 100.0 }, 1.0, kUpdateSeconds, 0, close, 1);
		CHECK(degrees_between(e.velocity, DVec3{ 8.7265, 0.0, 1000.0 }) < 1e-4);
		const MissileStep lost =
				step_guided_missile(d, homing, at, DVec3{ 0.0, 0.0, 100.0 }, 1.0, kUpdateSeconds, 0, std::nullopt, 1);
		CHECK(lost.guide.target == kNoLockTarget);
		CHECK(lost.guide.state == MissileState::Powered);

		ProjectileSimDef spent = missile(100.0f, 200.0f, 0.0, 1.0);
		spent.drag = 0.5f;
		spent.gravity = -9.81f;
		MissileGuide burnt;
		burnt.state = MissileState::BurntOut;
		const MissileStep f = step_guided_missile(
				spent, burnt, at, DVec3{ 0.0, 0.0, 100.0 }, 5.0, kUpdateSeconds, 0, std::nullopt, 1);
		CHECK(f.velocity[2] == doctest::Approx(99.5));
		CHECK(f.velocity[1] == doctest::Approx(-0.327));
		CHECK_FALSE(f.engine);

		const ProjectileSimDef timeline = missile(100.0f, 200.0f, 0.1, 2.0);
		const MissileGuide g;
		MissileStep h = step_guided_missile(
				timeline, g, at, DVec3{ 0.0, 0.0, 10.0 }, at_update(2), kUpdateSeconds, 0, std::nullopt, 1);
		CHECK(h.guide.state == MissileState::PreIgnition);
		CHECK_FALSE(h.engine);
		h = step_guided_missile(timeline, h.guide, at, h.velocity, at_update(3), kUpdateSeconds, 0, std::nullopt, 1);
		CHECK(h.guide.state == MissileState::Powered);
		CHECK(h.engine);
		h = step_guided_missile(timeline, h.guide, at, h.velocity, at_update(63), kUpdateSeconds, 0, std::nullopt, 1);
		CHECK(h.guide.state == MissileState::Powered);
		CHECK(h.engine);
		h = step_guided_missile(timeline, h.guide, at, h.velocity, at_update(64), kUpdateSeconds, 0, std::nullopt, 1);
		CHECK(h.guide.state == MissileState::BurntOut);
		CHECK_FALSE(h.engine);

		ProjectileSimDef fused = missile(0.0f, 200.0f, 0.0, 10.0);
		fused.guidance.near_target = true;
		fused.guidance.near_radius = 5.0f;
		fused.guidance.near_min_delay = seconds_to_units(0.1);
		fused.guidance.near_max_delay = seconds_to_units(0.2);
		MissileGuide near;
		near.state = MissileState::Powered;
		near.target = 5;
		const std::optional<GuidanceTarget> by = GuidanceTarget{ Vec3{ 0.0f, 0.0f, 4.9f }, Vec3{ 0.0f, 0.0f, 4.9f } };
		const SimTime now = 1000 * kTimeUnitsPerTick;
		const MissileStep i =
				step_guided_missile(fused, near, at, DVec3{ 0.0, 0.0, 10.0 }, 1.0, kUpdateSeconds, now, by, 99);
		CHECK(i.guide.state == MissileState::Proximity);
		CHECK(i.guide.detonate_at >= now + seconds_to_units(0.1));
		CHECK(i.guide.detonate_at <= now + seconds_to_units(0.2));
		CHECK_FALSE(i.detonate);
		const MissileStep j =
				step_guided_missile(fused, i.guide, at, i.velocity, 1.0, kUpdateSeconds, i.guide.detonate_at, by, 99);
		CHECK_FALSE(j.detonate);
		const MissileStep k = step_guided_missile(
				fused, i.guide, at, i.velocity, 1.0, kUpdateSeconds, i.guide.detonate_at + 1, by, 99);
		CHECK(k.detonate);
		const std::optional<GuidanceTarget> far = GuidanceTarget{ Vec3{ 0.0f, 0.0f, 5.1f }, Vec3{ 0.0f, 0.0f, 5.1f } };
		CHECK(step_guided_missile(fused, near, at, DVec3{ 0.0, 0.0, 10.0 }, 1.0, kUpdateSeconds, now, far, 99)
						.guide.state != MissileState::Proximity);
	}

	TEST_CASE("guidance: the static point is StaticPosition.x ahead along the level launch direction and .y up, (0, "
			  "100, 0) for a vertical launch; a top attack steers to the static point, to the target + "
			  "TargetPositionOffset.y after TimeToActivateGuidingSystem, and homes inside TargetPositionOffset.x") {
		const DVec3 p = missile_static_point(DVec3{ 10.0, 2.0, 0.0 }, DVec3{ 0.0, -0.5, 2.0 }, { 50.0f, 50.0f });
		CHECK(p[0] == doctest::Approx(10.0));
		CHECK(p[1] == doctest::Approx(52.0));
		CHECK(p[2] == doctest::Approx(50.0));
		const DVec3 up = missile_static_point(DVec3{ 10.0, 2.0, 0.0 }, DVec3{ 0.0, 1.0, 0.0 }, { 50.0f, 50.0f });
		CHECK(up == kVerticalStaticPoint);

		ProjectileSimDef d = missile(0.0f, 250.0f, 0.0, 40.0);
		d.guidance.max_turn_deg_per_s = 360.0f;
		d.guidance.activate = seconds_to_units(1.75);
		d.guidance.use_static = true;
		d.guidance.use_target = true;
		d.guidance.static_position = { 50.0f, 50.0f };
		d.guidance.target_offset = { 15.0f, 35.0f };
		MissileGuide g;
		g.state = MissileState::Powered;
		g.target = 3;
		g.has_static = true;
		g.static_point = DVec3{ 0.0, 50.0, 50.0 };
		const std::optional<GuidanceTarget> tank =
				GuidanceTarget{ Vec3{ 0.0f, 0.0f, 200.0f }, Vec3{ 0.0f, 1.0f, 200.0f } };
		const MissileStep a =
				step_guided_missile(d, g, DVec3{}, DVec3{ 0.0, 0.0, 20.0 }, 1.0, kUpdateSeconds, 0, tank, 1);
		CHECK(a.guide.state == MissileState::StaticPoint);
		CHECK(degrees_between(a.velocity, DVec3{ 0.0, 0.0, 1.0 }) == doctest::Approx(12.0));
		const MissileStep b = step_guided_missile(d, a.guide, DVec3{}, a.velocity, 1.8, kUpdateSeconds, 0, tank, 1);
		CHECK(b.guide.state == MissileState::TargetOffset);
		const MissileStep c =
				step_guided_missile(d, b.guide, DVec3{ 0.0, 30.0, 150.0 }, b.velocity, 2.0, kUpdateSeconds, 0, tank, 1);
		CHECK(c.guide.state == MissileState::TargetOffset);
		const MissileStep e =
				step_guided_missile(d, c.guide, DVec3{ 0.0, 30.0, 186.0 }, c.velocity, 2.5, kUpdateSeconds, 0, tank, 1);
		CHECK(e.guide.state == MissileState::Homing);
	}
}
