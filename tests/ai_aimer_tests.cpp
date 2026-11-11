#include <doctest/doctest.h>

#include "bf3/ai/actionplan/aimer.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace bf3;
using namespace bf3::ai;

namespace {

float length(const Vec3 &p_v) {
	return std::sqrt(p_v.x * p_v.x + p_v.y * p_v.y + p_v.z * p_v.z);
}

Vec3 impact_at_range(const Aimer &p_aimer, const Vec3 &p_direction, float p_horizontal_range) {
	const float horizontal_speed =
			std::sqrt(p_direction.x * p_direction.x + p_direction.z * p_direction.z) * p_aimer.speed;
	Vec3 out{};
	p_aimer.impact_position(out, p_direction, p_horizontal_range / horizontal_speed);
	return out;
}

}

TEST_SUITE("ai-actionplan") {
	TEST_CASE("cubic roots: three, one, a double root, and the quadratic and linear cases") {
		std::vector<float> roots;
		cubic_roots(-6.0f, 11.0f, -6.0f, 1.0f, roots);
		std::sort(roots.begin(), roots.end());
		REQUIRE(roots.size() == 3u);
		CHECK(roots[0] == doctest::Approx(1.0f).epsilon(1e-5));
		CHECK(roots[1] == doctest::Approx(2.0f).epsilon(1e-5));
		CHECK(roots[2] == doctest::Approx(3.0f).epsilon(1e-5));
		cubic_roots(-8.0f, 0.0f, 0.0f, 1.0f, roots);
		REQUIRE(roots.size() == 1u);
		CHECK(roots[0] == doctest::Approx(2.0f).epsilon(1e-5));
		cubic_roots(-4.0f, 0.0f, 1.0f, 0.0f, roots);
		std::sort(roots.begin(), roots.end());
		REQUIRE(roots.size() == 2u);
		CHECK(roots[0] == doctest::Approx(-2.0f).epsilon(1e-5));
		CHECK(roots[1] == doctest::Approx(2.0f).epsilon(1e-5));
		cubic_roots(1.0f, 0.0f, 1.0f, 0.0f, roots);
		CHECK(roots.empty());
		cubic_roots(3.0f, -1.5f, 0.0f, 0.0f, roots);
		REQUIRE(roots.size() == 1u);
		CHECK(roots[0] == doctest::Approx(2.0f));
		cubic_roots(0.0f, 0.0f, 0.0f, 0.0f, roots);
		CHECK(roots.empty());
	}

	TEST_CASE(
			"Aimer: a still target without gravity is aimed at straight, and the impact time is distance over speed") {
		Aimer aimer;
		aimer.speed = 400.0f;
		CHECK(aimer.angle() == Aimer::kNoAngle);
		const Vec3 rel{ 30.0f, 0.0f, 40.0f };
		CHECK(aimer.set_target_data(rel, {}, {}, 10.0));
		Vec3 dir{};
		Vec3 target{};
		double impact = 0.0;
		REQUIRE(aimer.firing_direction(10.0, 1.0f, false, dir, target, &impact));
		const float len = length(rel);
		CHECK(dir.x == doctest::Approx(rel.x / len).epsilon(1e-4));
		CHECK(dir.y == doctest::Approx(rel.y / len).epsilon(1e-4));
		CHECK(dir.z == doctest::Approx(rel.z / len).epsilon(1e-4));
		CHECK(impact == doctest::Approx(10.0 + 50.0 / 400.0).epsilon(1e-4));
		CHECK(target.x == rel.x);
		CHECK(aimer.vertical_extreme() == doctest::Approx(25.0 / 400.0).epsilon(1e-4));
	}

	TEST_CASE("Aimer: under gravity the iterated elevation meets the analytic one and the shot lands on the target") {
		Aimer aimer;
		aimer.speed = 80.0f;
		aimer.gravity = -9.81f;
		aimer.time_step = 1.0f / 120.0f;
		const Vec3 rel{ 0.0f, 2.0f, 150.0f };
		aimer.set_target_data(rel, {}, {}, 0.0);
		Vec3 analytic{};
		Vec3 analytic_target{};
		REQUIRE(aimer.static_firing_direction(analytic, analytic_target));
		Vec3 dir{};
		Vec3 target{};
		for (int i = 0; i < 6; ++i) {
			REQUIRE(aimer.firing_direction(0.0, 1.0f, false, dir, target, nullptr));
		}
		const float a = rel.z * rel.z * 9.81f / (2.0f * aimer.speed * aimer.speed);
		const float low = std::atan((rel.z - std::sqrt(rel.z * rel.z - 4.0f * a * (a + rel.y))) / (2.0f * a));
		CHECK(std::asin(analytic.y) > low + 1.0f);
		const float elevation = std::asin(dir.y);
		CHECK(elevation > std::atan2(rel.y, rel.z) + 0.05f);
		CHECK(elevation == doctest::Approx(low).epsilon(2e-2));
		const Vec3 hit = impact_at_range(aimer, dir, rel.z);
		CHECK(std::fabs(hit.y - rel.y) < 0.3f);
		CHECK(std::fabs(hit.x) < 1e-3f);

		Aimer high;
		high.speed = 80.0f;
		high.gravity = -9.81f;
		high.time_step = 1.0f / 120.0f;
		high.set_target_data(rel, {}, {}, 0.0);
		Vec3 lob{};
		CHECK_FALSE(high.firing_direction(0.0, 1.0f, true, lob, target, nullptr));
		REQUIRE(high.firing_direction(0.0, 1.0f, true, lob, target, nullptr));
		REQUIRE(high.firing_direction(0.0, 1.0f, true, lob, target, nullptr));
		CHECK(std::asin(lob.y) == doctest::Approx(std::asin(analytic.y)).epsilon(1e-3));
		CHECK(std::asin(lob.y) > 0.7853982f);
		CHECK(std::fabs(impact_at_range(high, lob, rel.z).y - rel.y) < 1.0f);
	}

	TEST_CASE("Aimer: a crossing target is led by its speed times the flight time; with no projectile speed a moving "
			  "shooter aims 20 m above") {
		Aimer aimer;
		aimer.speed = 300.0f;
		const Vec3 rel{ 0.0f, 0.0f, 120.0f };
		const Vec3 velocity{ 6.0f, 0.0f, 0.0f };
		aimer.set_target_data(rel, velocity, {}, 5.0);
		Vec3 dir{};
		Vec3 target{};
		double impact = 0.0;
		REQUIRE(aimer.firing_direction(5.0, 1.0f, false, dir, target, &impact));
		const float flight = float(impact - 5.0);
		CHECK(flight == doctest::Approx(120.0f / 300.0f).epsilon(1e-2));
		CHECK(target.x == doctest::Approx(velocity.x * flight).epsilon(1e-3));
		CHECK(dir.x / dir.z == doctest::Approx(target.x / target.z).epsilon(1e-3));
		Vec3 predicted{};
		aimer.predicted_target_position(6.0, predicted);
		CHECK(predicted.x == doctest::Approx(6.0f));

		Aimer fleeing;
		fleeing.speed = 5.0f;
		fleeing.set_target_data(rel, Vec3{ 0.0f, 0.0f, 10.0f }, {}, 0.0);
		CHECK_FALSE(fleeing.firing_direction(0.0, 1.0f, false, dir, target, nullptr));

		Aimer none;
		none.speed = 0.0f;
		none.set_target_data(Vec3{ 0.0f, 0.0f, 100.0f }, Vec3{ -10.0f, 0.0f, 0.0f }, {}, 0.0);
		CHECK_FALSE(none.firing_direction(0.0, 1.0f, false, dir, target, nullptr));
		none.set_target_data(Vec3{ 0.0f, 0.0f, 100.0f }, {}, Vec3{ 0.0f, 0.0f, 50.0f }, 0.0);
		REQUIRE(none.firing_direction(0.0, 1.0f, false, dir, target, nullptr));
		CHECK(dir.y / std::sqrt(dir.x * dir.x + dir.z * dir.z) == doctest::Approx(20.0f / 100.0f).epsilon(1e-3));
	}

	TEST_CASE(
			"Aimer: drag slows the flight, a grenade aims short with its fuse time, and the nearest impact is found") {
		Aimer aimer;
		aimer.time_step = 0.01f;
		CHECK(aimer.impact_time(100.0f, 100.0f) == doctest::Approx(1.0f).epsilon(1e-3));
		aimer.drag = 0.1f;
		const float dragged = aimer.impact_time(100.0f, 100.0f);
		CHECK(dragged > 1.04f);
		CHECK(dragged < 1.1f);
		CHECK(aimer.impact_time(1e6f, 1.0f) == 1000.0f);
		Vec3 fall{};
		aimer.drag = 0.0f;
		aimer.gravity = -10.0f;
		aimer.predicted_impact_position(Vec3{ 10.0f, 0.0f, 0.0f }, 1.0f, fall);
		CHECK(fall.x == doctest::Approx(10.0f).epsilon(1e-4));
		CHECK(fall.y == doctest::Approx(-5.0f).epsilon(1e-3));

		Aimer grenade;
		grenade.grenade = true;
		grenade.speed = 15.0f;
		grenade.gravity = -9.81f;
		grenade.set_target_data(Vec3{ 0.0f, 0.0f, 20.0f }, Vec3{ 1.0f, 0.0f, 0.0f }, {}, 0.0);
		Vec3 aim{};
		grenade.predicted_target_position(100.0, aim);
		CHECK(aim.x == doctest::Approx(Aimer::kGrenadeFuseSeconds));
		CHECK(aim.z == doctest::Approx(18.0f));
		grenade.grenade_exact = true;
		grenade.predicted_target_position(100.0, aim);
		CHECK(aim.z == doctest::Approx(20.0f));
		Vec3 apex{};
		float apex_time = 0.0f;
		CHECK(grenade.turn_point_for_grenade(Vec3{ 0.0f, 0.6f, 0.8f }, apex, apex_time));
		CHECK(apex_time == doctest::Approx(0.6f * 15.0f / 9.81f));
		CHECK(apex.y == doctest::Approx(0.6f * 15.0f * 0.5f * apex_time));
		CHECK_FALSE(grenade.turn_point_for_grenade(Vec3{ 0.0f, -0.1f, 1.0f }, apex, apex_time));

		Aimer straight;
		straight.speed = 200.0f;
		straight.set_target_data(Vec3{ 0.0f, 0.0f, 100.0f }, {}, {}, 0.0);
		Vec3 nearest{};
		double t = 0.0;
		REQUIRE(straight.nearest_impact_point(Vec3{ 0.0f, 0.0f, 1.0f }, nearest, t));
		CHECK(t == doctest::Approx(0.5));
		CHECK(nearest.z == doctest::Approx(100.0f).epsilon(1e-3));
	}
}
