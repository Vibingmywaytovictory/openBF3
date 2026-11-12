#include <doctest/doctest.h>

#include "bf3/sim/vehicles/vehicle_damage.h"
#include "bf3/sim/vehicles/vehicle_targets.h"

#include <cmath>

using namespace bf3::sim::vehicles;

namespace {

constexpr float kServerDt = 1.0f / 30.0f;

VehicleDamageRule four_zones(float p_max, bool p_share) {
	VehicleDamageRule r;
	for (HealthZoneRule &z : r.zones) {
		z.max_health = p_max;
	}
	r.share_damage = p_share;
	return r;
}

VehicleHealth at_health(const VehicleDamageRule &p_rule, float p_health) {
	VehicleHealth s = spawned_vehicle_health(p_rule);
	for (size_t i = 0; i < kHullZoneCount; ++i) {
		s.health[i] = p_rule.zones[i].max_health * p_health / full_vehicle_health(p_rule);
	}
	return s;
}

VehicleHit hit(float p_damage, HullZone p_zone = HullZone::Front) {
	VehicleHit h;
	h.damage = p_damage;
	h.zone = p_zone;
	return h;
}

VehicleTickInput tick(float p_dt) {
	VehicleTickInput t;
	t.dt = p_dt;
	return t;
}

}

