#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/sim/spotting.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <vector>

namespace {

using namespace bf3;
namespace sp = bf3::sim::spotting;

core::gameplay::SoldierDef metro_spotting() {
	core::gameplay::SoldierDef s;
	s.spotting = { { "Excluded", 0 }, { "SpottingFov", 4 }, { "TeamOrderFov", 15 }, { "SpottingDistance", 1200 },
		{ "PassiveSpottingTimeInterval", 1 }, { "TimeRequiredToPassiveSpot", 1 }, { "OrderNeedAmmoPercentage", 50 },
		{ "OrderHealPercentage", 60 }, { "OrderRepairPercentage", 75 }, { "PickupOrderDistance", 50 },
		{ "HealOrderDistance", 30 }, { "AmmoOrderDistance", 30 }, { "RepairOrderDistance", 50 },
		{ "OnlyAllowedToHaveOneSpottedPlayer", 0 }, { "CoolDownHistoryTime", 3 },
		{ "CoolDownAllowedSpotsWithinHistory", 3 } };
	s.spotting_target = { { "Excluded", 0 }, { "ActiveSpottedTime", 7 }, { "PassiveSpottedTime", 1.5 },
		{ "ActiveSpottedTimeMultiplier", 1 }, { "PassiveSpottedTimeMultiplier", 1 }, { "SpotOnFireMultiplier", 0.45 },
		{ "TargetBoundingBoxCenter", 0 } };
	return s;
}

sp::TargetDef target_def(float p_active, float p_passive) {
	sp::TargetDef t;
	t.active_time = p_active;
	t.passive_time = p_passive;
	t.on_fire_multiplier = 0.45f;
	return t;
}

uint32_t updates_until_none(float p_seconds, float p_dt) {
	sp::SpotTarget t;
	REQUIRE(sp::apply_active(t, target_def(p_seconds, 1.0f), 3));
	for (uint32_t u = 1; u < 100000; ++u) {
		if (sp::step_target(t, p_dt)) {
			return u;
		}
	}
	return 0;
}

sim::Vec3 at_angle(float p_deg, float p_distance) {
	const double r = double(p_deg) * 3.14159265358979323846 / 180.0;
	return { float(std::sin(r)) * p_distance, 0.0f, float(std::cos(r)) * p_distance };
}

}

