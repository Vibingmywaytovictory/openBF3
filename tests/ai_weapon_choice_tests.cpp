#include <doctest/doctest.h>

#include "bf3/ai/behaviour/curves.h"
#include "bf3/ai/behaviour/vehicle_gadgets.h"
#include "bf3/ai/behaviour/weapon_choice.h"

#include <array>

using namespace bf3::ai;

namespace {

WeaponChoice gun(int p_strength, float p_max, int p_ammo = 30) {
	WeaponChoice w;
	w.strength = p_strength;
	w.ammo = p_ammo;
	w.max_range = p_max;
	return w;
}

}

TEST_SUITE("ai-gadgets") {
	TEST_CASE("chose_weapon: a weapon whose range brackets the distance beats a stronger one out of range, the "
			  "strongest in range wins, and none answers -1 (WeaponChoice.cpp choseWeapon)") {
		const std::array<WeaponChoice, 3> w{ gun(5, 200.0f), gun(250, 20.0f), gun(80, 150.0f) };
		CHECK(chose_weapon(w, -1, 35.0f, false, false, false, false) == 2);
		CHECK(chose_weapon(w, -1, 10.0f, false, false, false, false) == 1);
		CHECK(chose_weapon(w, -1, 500.0f, false, false, false, false) == 1);
		CHECK(chose_weapon(std::span<const WeaponChoice>(), -1, 35.0f, false, false, false, false) == -1);
	}

	TEST_CASE("chose_weapon: strength 0, an empty weapon or another type is never chosen; a type flag admits only "
			  "that type and an empty weapon of it; noIndirect drops indirect weapons") {
		std::array<WeaponChoice, 4> w{ gun(0, 200.0f), gun(90, 200.0f, 0), gun(40, 200.0f), gun(60, 200.0f) };
		w[2].type = WeaponType::Heal;
		w[3].indirect = true;
		CHECK(chose_weapon(w, -1, 35.0f, false, false, false, false) == 3);
		CHECK(chose_weapon(w, -1, 35.0f, true, false, false, false) == -1);
		CHECK(chose_weapon(w, -1, 35.0f, false, true, false, false) == 2);
		w[2].ammo = 0;
		CHECK(chose_weapon(w, -1, 35.0f, false, true, false, false) == 2);
		w[1].type = WeaponType::C4;
		w[1].ammo = 1;
		CHECK(chose_weapon(w, -1, 35.0f, false, false, false, false) == 1);
		w[0].type = WeaponType::Resurrect;
		w[0].strength = 10;
		CHECK(chose_weapon(w, -1, 35.0f, false, false, true, false) == 0);
	}

	TEST_CASE("chose_weapon: the current weapon counts triple against the ones after it, in range and out of "
			  "range, as the pin's best-strength update does") {
		const std::array<WeaponChoice, 2> in{ gun(40, 200.0f), gun(100, 200.0f) };
		CHECK(chose_weapon(in, -1, 35.0f, false, false, false, false) == 1);
		CHECK(chose_weapon(in, 0, 35.0f, false, false, false, false) == 0);
		CHECK(chose_weapon(in, 1, 35.0f, false, false, false, false) == 1);
		const std::array<WeaponChoice, 2> out{ gun(40, 20.0f), gun(100, 20.0f) };
		CHECK(chose_weapon(out, 0, 35.0f, false, false, false, false) == 0);
		CHECK(chose_weapon(out, -1, 35.0f, false, false, false, false) == 1);
	}

	TEST_CASE("needs_assist, assist_score and assist_reach: BBMedicAssist's 0.75 and 0.99 health thresholds and its "
			  "strength / (DecliningSlopeCurve(1 - health) x distance) score, BBPMedicAssist's half replenish radius "
			  "plus both radii") {
		CHECK(needs_assist(0.74f, false));
		CHECK_FALSE(needs_assist(0.75f, false));
		CHECK_FALSE(needs_assist(0.9f, false));
		CHECK(needs_assist(0.9f, true));
		CHECK_FALSE(needs_assist(0.99f, true));
		CHECK_FALSE(needs_assist(0.0f, true));
		CHECK(kAssistRadiusMetres == 75.0f);
		CHECK(assist_score(1.0f, 0.5f, 10.0f) ==
				doctest::Approx(1.0f / (DecliningSlopeCurve::calculate(0.5f) * 10.0f)));
		CHECK(assist_score(1.0f, 0.3f, 4.0f) == doctest::Approx(1.0f / (DecliningSlopeCurve::calculate(0.7f) * 4.0f)));
		CHECK(assist_score(1.0f, 0.5f, 5.0f) > assist_score(1.0f, 0.5f, 20.0f));
		CHECK(assist_score(2.0f, 0.5f, 10.0f) == doctest::Approx(2.0f * assist_score(1.0f, 0.5f, 10.0f)));
		CHECK(assist_reach(1.8f, 0.0f) == doctest::Approx(0.9f));
		CHECK(assist_reach(1.8f, 0.6f) == doctest::Approx(1.5f));
	}

	TEST_CASE("placed_explosive_next: BBPFireInfantery's placed-explosive steps, walk up within reach, place, stay "
			  "until the weapon is ready or empty, back off until 9 m clear, set off, and again once no charge lies") {
		PlacedExplosiveCheck c;
		c.loaded = true;
		c.gap = 4.0f;
		c.reach = 1.5f;
		CHECK(placed_explosive_next(ExplosivePhase::Approach, c) == ExplosivePhase::Approach);
		c.gap = 1.0f;
		CHECK(placed_explosive_next(ExplosivePhase::Approach, c) == ExplosivePhase::Place);
		c.loaded = false;
		CHECK(placed_explosive_next(ExplosivePhase::Approach, c) == ExplosivePhase::Approach);
		CHECK(placed_explosive_next(ExplosivePhase::Place, c) == ExplosivePhase::Place);
		c.thrown = true;
		CHECK(placed_explosive_next(ExplosivePhase::Place, c) == ExplosivePhase::Wait);
		CHECK(placed_explosive_next(ExplosivePhase::Wait, c) == ExplosivePhase::BackOff);
		c.loaded = true;
		CHECK(placed_explosive_next(ExplosivePhase::Wait, c) == ExplosivePhase::Wait);
		c.ready = true;
		CHECK(placed_explosive_next(ExplosivePhase::Wait, c) == ExplosivePhase::BackOff);
		c.gap = 8.9f;
		CHECK(placed_explosive_next(ExplosivePhase::BackOff, c) == ExplosivePhase::BackOff);
		c.gap = kPlacedClearMetres;
		CHECK(placed_explosive_next(ExplosivePhase::BackOff, c) == ExplosivePhase::Detonate);
		c.charges_out = true;
		CHECK(placed_explosive_next(ExplosivePhase::Detonate, c) == ExplosivePhase::Detonate);
		c.charges_out = false;
		CHECK(placed_explosive_next(ExplosivePhase::Detonate, c) == ExplosivePhase::Approach);
		CHECK(kPlacedClearMetres == 9.0f);
		CHECK(kPlacedBackOffMetres == 30.0f);
		CHECK(kPlacedDetonateDelayS == doctest::Approx(0.7f));
	}
}