TEST_SUITE("sim-vehicle-damage-unit") {
	TEST_CASE("vehicle damage: the AngleOfImpact bands (R-G3 vectors)") {
		AngleOfImpactRule a;
		a.enabled = true;
		a.zone12_degrees = 30.0f;
		a.zone23_degrees = 60.0f;
		a.multiplier = { 1.0f, 0.7f, 0.4f };
		CHECK(angle_of_impact_factor(a, 0.0f) == doctest::Approx(1.0f));
		CHECK(angle_of_impact_factor(a, 45.0f) == doctest::Approx(0.7f));
		CHECK(angle_of_impact_factor(a, 75.0f) == doctest::Approx(0.4f));
		CHECK(angle_of_impact_factor(a, 60.0f) == doctest::Approx(0.4f));
		a.enabled = false;
		CHECK(angle_of_impact_factor(a, 75.0f) == doctest::Approx(1.0f));
	}

	TEST_CASE("vehicle damage: armour, friendly and occupant modifiers (R-G3 vectors)") {
		DamageParties enemy;
		enemy.vehicle_team = 1;
		enemy.attacker_team = 2;
		enemy.attacker_is_player = true;
		CHECK(damage_modifier(2.0f, enemy) == doctest::Approx(0.5f));
		VehicleDamageRule r = four_zones(500.0f, true);
		r.armor_multiplier = 2.0f;
		VehicleHealth s = spawned_vehicle_health(r);
		VehicleHit h = hit(100.0f);
		h.modifier = damage_modifier(r.armor_multiplier, enemy);
		CHECK(apply_vehicle_damage(r, s, h).damage == doctest::Approx(50.0f));

		DamageParties friendly = enemy;
		friendly.attacker_team = 1;
		CHECK(damage_modifier(2.0f, friendly) == doctest::Approx(0.5f));
		friendly.difficulty = DifficultyRule{ 0.0f, 1.0f };
		CHECK(damage_modifier(2.0f, friendly) == doctest::Approx(0.0f));
		friendly.friendly_fire = true;
		CHECK(damage_modifier(2.0f, friendly) == doctest::Approx(0.5f));

		DamageParties passenger = enemy;
		passenger.attacker_team = 1;
		passenger.attacker_is_occupant = true;
		passenger.human_occupant = true;
		CHECK(damage_modifier(1.0f, passenger) == doctest::Approx(0.0f));
		DamageParties crewed = enemy;
		crewed.human_occupant = true;
		crewed.difficulty = DifficultyRule{ 0.0f, 0.8f };
		CHECK(damage_modifier(1.0f, crewed) == doctest::Approx(0.8f));
		CHECK(damage_modifier(0.0f, enemy) == 0.0f);
	}

	TEST_CASE("vehicle damage: the material relation is a cutoff, not a subtraction (R-G4 checker vectors)") {
		CHECK(material_relation_damage(50.0f, 0.5f, 5.0f) == doctest::Approx(25.0f));
		CHECK(material_relation_damage(10.0f, 0.5f, 5.0f) == doctest::Approx(5.0f));
		CHECK(material_relation_damage(9.0f, 0.5f, 5.0f) == 0.0f);
		CHECK(material_relation_damage(100.0f, 1.0f, 0.0f) == doctest::Approx(100.0f));
	}

	TEST_CASE("vehicle damage: the reactive shield, repair overflow and shared damage (R-G3 vectors)") {
		VehicleDamageRule r = four_zones(500.0f, false);
		r.zones[0].max_shield = 300.0f;
		VehicleHealth s = spawned_vehicle_health(r);
		CHECK(s.shield[0] == 300.0f);
		VehicleDamageOutcome o = apply_vehicle_damage(r, s, hit(400.0f));
		CHECK(o.absorbed);
		CHECK(s.health[0] == 500.0f);
		CHECK(s.shield[0] == 0.0f);
		apply_vehicle_damage(r, s, hit(400.0f));
		CHECK(s.health[0] == doctest::Approx(100.0f));

		VehicleHealth small = spawned_vehicle_health(r);
		CHECK_FALSE(apply_vehicle_damage(r, small, hit(200.0f)).absorbed);
		CHECK(small.health[0] == doctest::Approx(300.0f));
		CHECK(small.shield[0] == 300.0f);

		VehicleHealth repair = spawned_vehicle_health(r);
		repair.health[0] = 480.0f;
		repair.shield[0] = 0.0f;
		apply_vehicle_damage(r, repair, hit(-50.0f));
		CHECK(repair.health[0] == doctest::Approx(500.0f));
		CHECK(repair.shield[0] == doctest::Approx(30.0f));

		VehicleDamageRule shared = four_zones(500.0f, true);
		shared.zones[3].max_health = 1000.0f;
		VehicleHealth sh = spawned_vehicle_health(shared);
		apply_vehicle_damage(shared, sh, hit(100.0f, HullZone::Front));
		CHECK(sh.health[0] == doctest::Approx(400.0f));
		CHECK(sh.health[1] == doctest::Approx(400.0f));
		CHECK(sh.health[2] == doctest::Approx(400.0f));
		CHECK(sh.health[3] == doctest::Approx(800.0f));

		VehicleDamageRule none = four_zones(500.0f, true);
		none.zones[1].max_health = 0.0f;
		VehicleHealth n = spawned_vehicle_health(none);
		CHECK_FALSE(apply_vehicle_damage(none, n, hit(100.0f, HullZone::Rear)).applied);
		CHECK(vehicle_health(none, n) == doctest::Approx(500.0f));
	}

	TEST_CASE("vehicle damage: the zone angle factor (R-G3 vector) and the tank zones' data") {
		HealthZoneRule z;
		z.min_damage_angle = 20.0f;
		z.damage_angle_multiplier = 1.5f;
		CHECK(zone_angle_factor(z, 10.0f) == doctest::Approx(20.0f / 90.0f * 1.5f));
		CHECK(zone_angle_factor(z, 90.0f) == doctest::Approx(1.5f));

		VehicleDamageRule tank = four_zones(1000.0f, true);
		tank.zones[size_t(HullZone::Front)] = { 1000.0f, 0.0f, 81.0f, 1.0f, true };
		tank.zones[size_t(HullZone::Rear)] = { 1000.0f, 175.0f, 36.0f, 2.75f, true };
		tank.zones[size_t(HullZone::Left)] = { 1000.0f, 175.0f, 45.0f, 1.8f, true };
		tank.zones[size_t(HullZone::Right)] = { 1000.0f, 175.0f, 45.0f, 1.8f, true };
		const HullBox box{ { 0.0f, 1.0f, 0.0f }, { 1.8f, 1.2f, 4.0f } };

		const HullHit rear = hull_hit(box, { 0.0f, 1.0f, -4.0f }, { 0.0f, 0.0f, 1.0f });
		CHECK(rear.zone == HullZone::Rear);
		CHECK(rear.impact_degrees == doctest::Approx(0.0f));
		CHECK(rear.zone_degrees == doctest::Approx(0.0f));
		VehicleHealth s = spawned_vehicle_health(tank);
		s.shield = {};
		VehicleHit h = hit(100.0f, rear.zone);
		h.zone_angle = rear.zone_angle;
		h.zone_degrees = rear.zone_degrees;
		CHECK(apply_vehicle_damage(tank, s, h).damage == doctest::Approx(110.0f));

		const HullHit glancing = hull_hit(box, { 0.0f, 1.0f, -4.0f }, { 0.9659258f, 0.0f, 0.2588190f });
		CHECK(glancing.zone == HullZone::Rear);
		CHECK(glancing.zone_degrees == doctest::Approx(75.0f).epsilon(1e-4));
		VehicleHealth g = spawned_vehicle_health(tank);
		g.shield = {};
		h.zone_degrees = glancing.zone_degrees;
		CHECK(apply_vehicle_damage(tank, g, h).damage == doctest::Approx(75.0f / 90.0f * 275.0f).epsilon(1e-4));

		const HullHit front = hull_hit(box, { 0.5f, 1.0f, 4.0f }, { -0.5f, 0.0f, -0.8660254f });
		CHECK(front.zone == HullZone::Front);
		CHECK(front.impact_degrees == doctest::Approx(30.0f).epsilon(1e-4));
		CHECK(front.zone_degrees == doctest::Approx(30.0f).epsilon(1e-4));
		CHECK(zone_angle_factor(tank.zones[size_t(HullZone::Front)], front.zone_degrees) ==
				doctest::Approx(81.0f / 90.0f));

		const HullHit side = hull_hit(box, { 1.8f, 1.0f, 0.5f }, { -1.0f, 0.0f, 0.0f });
		CHECK(side.zone == HullZone::Right);
		CHECK(side.zone_degrees == doctest::Approx(0.0f));
		CHECK(zone_angle_factor(tank.zones[size_t(HullZone::Right)], side.zone_degrees) == doctest::Approx(0.9f));
		CHECK(hull_hit(box, { -1.8f, 1.0f, -0.5f }, { 1.0f, 0.0f, 0.0f }).zone == HullZone::Left);
		const HullHit top = hull_hit(box, { 0.0f, 2.2f, 1.0f }, { 0.0f, -1.0f, 0.0f });
		CHECK(top.impact_degrees == doctest::Approx(0.0f));
		CHECK_FALSE(top.zone_angle);
	}

	TEST_CASE("vehicle damage: health, disabled and back to healthy only at full (R-G3, R3-G3 TV1, TV2)") {
		VehicleDamageRule r = four_zones(1000.0f, false);
		VehicleHealth s = spawned_vehicle_health(r);
		s.health = { 600.0f, 1000.0f, 1000.0f, 1000.0f };
		CHECK(vehicle_health(r, s) == doctest::Approx(600.0f));

		VehicleDamageRule d = four_zones(1000.0f, true);
		d.disabled_threshold = 200.0f;
		VehicleHealth h = at_health(d, 250.0f);
		VehicleDamageOutcome o = apply_vehicle_damage(d, h, hit(60.0f));
		CHECK(o.health_after == doctest::Approx(190.0f));
		CHECK((o.events & vehicle_events::kDisabled) != 0);
		CHECK(h.condition == VehicleCondition::Disabled);
		o = apply_vehicle_damage(d, h, hit(10.0f));
		CHECK(o.events == 0);
		CHECK(h.condition == VehicleCondition::Disabled);
		o = apply_vehicle_damage(d, h, hit(-700.0f));
		CHECK(h.condition == VehicleCondition::Disabled);
		o = apply_vehicle_damage(d, h, hit(-200.0f));
		CHECK(o.health_after == doctest::Approx(1000.0f));
		CHECK((o.events & vehicle_events::kHealthy) != 0);
		CHECK(h.condition == VehicleCondition::Healthy);

		VehicleDamageRule m1a2 = four_zones(1000.0f, true);
		m1a2.disabled_threshold = 500.0f;
		m1a2.decay_delay = 5.0f;
		m1a2.decay_rate = 18.0f;
		VehicleHealth t = at_health(m1a2, 520.0f);
		o = apply_vehicle_damage(m1a2, t, hit(30.0f));
		CHECK(o.health_after == doctest::Approx(490.0f));
		CHECK(t.condition == VehicleCondition::Disabled);
		for (int i = 0; i < 90; ++i) {
			CHECK(step_vehicle_health(m1a2, t, tick(kServerDt)).decay_damage == 0.0f);
		}
		apply_vehicle_damage(m1a2, t, hit(-10.0f));
		CHECK(vehicle_health(m1a2, t) == doctest::Approx(500.0f));
		int quiet = 0;
		while (step_vehicle_health(m1a2, t, tick(kServerDt)).decay_damage == 0.0f && quiet < 1000) {
			++quiet;
		}
		CHECK(quiet >= 149);
		CHECK(quiet <= 150);
		CHECK(vehicle_health(m1a2, t) == doctest::Approx(500.0f - 0.6f));
	}

	TEST_CASE("vehicle damage: regeneration after its delay, only while healthy (R-G3 vector, R3-G3 TV3)") {
		VehicleDamageRule r = four_zones(1000.0f, true);
		r.regeneration_delay = 5.0f;
		r.regeneration_rate = 20.0f;
		VehicleHealth s = spawned_vehicle_health(r);
		apply_vehicle_damage(r, s, hit(100.0f));
		const float dt = 1.0f / 60.0f;
		int quiet = 0;
		VehicleTickOutcome o;
		while ((o = step_vehicle_health(r, s, tick(dt))).regenerated == 0.0f && quiet < 1000) {
			++quiet;
		}
		CHECK(quiet >= 298);
		CHECK(quiet <= 300);
		CHECK((o.events & vehicle_events::kRegenerationStarted) != 0);
		CHECK(o.regenerated == doctest::Approx(20.0f / 60.0f));
		VehicleTickInput off = tick(dt);
		off.regeneration_disabled = true;
		CHECK(step_vehicle_health(r, s, off).regenerated == 0.0f);

		VehicleDamageRule m1a2 = four_zones(1000.0f, true);
		m1a2.regeneration_delay = 12.0f;
		m1a2.regeneration_rate = 30.0f;
		m1a2.disabled_threshold = 500.0f;
		VehicleHealth t = at_health(m1a2, 800.0f);
		int ticks = 0;
		bool full = false;
		while (!full && ticks < 2000) {
			const VehicleTickOutcome step = step_vehicle_health(m1a2, t, tick(kServerDt));
			full = (step.events & vehicle_events::kFullHealth) != 0;
			if (step.regenerated > 0.0f && !full) {
				CHECK(step.regenerated == doctest::Approx(1.0f));
			}
			++ticks;
		}
		CHECK(full);
		CHECK(ticks >= 359 + 200);
		CHECK(ticks <= 361 + 200);
		CHECK(vehicle_health(m1a2, t) == doctest::Approx(1000.0f));

		VehicleHealth disabled = at_health(m1a2, 400.0f);
		disabled.condition = VehicleCondition::Disabled;
		for (int i = 0; i < 600; ++i) {
			CHECK(step_vehicle_health(m1a2, disabled, tick(kServerDt)).regenerated == 0.0f);
		}
	}

	TEST_CASE("vehicle damage: burning after DecayDelay until destroyed (R-G3 vector)") {
		VehicleDamageRule r = four_zones(1000.0f, true);
		r.disabled_threshold = 200.0f;
		r.decay_delay = 2.0f;
		r.decay_rate = 10.0f;
		VehicleHealth s = at_health(r, 250.0f);
		apply_vehicle_damage(r, s, hit(60.0f));
		const float dt = 1.0f / 60.0f;
		int quiet = 0;
		VehicleTickOutcome o;
		while ((o = step_vehicle_health(r, s, tick(dt))).decay_damage == 0.0f && quiet < 1000) {
			++quiet;
		}
		CHECK(quiet >= 118);
		CHECK(quiet <= 120);
		CHECK(o.decay_damage == doctest::Approx(10.0f / 60.0f));
		int burning = 1;
		while (s.condition != VehicleCondition::Destroyed && burning < 5000) {
			o = step_vehicle_health(r, s, tick(dt));
			++burning;
		}
		CHECK((o.events & vehicle_events::kDestroyed) != 0);
		CHECK(burning == doctest::Approx(19 * 60).epsilon(0.002));
	}

	TEST_CASE("vehicle damage: upside down lumps (R3-G3 TV6) and abandonment (TV7)") {
		VehicleDamageRule r = four_zones(1000.0f, true);
		r.upside_down_degrees = 120.0f;
		r.upside_down_damage = 50.0f;
		r.upside_down_delay = 1.0f;
		VehicleHealth s = spawned_vehicle_health(r);
		VehicleTickInput t = tick(kServerDt);
		t.up_y = -0.6f;
		int ticks = 0;
		VehicleTickOutcome o;
		while ((o = step_vehicle_health(r, s, t)).upside_down_damage == 0.0f && ticks < 100) {
			++ticks;
		}
		CHECK(ticks >= 29);
		CHECK(ticks <= 30);
		CHECK(o.upside_down_damage == doctest::Approx(50.0f * float(ticks + 1) * kServerDt));
		t.up_y = -0.4f;
		for (int i = 0; i < 100; ++i) {
			CHECK(step_vehicle_health(r, s, t).upside_down_damage == 0.0f);
		}

		VehicleDamageRule a = four_zones(1000.0f, true);
		VehicleHealth h = spawned_vehicle_health(a);
		VehicleTickInput in = tick(kServerDt);
		in.abandon_applies = true;
		in.time_until_abandoned = 30.0f;
		in.time_until_abandoned_is_destroyed = 60.0f;
		step_vehicle_health(a, h, in);
		in.abandoned = true;
		for (int i = 0; i < 30 * 30 - 2; ++i) {
			step_vehicle_health(a, h, in);
		}
		CHECK(vehicle_health(a, h) == doctest::Approx(1000.0f));
		for (int i = 0; i < 30 * 30 + 2; ++i) {
			step_vehicle_health(a, h, in);
		}
		CHECK(vehicle_health(a, h) == doctest::Approx(500.0f).epsilon(0.01));
		int left = 0;
		while (h.condition != VehicleCondition::Destroyed && left < 4000) {
			step_vehicle_health(a, h, in);
			++left;
		}
		CHECK(left == doctest::Approx(30 * 30).epsilon(0.01));
		in.abandoned = false;
		VehicleHealth back = spawned_vehicle_health(a);
		back.abandon_timer = -5.0f;
		step_vehicle_health(a, back, in);
		CHECK(back.abandon_timer == 30.0f);
	}

	TEST_CASE("vehicle damage: occupants die at once, the explosion waits for the destroyed window (R3-G3 TV8)") {
		VehicleDamageRule ah1z = four_zones(300.0f, true);
		ah1z.critical_damage_time = 2.75f;
		VehicleHealth s = spawned_vehicle_health(ah1z);
		VehicleDamageOutcome o = apply_vehicle_damage(ah1z, s, hit(300.0f));
		CHECK((o.events & vehicle_events::kDestroyed) != 0);
		CHECK(s.condition == VehicleCondition::Destroyed);
		int ticks = 0;
		while ((step_vehicle_health(ah1z, s, tick(kServerDt)).events & vehicle_events::kExploded) == 0 && ticks < 500) {
			++ticks;
		}
		CHECK(ticks + 1 >= 82);
		CHECK(ticks + 1 <= 83);
		CHECK(step_vehicle_health(ah1z, s, tick(kServerDt)).events == 0);
		CHECK_FALSE(apply_vehicle_damage(ah1z, s, hit(50.0f)).applied);

		ah1z.pre_destruction_threshold = 200.0f;
		VehicleHealth now = spawned_vehicle_health(ah1z);
		apply_vehicle_damage(ah1z, now, hit(300.0f));
		CHECK((step_vehicle_health(ah1z, now, tick(kServerDt)).events & vehicle_events::kExploded) != 0);

		VehicleDamageRule growler = four_zones(500.0f, true);
		VehicleHealth g = spawned_vehicle_health(growler);
		apply_vehicle_damage(growler, g, hit(600.0f));
		CHECK((step_vehicle_health(growler, g, tick(kServerDt)).events & vehicle_events::kExploded) != 0);
	}

	TEST_CASE("vehicle damage: the hull's hit target finds the struck zone and skips the vehicle's own crew") {
		const HullBox box{ { 0.0f, 1.0f, 0.2f }, { 1.5f, 1.0f, 3.0f } };
		const bf3::core::Transform hull = bf3::core::Transform::from_rotation_translation(
				{ 0.0f, 0.70710678f, 0.0f, 0.70710678f }, { 10.0f, 2.0f, -4.0f });
		const bf3::sim::weapons::HitTarget t = hull_target(box, hull, kVehicleTargetIdBase + 3u, 7u);
		CHECK(t.capsule_count == 2);
		CHECK(t.id == kVehicleTargetIdBase + 3u);
		const bf3::sim::Vec3 nose = hull.transform_point({ 0.0f, 1.0f, 3.2f });
		const bf3::sim::Vec3 heading = hull.transform_vector({ 0.0f, 0.0f, -1.0f });
		const bf3::sim::Vec3 from{ nose[0] - 20.0f * heading[0], nose[1], nose[2] - 20.0f * heading[2] };
		const bf3::sim::Vec3 to{ nose[0] + 2.0f * heading[0], nose[1], nose[2] + 2.0f * heading[2] };
		bf3::sim::weapons::TargetHit h;
		REQUIRE(bf3::sim::weapons::segment_target(from, to, t, h));
		CHECK(h.property == 7u);
		const HullHit zone = hull_hit(box, to_hull(hull, h.position), to_hull_direction(hull, heading));
		CHECK(zone.zone == HullZone::Front);
		CHECK(zone.impact_degrees == doctest::Approx(0.0f).epsilon(1e-3));

		VehicleTarget v;
		v.target = t;
		v.crew[0] = 5u;
		v.crew_count = 1;
		const std::vector<VehicleTarget> vehicles{ v };
		bf3::sim::weapons::StaticTargets soldiers({ bf3::sim::weapons::HitTarget{} });
		const JoinedTargets joined(soldiers, vehicles);
		bf3::sim::weapons::TargetQuery q;
		q.player_slot = 4u;
		CHECK(joined.targets_for(q).size() == 2);
		q.player_slot = 5u;
		CHECK(joined.targets_for(q).size() == 1);
		const JoinedTargets none(soldiers, {});
		CHECK(none.targets_for(q).data() == soldiers.targets().data());
	}
}
