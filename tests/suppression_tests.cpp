#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/weapons/suppression.h"

#include <doctest/doctest.h>

#include <cmath>
#include <optional>
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
	TEST_CASE(
			"suppression: the soldier component's values and Q9/R2-A7: the counter's quadratic fall-off, the near-miss "
			"amount, Cover and Suppression, the 0.4/0.3 hysteresis, log10(1 + 9 s) and the added dispersion") {
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
		sw::SuppressionDef q9 = d;
		q9.sphere_radius = 2.0f;
		q9.fall_off_delay = 1.0f;
		q9.fall_off_multiplier = 0.5f;
		const sw::SuppressionState stored{ 20000, t0, false };
		CHECK(sw::suppression_counter(q9, stored, t0 + sim::kSimHz / 2) == 20000);
		CHECK(sw::suppression_counter(q9, stored, t0 + 2 * sim::kSimHz) == 15000);
		CHECK(sw::suppression_counter(q9, stored, t0 + 3 * sim::kSimHz) == 0);
		CHECK(sw::suppression_counter(d, sw::SuppressionState{ 30000, t0, false }, t0 + 2 * sim::kSimHz + 30) == 27500);

		sw::ProjectileSimDef p;
		p.suppression_max = 10.0f;
		p.suppression_min = 2.0f;
		p.suppression_min_distance = 3.0f;
		p.suppression_max_distance = 500.0f;
		const sim::Vec3 origin{ 0.0f, 0.0f, 0.0f };
		const auto pass = [&](float p_travelled, float p_miss) {
			return sw::near_miss(sim::Vec3{ 0.0f, 0.0f, p_travelled - 5.0f },
					sim::Vec3{ 0.0f, 0.0f, p_travelled + 5.0f }, origin, sim::Vec3{ p_miss, 0.0f, p_travelled },
					q9.sphere_radius);
		};
		REQUIRE(pass(3.0f, 1.0f));
		CHECK(pass(3.0f, 1.0f)->travelled == 3.0f);
		CHECK(pass(3.0f, 1.0f)->miss == 1.0f);
		CHECK(sw::near_miss_amount(p, q9, *pass(3.0f, 1.0f)) == 7500);
		CHECK(sw::near_miss_amount(p, q9, *pass(251.5f, 1.0f)) == 4500);
		CHECK(sw::near_miss_amount(p, q9, *pass(600.0f, 0.0f)) == 2000);
		CHECK_FALSE(pass(3.0f, 2.0f));
		CHECK(sw::near_miss_amount(p, q9, sw::NearMiss{ 3.0f, 1.0001f }) == 7499);

		const sim::Vec3 centre{ 1.0f, 0.0f, 30.0f };
		CHECK_FALSE(
				sw::near_miss(sim::Vec3{ 0.0f, 0.0f, 31.0f }, sim::Vec3{ 0.0f, 0.0f, 37.0f }, origin, centre, 1.5f));
		const std::optional<sw::NearMiss> short_of =
				sw::near_miss(sim::Vec3{ 0.0f, 0.0f, 20.0f }, sim::Vec3{ 0.0f, 0.0f, 29.5f }, origin, centre, 1.5f);
		REQUIRE(short_of);
		CHECK(short_of->travelled == 29.5f);
		CHECK(short_of->miss == doctest::Approx(std::sqrt(1.25f)));
		CHECK_FALSE(
				sw::near_miss(sim::Vec3{ 0.0f, 0.0f, 20.0f }, sim::Vec3{ 0.0f, 0.0f, 20.0f }, origin, centre, 1.5f));

		CHECK(sw::near_miss_add(7500, 1.0f, 1.0f) == 7500);
		CHECK(sw::near_miss_add(7500, 0.5f, 1.0f) == 3750);
		CHECK(sw::near_miss_add(7500, 1.0f, 1.5f) == 11250);
		CHECK(sw::near_miss_add(7500, 0.5f, 1.5f) == 5625);
		CHECK(sw::near_miss_add(1, 0.4f, 1.0f) == 0);

		sw::SuppressionState s = sw::add_suppression(d, {}, t0, 7500);
		CHECK(s.counter == 7500);
		CHECK(s.hit_tick == t0);
		for (int i = 0; i < 9; ++i) {
			s = sw::add_suppression(d, s, t0, 7500);
		}
		CHECK(s.counter == sw::kSuppressionCounterCap);
		CHECK(sw::add_suppression(d, s, t0 + 10, 0) == s);
		CHECK(sw::add_suppression(sw::SuppressionDef{}, {}, t0, 7500) == sw::SuppressionState{});

		CHECK(d.high_threshold == doctest::Approx(0.4f));
		CHECK_FALSE(sw::add_suppression(d, {}, t0, 26213).suppressed);
		const sw::SuppressionState entered = sw::add_suppression(d, {}, t0, 26214);
		CHECK(entered.suppressed);
		CHECK(sw::suppressed(d, entered, t0));
		CHECK(sw::suppressed(d, sw::SuppressionState{ 19661, t0, true }, t0));
		CHECK_FALSE(sw::suppressed(d, sw::SuppressionState{ 19660, t0, true }, t0));
		CHECK_FALSE(sw::suppressed(d, sw::SuppressionState{ 30000, t0, false }, t0));
		const sw::SuppressionState topped =
				sw::add_suppression(d, sw::SuppressionState{ 20000, t0, true }, t0 + 1, 100);
		CHECK(topped.suppressed);
		CHECK(topped.counter == 20100);
		CHECK_FALSE(sw::add_suppression(d, sw::SuppressionState{ 19000, t0, true }, t0 + 1, 100).suppressed);
		const sim::Tick out_tick = t0 + 2 * sim::kSimHz + 49;
		CHECK(sw::suppressed(d, entered, out_tick - 1));
		CHECK_FALSE(sw::suppressed(d, entered, out_tick + 1));

		sw::SuppressionDef l2 = d;
		l2.low_threshold = 0.2f;
		CHECK(sw::suppression_level(l2, sw::SuppressionState{ 32768, t0, true }, t0) ==
				doctest::Approx(0.375f).epsilon(1e-4));
		CHECK(sw::suppression_scale(l2, sw::SuppressionState{ 32768, t0, true }, t0) ==
				doctest::Approx(0.641f).epsilon(1e-3));
		CHECK(sw::suppression_scale(l2, sw::SuppressionState{ 65535, t0, true }, t0) == doctest::Approx(1.0f));
		CHECK(sw::suppression_level(l2, sw::SuppressionState{ 13107, t0, true }, t0) == 0.0f);
		CHECK(sw::suppression_scale(l2, sw::SuppressionState{ 13107, t0, true }, t0) == 0.0f);
		CHECK(sw::suppression_scale(l2, sw::SuppressionState{ 65535, t0, false }, t0) == 0.0f);
		CHECK(sw::suppression_level(l2, sw::SuppressionState{ 0, t0, false }, t0) == doctest::Approx(-0.2f));

		sw::SwayModifier m;
		m.min_angle = 0.5f;
		const sw::Dispersion base{ 0.2f, 3.0f, 0.1f, 15.0f };
		const sw::Dispersion raised = sw::suppressed_dispersion(base, m, 0.641f);
		CHECK(raised.min_angle == doctest::Approx(0.5205f));
		CHECK(raised.max_angle == doctest::Approx(3.641f));
		CHECK(raised.increase_per_shot == doctest::Approx(0.741f));
		CHECK(raised.decrease_per_second == doctest::Approx(15.641f));
		CHECK(sw::suppressed_dispersion(base, m, 0.0f).min_angle == 0.2f);

		const sw::SuppressionState mirrored = sw::mirror_suppression(d, {}, 26214, t0);
		CHECK(mirrored.suppressed);
		CHECK(sw::mirror_suppression(d, mirrored, 26214, t0) == mirrored);
		const sw::SuppressionState kept = sw::mirror_suppression(d, mirrored, 20000, t0 + 30);
		CHECK(kept.suppressed);
		CHECK_FALSE(sw::mirror_suppression(d, {}, 20000, t0 + 30).suppressed);
		CHECK(sw::mirror_suppression(d, mirrored, 0, 0) == sw::SuppressionState{});
	}
}
