#include <doctest/doctest.h>

#include "bf3/core/gameplay/vehicle_damage_visuals.h"
#include "bf3/present/vehicle_damage_fx.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace bf3;
using core::gameplay::HealthCompare;
using present::VehicleFxCommand;
using present::VehicleFxOp;
using present::VehicleFxSight;

namespace {

core::gameplay::VehicleDamageVisualsDef tank() {
	core::gameplay::VehicleDamageVisualsDef d;
	d.damage.push_back({ "fx/high", core::Transform::identity(), HealthCompare::LessOrEqual, 0.85f });
	d.damage.push_back({ "fx/disabled", core::Transform::identity(), HealthCompare::LessOrEqual, 0.5f });
	d.damage.push_back({ "fx/critical", core::Transform::identity(), HealthCompare::LessOrEqual, 0.25f });
	core::gameplay::VehicleWreckDef w;
	w.blueprint = "vehicles/tank/tankwreck";
	w.mesh = "vehicles/tank/tankwreck_mesh";
	w.time_to_live = 60.0f;
	w.gates.push_back({ 50.0f, -1, true });
	w.gates.push_back({ 50.0f, 0, true });
	w.effects.push_back({ "fx/fire_s", core::Transform::identity(), -1, true });
	w.effects.push_back({ "fx/fire_a", core::Transform::identity(), 1, true });
	w.effects.push_back({ "fx/fire_b", core::Transform::identity(), 1, false });
	w.effects.push_back({ "fx/smoke_c", core::Transform::identity(), 0, false });
	d.wreck = w;
	return d;
}

VehicleFxSight seen(float p_health, uint8_t p_generation = 1) {
	VehicleFxSight s;
	s.spawner = 7;
	s.generation = p_generation;
	s.kind = 0;
	s.health = p_health;
	return s;
}

std::vector<VehicleFxCommand> step(present::VehicleDamageFx &r_fx, std::vector<VehicleFxSight> p_seen, double p_now) {
	std::vector<VehicleFxCommand> out;
	r_fx.step(p_seen, p_now, 99, out);
	return out;
}

VehicleFxCommand cmd(VehicleFxOp p_op, uint16_t p_effect, uint8_t p_generation = 1) {
	return { p_op, 7, p_generation, 0, p_effect };
}

}

TEST_SUITE("present-vehicle-fx-unit") {
	TEST_CASE("vehicle fx: each damage effect runs while its health comparison holds and stops on repair") {
		present::VehicleDamageFx fx({ tank() });
		CHECK(step(fx, { seen(1.0f) }, 0.0).empty());
		CHECK(step(fx, { seen(0.86f) }, 0.1).empty());
		CHECK(step(fx, { seen(0.85f) }, 0.2) == std::vector{ cmd(VehicleFxOp::StartDamage, 0) });
		CHECK(step(fx, { seen(0.7f) }, 0.3).empty());
		CHECK(step(fx, { seen(0.45f) }, 0.4) == std::vector{ cmd(VehicleFxOp::StartDamage, 1) });
		CHECK(step(fx, { seen(0.2f) }, 0.5) == std::vector{ cmd(VehicleFxOp::StartDamage, 2) });
		CHECK(fx.running() == 3);
		CHECK(step(fx, { seen(0.9f) }, 0.6) ==
				std::vector{ cmd(VehicleFxOp::StopDamage, 0), cmd(VehicleFxOp::StopDamage, 1),
						cmd(VehicleFxOp::StopDamage, 2) });
		CHECK(fx.running() == 0);
		CHECK(step(fx, { seen(0.3f) }, 0.7) ==
				std::vector{ cmd(VehicleFxOp::StartDamage, 0), cmd(VehicleFxOp::StartDamage, 1) });
		CHECK(step(fx, {}, 0.8) == std::vector{ cmd(VehicleFxOp::StopDamage, 0), cmd(VehicleFxOp::StopDamage, 1) });
		CHECK(step(fx, { seen(0.3f) }, 0.9) ==
				std::vector{ cmd(VehicleFxOp::StartDamage, 0), cmd(VehicleFxOp::StartDamage, 1) });
		CHECK(step(fx, { seen(1.0f, 2) }, 1.0) ==
				std::vector{ cmd(VehicleFxOp::StopDamage, 0), cmd(VehicleFxOp::StopDamage, 1) });
	}

	TEST_CASE("vehicle fx: an explosion stops the damage effects and leaves a wreck for its TimeToLive") {
		present::VehicleDamageFx fx({ tank() });
		CHECK(step(fx, { seen(0.2f) }, 0.0).size() == 3);
		VehicleFxSight boom = seen(0.0f);
		boom.hidden = true;
		boom.explosion = true;
		const std::vector<VehicleFxCommand> out = step(fx, { boom }, 1.0);
		REQUIRE(out.size() >= 5);
		CHECK(out[0] == cmd(VehicleFxOp::StopDamage, 0));
		CHECK(out[1] == cmd(VehicleFxOp::StopDamage, 1));
		CHECK(out[2] == cmd(VehicleFxOp::StopDamage, 2));
		CHECK(out[3] == cmd(VehicleFxOp::StartWreck, 0));
		CHECK(out[4] == cmd(VehicleFxOp::StartWreckEffect, 0));
		CHECK(fx.running() == 0);
		CHECK(fx.wrecks() == 1);
		boom.explosion = false;
		CHECK(step(fx, { boom }, 2.0).empty());
		CHECK(step(fx, {}, 60.9).empty());
		CHECK(step(fx, {}, 61.0) == std::vector{ cmd(VehicleFxOp::EndWreck, 0) });
		CHECK(fx.wrecks() == 0);
		CHECK(step(fx, { seen(1.0f, 2) }, 62.0).empty());
	}

	TEST_CASE("vehicle fx: wreck effects follow the random gates, each branch exclusive, about half each") {
		const core::gameplay::VehicleWreckDef w = *tank().wreck;
		size_t first = 0, a = 0, b = 0;
		for (uint64_t seed = 0; seed < 2000; ++seed) {
			const std::vector<bool> on = core::gameplay::roll_wreck_effects(w, seed);
			REQUIRE(on.size() == 4);
			CHECK(on[0]);
			CHECK_FALSE((on[1] && on[2]));
			CHECK((on[1] || on[2]) != on[3]);
			first += on[3] ? 0 : 1;
			a += on[1] ? 1 : 0;
			b += on[2] ? 1 : 0;
		}
		CHECK(first > 900);
		CHECK(first < 1100);
		CHECK(a + b == first);
		CHECK(a > 400);
		CHECK(b > 400);
		CHECK(core::gameplay::roll_wreck_effects(w, 5) == core::gameplay::roll_wreck_effects(w, 5));
	}

	TEST_CASE("vehicle fx: the health comparisons") {
		using core::gameplay::health_compare;
		CHECK(health_compare(HealthCompare::LessOrEqual, 0.5f, 0.5f));
		CHECK_FALSE(health_compare(HealthCompare::Less, 0.5f, 0.5f));
		CHECK(health_compare(HealthCompare::Greater, 0.6f, 0.5f));
		CHECK(health_compare(HealthCompare::GreaterOrEqual, 0.5f, 0.5f));
		CHECK(health_compare(HealthCompare::Equal, 0.5f, 0.5f));
		CHECK(health_compare(HealthCompare::NotEqual, 0.4f, 0.5f));
	}

	
}
