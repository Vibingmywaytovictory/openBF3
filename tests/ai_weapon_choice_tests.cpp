#include <doctest/doctest.h>

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
}