TEST_SUITE("spotting-unit") {
	TEST_CASE("spotting: the soldier's SpottingComponentData and SpottingTargetComponentData by field name") {
		core::Result<sp::SpottingDef> def = sp::make_spotting_def(metro_spotting());
		REQUIRE(def.ok());
		CHECK(def->spotter_present);
		CHECK(def->target_present);
		CHECK(def->spotter.fov_deg == 4.0f);
		CHECK(def->spotter.team_order_fov_deg == 15.0f);
		CHECK(def->spotter.distance == 1200.0f);
		CHECK(def->spotter.passive_interval == 1.0f);
		CHECK(def->spotter.passive_required == 1.0f);
		CHECK(def->spotter.cooldown_history_time == 3.0f);
		CHECK(def->spotter.cooldown_allowed == 3);
		CHECK_FALSE(def->spotter.one_spotted);
		CHECK(def->spotter.order_heal_percent == 60.0f);
		CHECK(def->spotter.pickup_order_distance == 50.0f);
		CHECK(def->spotter.heal_order_distance == 30.0f);
		CHECK(def->target.active_time == 7.0f);
		CHECK(def->target.passive_time == 1.5f);
		CHECK(def->target.on_fire_multiplier == doctest::Approx(0.45f));

		core::gameplay::SoldierDef missing = metro_spotting();
		missing.spotting.pop_back();
		CHECK_FALSE(sp::make_spotting_def(missing).ok());
		core::gameplay::SoldierDef excluded = metro_spotting();
		excluded.spotting.front().second = 1;
		core::Result<sp::SpottingDef> off = sp::make_spotting_def(excluded);
		REQUIRE(off.ok());
		CHECK_FALSE(off->spotter_present);
		CHECK(off->target_present);
	}

	TEST_CASE("spotting Q11: the server applies Active and Passive requests to the target's state and timer") {
		const sp::TargetDef def = target_def(7.0f, 1.5f);
		sp::SpotTarget t;
		CHECK(sp::apply_active(t, def, 4));
		CHECK(t.state == sp::SpotState::Active);
		CHECK(t.remaining == 7.0f);
		CHECK(t.spotter == 4);

		t.remaining = 2.0f;
		CHECK_FALSE(sp::apply_active(t, def, 5));
		CHECK(t.remaining == 2.0f);
		CHECK(t.spotter == 4);

		CHECK_FALSE(sp::apply_passive(t, def, 5, false));
		CHECK(t.state == sp::SpotState::Active);
		CHECK(t.remaining == 7.0f);
		CHECK(t.spotter == 4);

		t.remaining = 2.0f;
		CHECK_FALSE(sp::apply_passive(t, def, 5, true));
		CHECK(t.remaining == 2.0f);

		sp::SpotTarget p;
		CHECK(sp::apply_passive(p, def, 6, true));
		CHECK(p.state == sp::SpotState::Passive);
		CHECK(p.remaining == doctest::Approx(1.5f * 0.45f));
		CHECK(p.spotter == 6);
		CHECK(sp::apply_active(p, def, 7));
		CHECK(p.state == sp::SpotState::Active);

		sp::SpotTarget radar{ sp::SpotState::Radar, 0.2f, 9 };
		CHECK_FALSE(sp::apply_passive(radar, def, 6, false));
		CHECK(radar.state == sp::SpotState::Radar);
		CHECK(radar.remaining == 1.5f);
		CHECK(radar.spotter == 9);

		sp::SpotTarget u{ sp::SpotState::Unspottable, 0.0f, sp::kNoSpotter };
		CHECK_FALSE(sp::apply_active(u, def, 1));
		CHECK_FALSE(sp::apply_passive(u, def, 1, false));
		CHECK(u.state == sp::SpotState::Unspottable);
	}

	TEST_CASE("spotting R2-E1 B: the countdown drops the state to None one update after it reaches 0") {
		const float dt30 = 1.0f / 30.0f;
		CHECK(updates_until_none(5.0f, dt30) == 152);
		CHECK(updates_until_none(2.0f, dt30) == 62);
		CHECK(updates_until_none(1.0f, dt30) == 31);
		const uint32_t retail = updates_until_none(7.0f, sim::kTickSeconds);
		CHECK(retail >= 7u * sim::kSimHz);
		CHECK(retail <= 7u * sim::kSimHz + 2u);
		sp::SpotTarget none;
		CHECK_FALSE(sp::step_target(none, dt30));
	}

	TEST_CASE("spotting R-E1 3: SpottingFov is the half-angle around the crosshair, the range is to the box centre") {
		const sim::Vec3 eye{ 0, 0, 0 };
		const sim::Vec3 view{ 0, 0, 1 };
		CHECK(sp::in_spot_cone(eye, view, at_angle(9.5f, 100.0f), 10.0f, 300.0f));
		CHECK_FALSE(sp::in_spot_cone(eye, view, at_angle(10.5f, 100.0f), 10.0f, 300.0f));
		CHECK(sp::in_spot_cone(eye, view, at_angle(14.0f, 100.0f), 10.0f * sp::kSeatedConeWidening, 300.0f));
		CHECK_FALSE(sp::in_spot_cone(eye, view, at_angle(14.0f, 100.0f), 10.0f, 300.0f));
		CHECK(sp::in_spot_cone(eye, view, sim::Vec3{ 0, 0, 299.9f }, 10.0f, 300.0f));
		CHECK_FALSE(sp::in_spot_cone(eye, view, sim::Vec3{ 0, 0, 300.1f }, 10.0f, 300.0f));
		CHECK(sp::crosshair_cosine(eye, view, at_angle(60.0f, 10.0f)) == doctest::Approx(0.5f));
	}

	TEST_CASE("spotting R-E1 4a: the cool-down history appends first and keeps refused presses") {
		sp::SpotterDef def;
		def.cooldown_history_time = 5.0f;
		def.cooldown_allowed = 3;
		sp::SpotCooldown c;
		const std::array<double, 7> at{ 0.0, 1.0, 2.0, 3.0, 5.5, 6.5, 8.1 };
		const std::array<bool, 7> allowed{ true, true, true, false, false, false, true };
		for (size_t i = 0; i < at.size(); ++i) {
			CAPTURE(at[i]);
			CHECK(c.press(at[i], def) == allowed[i]);
		}
		CHECK(c.held() == 3);
	}

	TEST_CASE("spotting R-E1 5: a ray takes its first enemy hit, later hits and teammates are skipped") {
		const std::vector<sp::RayHit> hits{ { true, 2, 1 }, { true, 7, 2 }, { true, 9, 2 } };
		CHECK(sp::first_enemy_hit(hits, 1) == std::optional<uint8_t>(7));
		const std::vector<sp::RayHit> blocked{ { false, 0, 0 }, { true, 3, 0 }, { true, 4, 1 } };
		CHECK_FALSE(sp::first_enemy_hit(blocked, 1).has_value());
		const std::vector<sp::RayHit> sixth{ { true, 1, 1 }, { true, 1, 1 }, { true, 1, 1 }, { true, 1, 1 },
			{ true, 1, 1 }, { true, 8, 2 } };
		CHECK_FALSE(sp::first_enemy_hit(sixth, 1).has_value());
	}

	TEST_CASE("spotting R-E1 checker: the potential list holds 8, the one-spotted trim keeps the nearest crosshair") {
		sp::PotentialList list;
		for (uint8_t i = 0; i < 8; ++i) {
			CHECK(list.add(i, 0.1f * float(i)));
		}
		CHECK_FALSE(list.add(20, 1.0f));
		CHECK(list.add(3, 0.3f));
		CHECK(list.entries().size() == sp::kMaxSpotTargets);
		list.trim_to_nearest_crosshair();
		REQUIRE(list.entries().size() == 1);
		CHECK(list.entries()[0].slot == 7);
		sp::PotentialList behind;
		behind.add(1, -0.5f);
		behind.trim_to_nearest_crosshair();
		CHECK(behind.empty());
	}

	TEST_CASE("spotting R-E1 7: the passive accumulator loses one interval per update and sends below zero") {
		sp::SpotterDef def;
		def.passive_interval = 0.25f;
		def.passive_required = 1.0f;
		sp::PassiveScan scan;
		CHECK_FALSE(scan.send_due(true, false, def));
		CHECK(scan.accumulator() == 1.0f);
		const std::array<float, 4> left{ 0.75f, 0.5f, 0.25f, 0.0f };
		for (float l : left) {
			CHECK_FALSE(scan.send_due(false, false, def));
			CHECK(scan.accumulator() == l);
		}
		CHECK(scan.send_due(false, false, def));
		CHECK(scan.accumulator() == 1.0f);
		CHECK_FALSE(scan.send_due(false, true, def));
		CHECK(scan.accumulator() == 1.0f);

		sp::SpotterDef retail;
		retail.passive_interval = 1.0f;
		retail.passive_required = 1.0f;
		sp::PassiveScan r;
		CHECK_FALSE(r.send_due(false, false, retail));
		CHECK(r.send_due(false, false, retail));

		sp::PassiveScan timer;
		CHECK(timer.scan_due(0.1f, false, false, retail));
		CHECK(timer.timer() == 1.0f);
		CHECK_FALSE(timer.scan_due(0.5f, false, false, retail));
		CHECK_FALSE(timer.scan_due(0.5f, true, false, retail));
		CHECK(timer.scan_due(0.5f, false, false, retail));
		CHECK_FALSE(timer.scan_due(1.0f, false, true, retail));
	}
}
