#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/weapons/suppression.h"

#include <doctest/doctest.h>

#include <string>
#include <utility>
#include <vector>

namespace {

using namespace bf3;
namespace sw = bf3::sim::weapons;

core::gameplay::SoldierDef metro_suppression() {
	core::gameplay::SoldierDef s;
	s.suppression = { { "Excluded", 0 }, { "SuppressionResistModifier", 1 }, { "SuppressionBoostModifier", 1 },
		{ "SuppressionDecay", 0.5 }, { "SuppressionSphereRadius", 1.5 }, { "FallOffDelay", 2 },
		{ "FallOffMultiplier", 1 }, { "SuppressionAbortsHealthRegeneration", 1 }, { "CheckLineOfSight", 0 },
		{ "SuppressionLowThreshold", 0.3 }, { "SuppressionHighThreshold", 0.4 }, { "SuppressionUIThreshold", 0.15 } };
	return s;
}

}

TEST_SUITE("weapons-unit") {
	TEST_CASE("suppression: the soldier component's values, the level held for FallOffDelay and then decaying, "
			  "the near-miss segment test and the projectile multiplier") {
		core::Result<sw::SuppressionDef> def = sw::make_suppression_def(metro_suppression());
		REQUIRE(def.ok());
		const sw::SuppressionDef &d = *def;
		CHECK(d.present);
		CHECK(d.sphere_radius == 1.5f);
		CHECK(d.decay == 0.5f);
		CHECK(d.fall_off_delay == 2.0f);
		CHECK(d.fall_off_multiplier == 1.0f);
		CHECK(d.low_threshold == doctest::Approx(0.3f));
		CHECK(d.aborts_regeneration);
		CHECK_FALSE(d.check_line_of_sight);

		core::gameplay::SoldierDef excluded = metro_suppression();
		excluded.suppression[0].second = 1;
		CHECK_FALSE(sw::make_suppression_def(excluded)->present);
		CHECK_FALSE(sw::make_suppression_def(core::gameplay::SoldierDef{})->present);
		core::gameplay::SoldierDef bad = metro_suppression();
		bad.suppression[3].second = -1;
		CHECK_FALSE(sw::make_suppression_def(bad).ok());

		const sim::Tick t0 = 1000;
		sw::SuppressionState s = sw::add_suppression(d, {}, t0, 0.35f);
		CHECK(s.hit_tick == t0);
		CHECK(sw::suppression_level(d, s, t0) == doctest::Approx(0.35f).epsilon(1e-4));
		CHECK(sw::suppression_level(d, s, t0 + 2 * sim::kSimHz) == doctest::Approx(0.35f).epsilon(1e-4));
		CHECK(sw::suppression_level(d, s, t0 + 2 * sim::kSimHz + 30) == doctest::Approx(0.10f).epsilon(1e-3));
		CHECK(sw::suppression_level(d, s, t0 + 3 * sim::kSimHz) == 0.0f);
		const sw::SuppressionState more = sw::add_suppression(d, s, t0 + 2 * sim::kSimHz + 30, 0.07f);
		CHECK(sw::suppression_level(d, more, t0 + 2 * sim::kSimHz + 30) == doctest::Approx(0.17f).epsilon(1e-3));
		CHECK(sw::suppression_level(d, sw::add_suppression(d, s, t0, 5.0f), t0) == 1.0f);
		CHECK(sw::add_suppression(d, s, t0 + 10, 0.0f) == s);
		CHECK(sw::add_suppression(sw::SuppressionDef{}, {}, t0, 0.5f) == sw::SuppressionState{});

		const sim::Vec3 eye{ 0.0f, 1.6f, 30.0f };
		const sim::Vec3 a{ 1.0f, 1.6f, 25.0f };
		const sim::Vec3 b{ 1.0f, 1.6f, 31.0f };
		CHECK(sw::passes_near(a, b, false, sw::SegmentEnd::Flying, eye, 1.5f));
		CHECK_FALSE(sw::passes_near(a, b, false, sw::SegmentEnd::Flying, eye, 0.9f));
		const sim::Vec3 c{ 1.0f, 1.6f, 37.0f };
		CHECK_FALSE(sw::passes_near(b, c, false, sw::SegmentEnd::Flying, eye, 1.5f));
		const sim::Vec3 short_of{ 1.0f, 1.6f, 29.5f };
		CHECK_FALSE(sw::passes_near(a, short_of, false, sw::SegmentEnd::Flying, eye, 1.5f));
		CHECK(sw::passes_near(a, short_of, false, sw::SegmentEnd::Ended, eye, 1.5f));
		CHECK(sw::passes_near(b, c, true, sw::SegmentEnd::Flying, eye, 1.5f));
		CHECK_FALSE(sw::passes_near(a, a, true, sw::SegmentEnd::Ended, eye, 1.5f));

		sw::ProjectileSimDef p;
		p.suppression_min = 7.0f;
		p.suppression_max = 7.0f;
		p.suppression_min_distance = 3.0f;
		p.suppression_max_distance = 500.0f;
		CHECK(sw::suppression_multiplier(p, 1.0f) == 7.0f);
		CHECK(sw::suppression_multiplier(p, 30.0f) == 7.0f);
		CHECK(sw::suppression_multiplier(p, 900.0f) == 7.0f);
		p.suppression_max = 17.0f;
		CHECK(sw::suppression_multiplier(p, 251.5f) == doctest::Approx(12.0f));
		CHECK(sw::suppression_multiplier(p, 2.0f) == 7.0f);
		CHECK(sw::suppression_multiplier(p, 600.0f) == 17.0f);
	}
}
