#include "bf3/sim/commo.h"
#include "bf3/sim/spotting.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <vector>

namespace {

using namespace bf3;
namespace cm = bf3::sim::commo;
namespace sp = bf3::sim::spotting;

sp::SpotterDef order_def() {
	sp::SpotterDef d;
	d.order_heal_percent = 50.0f;
	d.order_repair_percent = 50.0f;
	d.order_need_ammo_percent = 25.0f;
	d.pickup_order_distance = 10.0f;
	d.heal_order_distance = 3.0f;
	d.ammo_order_distance = 10.0f;
	d.repair_order_distance = 15.0f;
	return d;
}

cm::OrderParty party(float p_health, float p_x) {
	cm::OrderParty p;
	p.position = { p_x, 0.0f, 0.0f };
	p.health = p_health;
	p.max_health = 100.0f;
	return p;
}

cm::NeedParty needer(float p_health, const sp::SpotterDef &p_def) {
	cm::NeedParty n;
	n.health = p_health;
	n.max_health = 100.0f;
	n.def = &p_def;
	return n;
}

}

TEST_SUITE("commo-unit") {
	TEST_CASE("commo R-E2 A: the contextual order to the teammate nearest the crosshair, first match wins") {
		const sp::SpotterDef def = order_def();
		const cm::OrderParty me = party(40.0f, 0.0f);
		cm::OrderParty medic = party(100.0f, 6.0f);
		medic.supplies.heals = true;
		CHECK(cm::pick_order(def, me, medic) == cm::OrderType::RequestHeal);

		cm::OrderParty in_jeep = me;
		in_jeep.in_vehicle = true;
		in_jeep.vehicle_health = 30.0f;
		in_jeep.vehicle_max_health = 100.0f;
		cm::OrderParty engineer = party(100.0f, 6.0f);
		engineer.supplies.repairs = true;
		engineer.supplies.heals = true;
		CHECK(cm::pick_order(def, in_jeep, engineer) == cm::OrderType::RequestRepair);

		const cm::OrderParty far_medic = [&] {
			cm::OrderParty m = medic;
			m.position.x = 10.0f;
			return m;
		}();
		CHECK(cm::pick_order(def, me, far_medic) == cm::OrderType::None);

		cm::OrderParty me_medic = party(100.0f, 0.0f);
		me_medic.supplies.heals = true;
		CHECK(cm::pick_order(def, me_medic, party(30.0f, 5.0f)) == cm::OrderType::LetMeHeal);

		cm::OrderParty me_low = party(100.0f, 0.0f);
		me_low.low_ammo = true;
		cm::OrderParty support = party(100.0f, 5.0f);
		support.supplies.ammo = true;
		CHECK(cm::pick_order(def, me_low, support) == cm::OrderType::RequestAmmo);
		cm::OrderParty me_support = party(100.0f, 0.0f);
		me_support.supplies.ammo = true;
		cm::OrderParty mate_low = party(100.0f, 5.0f);
		mate_low.low_ammo = true;
		CHECK(cm::pick_order(def, me_support, mate_low) == cm::OrderType::LetMeGiveAmmo);

		cm::OrderParty driver = party(100.0f, 5.0f);
		driver.in_vehicle = true;
		driver.vehicle_has_free_seat = true;
		driver.vehicle_health = 100.0f;
		driver.vehicle_max_health = 100.0f;
		CHECK(cm::pick_order(def, party(100.0f, 0.0f), driver) == cm::OrderType::RequestRide);
		cm::OrderParty damaged = driver;
		damaged.vehicle_health = 20.0f;
		cm::OrderParty me_engineer = party(100.0f, 0.0f);
		me_engineer.supplies.repairs = true;
		CHECK(cm::pick_order(def, me_engineer, damaged) == cm::OrderType::LetMeRepair);
		cm::OrderParty me_driver = driver;
		me_driver.position.x = 0.0f;
		me_driver.offers_pickup = true;
		CHECK(cm::pick_order(def, me_driver, party(100.0f, 5.0f)) == cm::OrderType::GetIn);
	}

	TEST_CASE("commo R-E2 B: the low-ammo flag is any counted weapon below the percentage") {
		const std::array<cm::WeaponAmmo, 1> low{ { { 20.0f, 0.0f, 30.0f, 5.0f, true } } };
		CHECK(cm::low_ammo(low, 25.0f));
		const std::array<cm::WeaponAmmo, 2> mixed{ { { 30.0f, 120.0f, 30.0f, 5.0f, true },
				{ 0.0f, 0.0f, 1.0f, 1.0f, false } } };
		CHECK_FALSE(cm::low_ammo(mixed, 25.0f));
		const std::array<cm::WeaponAmmo, 2> empty_def{ { { 1.0f, 0.0f, 30.0f, 5.0f, true },
				{ 0.0f, 0.0f, 0.0f, 0.0f, true } } };
		CHECK_FALSE(cm::low_ammo(empty_def, 25.0f));
	}

	TEST_CASE("commo R-E2 C, R4-E2 A3: which order types carry a voice line") {
		const std::vector<uint8_t> issuer{ 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14 };
		for (uint8_t t = 0; t < uint8_t(cm::OrderType::Count); ++t) {
			CAPTURE(int(t));
			CHECK(cm::issuer_voice(cm::OrderType(t)) == (std::find(issuer.begin(), issuer.end(), t) != issuer.end()));
			CHECK(cm::server_voice(cm::OrderType(t)) == (t >= 1 && t <= 16));
		}
	}

	TEST_CASE("commo R2-E2 B/C, R4-E2 B: the receiver's order icon, gadget-gated, 10 s with a 2 s 2 Hz blink") {
		cm::Supplies medic;
		medic.heals = true;
		cm::OrderIcon icon;
		cm::receive_order(icon, 1, cm::OrderType::RequestHeal, 4, medic);
		CHECK(icon.icon == cm::kIconNeedMedic);
		CHECK(icon.issuer == 4);
		CHECK(icon.icon_timer == 10.0f);
		CHECK(icon.blink_timer == 2.0f);
		CHECK(cm::teammate_icon(icon, 4, cm::kIconNone, 0.3) == cm::kIconNeedMedicHighlight);
		CHECK(cm::teammate_icon(icon, 4, cm::kIconNone, 0.1) == cm::kIconNeedMedic);
		CHECK(cm::teammate_icon(icon, 4, cm::kIconNone, 0.6) == cm::kIconNeedMedic);
		CHECK(cm::teammate_icon(icon, 4, cm::kIconNone, 0.8) == cm::kIconNeedMedicHighlight);
		CHECK(cm::teammate_icon(icon, 5, cm::kIconNeedAmmo, 0.3) == cm::kIconNeedAmmo);
		cm::step_order_icon(icon, 2.5f, true);
		CHECK(cm::teammate_icon(icon, 4, cm::kIconNone, 0.3) == cm::kIconNeedMedic);
		cm::step_order_icon(icon, 7.5f, true);
		CHECK(cm::teammate_icon(icon, 4, cm::kIconNeedAmmo, 0.3) == cm::kIconNeedAmmo);

		cm::OrderIcon no_bag;
		cm::receive_order(no_bag, 1, cm::OrderType::RequestHeal, 4, cm::Supplies{});
		CHECK(no_bag.icon_timer == 0.0f);
		CHECK(cm::teammate_icon(no_bag, 4, cm::kIconNone, 0.3) == cm::kIconNone);

		cm::OrderIcon ride;
		cm::receive_order(ride, 1, cm::OrderType::RequestRide, 2, cm::Supplies{});
		CHECK(cm::teammate_icon(ride, 2, cm::kIconNone, 0.1) == cm::kIconNeedPickup);
		cm::OrderIcon let_me;
		cm::receive_order(let_me, 1, cm::OrderType::LetMeHeal, 2, medic);
		CHECK(cm::teammate_icon(let_me, 2, cm::kIconNeedMedic, 0.1) == cm::kIconNone);

		cm::OrderIcon again = icon;
		cm::receive_order(again, 1, cm::OrderType::RequestHeal, 4, medic);
		CHECK(again.icon_timer == icon.icon_timer);
		cm::receive_order(again, 2, cm::OrderType::RequestHeal, 4, medic);
		CHECK(again.icon_timer == 10.0f);
		CHECK(again.blink_timer == 2.0f);
		cm::step_order_icon(again, 0.1f, false);
		CHECK(again.icon_timer == 0.0f);
	}

	TEST_CASE("commo R2-E2 D: standing need icons, first me as the needer, then the teammate") {
		const sp::SpotterDef def = order_def();
		cm::NeedParty wounded = needer(40.0f, def);
		cm::NeedParty medic = needer(100.0f, def);
		medic.supplies.heals = true;
		CHECK(cm::standing_need_icon(medic, wounded) == cm::kIconNeedMedic);
		CHECK(cm::standing_need_icon(wounded, medic) == cm::kIconNeedMedic);
		CHECK(cm::standing_need_icon(wounded, needer(100.0f, def)) == cm::kIconNone);
		cm::NeedParty low = needer(100.0f, def);
		low.low_ammo = true;
		cm::NeedParty support = needer(100.0f, def);
		support.supplies.ammo = true;
		CHECK(cm::standing_need_icon(support, low) == cm::kIconNeedAmmo);
		cm::NeedParty both = needer(40.0f, def);
		both.low_ammo = true;
		cm::NeedParty all = needer(100.0f, def);
		all.supplies.heals = true;
		all.supplies.ammo = true;
		CHECK(cm::standing_need_icon(all, both) == cm::kIconNeedMedic);
	}

	TEST_CASE("commo R2-E1 C/D: the 3D spot kind and the minimap rule") {
		cm::SpotKind s;
		s.active = true;
		s.spotter_known = true;
		s.spotter_team = 1;
		CHECK(cm::spotted_kind(s, 1, false) == 2);
		CHECK(cm::spotted_kind(s, 2, false) == 0);
		CHECK(cm::spotted_kind(s, 1, true) == 0);
		s.spotter_leads_squad = true;
		CHECK(cm::spotted_kind(s, 1, false) == 1);
		s.spotter_flagged = true;
		CHECK(cm::spotted_kind(s, 1, false) == 3);
		s.spotter_team = 0;
		CHECK(cm::spotted_kind(s, 2, false) == 3);
		cm::SpotKind passive = s;
		passive.active = false;
		CHECK(cm::spotted_kind(passive, 1, false) == 0);
		CHECK(cm::minimap_shows_enemy(sp::SpotState::Passive, false));
		CHECK(cm::minimap_shows_enemy(sp::SpotState::Radar, false));
		CHECK_FALSE(cm::minimap_shows_enemy(sp::SpotState::Unspottable, false));
		CHECK_FALSE(cm::minimap_shows_enemy(sp::SpotState::Active, true));
	}
}
