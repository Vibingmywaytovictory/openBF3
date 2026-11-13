#include "sim_world_support.h"

#include "bf3/core/math.h"
#include "bf3/sim/weapons/damage.h"
#include "bf3/sim/weapons/gadgets.h"
#include "bf3/sim/weapons/spread_recoil.h"

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
