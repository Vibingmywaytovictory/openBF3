#include "bf3/sim/commo.h"

#include <cmath>

namespace bf3::sim::commo {

namespace {

bool below_percent(float p_value, float p_max, float p_percent) {
	return p_max > 0.0f && 100.0f * p_value / p_max < p_percent;
}

bool closer_than(const Vec3 &p_a, const Vec3 &p_b, float p_distance) {
	const Vec3 d = core::vec::sub(p_a, p_b);
	return core::vec::dot(d, d) < p_distance * p_distance;
}

bool vehicle_needs_repair(const OrderParty &p_party, float p_percent) {
	return p_party.in_vehicle && below_percent(p_party.vehicle_health, p_party.vehicle_max_health, p_percent);
}

// SOURCE: notes R2-E2 D (repair, heal, then ammo; the first match wins).
int32_t need_of(const NeedParty &p_needer, const NeedParty &p_supplier) {
	if (p_needer.def == nullptr) {
		return kIconNone;
	}
	if (p_needer.in_vehicle && p_supplier.supplies.repairs &&
			below_percent(p_needer.vehicle_health, p_needer.vehicle_max_health, p_needer.def->order_repair_percent)) {
		return kIconNeedRepair;
	}
	if (p_supplier.supplies.heals &&
			below_percent(p_needer.health, p_needer.max_health, p_needer.def->order_heal_percent)) {
		return kIconNeedMedic;
	}
	if (p_needer.low_ammo && p_supplier.supplies.ammo) {
		return kIconNeedAmmo;
	}
	return kIconNone;
}

}

// SOURCE: notes R-E2 C and its checker (the issuer's order VO for types 1-5 and 7-14).
bool issuer_voice(OrderType p_type) {
	const uint8_t t = uint8_t(p_type);
	return (t >= 1 && t <= 5) || (t >= 7 && t <= 14);
}

// SOURCE: notes R4-E2 3 (the server's per-type order event for types 1 to 16).
bool server_voice(OrderType p_type) {
	const uint8_t t = uint8_t(p_type);
	return t >= 1 && t <= 16;
}

// SOURCE: notes R-E2 A and its checker (the order priority and its distance and percentage tests).
OrderType pick_order(const spotting::SpotterDef &p_def, const OrderParty &p_me, const OrderParty &p_mate) {
	const bool pickup = closer_than(p_me.position, p_mate.position, p_def.pickup_order_distance);
	const bool repair = closer_than(p_me.position, p_mate.position, p_def.repair_order_distance);
	const bool ammo = closer_than(p_me.position, p_mate.position, p_def.ammo_order_distance);
	if (vehicle_needs_repair(p_mate, p_def.order_repair_percent) && p_me.supplies.repairs && repair) {
		return OrderType::LetMeRepair;
	}
	if (!p_me.in_vehicle) {
		if (p_mate.in_vehicle && p_mate.vehicle_has_free_seat && pickup) {
			return OrderType::RequestRide;
		}
	} else if (vehicle_needs_repair(p_me, p_def.order_repair_percent) && p_mate.supplies.repairs && repair) {
		return OrderType::RequestRepair;
	}
	if (p_me.in_vehicle && p_me.offers_pickup && pickup) {
		return OrderType::GetIn;
	}
	if (below_percent(p_me.health, p_me.max_health, p_def.order_heal_percent) && p_mate.supplies.heals && pickup) {
		return OrderType::RequestHeal;
	}
	if (below_percent(p_mate.health, p_mate.max_health, p_def.order_heal_percent) && p_me.supplies.heals && pickup) {
		return OrderType::LetMeHeal;
	}
	if (p_me.low_ammo && p_mate.supplies.ammo && ammo) {
		return OrderType::RequestAmmo;
	}
	if (p_mate.low_ammo && p_me.supplies.ammo && ammo) {
		return OrderType::LetMeGiveAmmo;
	}
	return OrderType::None;
}

// SOURCE: notes R-E2 B and its checker (any counted weapon with 100 x (in weapon + reserve) / (capacity x count) below
// the percentage, and not low at once when a counted weapon has capacity x count 0).
// INFERRED: which values are in weapon, reserve, capacity and count, as R-E2 B infers. Settle: R-E2 B's getters.
bool low_ammo(std::span<const WeaponAmmo> p_weapons, float p_need_percent) {
	bool low = false;
	for (const WeaponAmmo &w : p_weapons) {
		if (!w.counted) {
			continue;
		}
		const float full = w.capacity * w.count;
		if (full == 0.0f) {
			return false;
		}
		low = low || 100.0f * (w.in_weapon + w.reserve) / full < p_need_percent;
	}
	return low;
}

int32_t highlight_icon(int32_t p_icon) {
	switch (p_icon) {
		case kIconNeedAmmo:
			return kIconNeedAmmoHighlight;
		case kIconNeedMedic:
			return kIconNeedMedicHighlight;
		case kIconNeedRepair:
			return kIconNeedRepairHighlight;
		case kIconNeedPickup:
			return kIconNeedPickupHighlight;
		default:
			return p_icon;
	}
}

// SOURCE: notes R2-E2 constants (a 2 Hz square wave, high when floor(4 x frac(c)) is odd).
bool blink_high(double p_clock) {
	const double frac = p_clock - std::floor(p_clock);
	return (int64_t(std::floor(4.0 * frac)) & 1) != 0;
}

// SOURCE: notes R2-E2 B (a newer order resets the timers; the gadget test; other types no icon with the timers set).
void receive_order(OrderIcon &r_icon, uint64_t p_update, OrderType p_type, uint8_t p_issuer, const Supplies &p_mine) {
	if (p_update <= r_icon.seen) {
		return;
	}
	r_icon.seen = p_update;
	r_icon.icon_timer = 0.0f;
	r_icon.blink_timer = 0.0f;
	r_icon.type = p_type;
	int32_t icon = kIconNone;
	bool shown = true;
	switch (p_type) {
		case OrderType::RequestHeal:
			icon = kIconNeedMedic;
			shown = p_mine.heals;
			break;
		case OrderType::RequestAmmo:
			icon = kIconNeedAmmo;
			shown = p_mine.ammo;
			break;
		case OrderType::RequestRepair:
			icon = kIconNeedRepair;
			shown = p_mine.repairs;
			break;
		case OrderType::RequestRide:
		case OrderType::GetIn:
			icon = kIconNeedPickup;
			break;
		default:
			break;
	}
	if (!shown) {
		return;
	}
	r_icon.icon = icon;
	r_icon.issuer = p_issuer;
	r_icon.icon_timer = kOrderIconSeconds;
	r_icon.blink_timer = kOrderBlinkSeconds;
}

void step_order_icon(OrderIcon &r_icon, float p_dt, bool p_have_soldier) {
	if (!p_have_soldier) {
		r_icon.icon_timer = 0.0f;
		return;
	}
	r_icon.icon_timer -= p_dt;
	r_icon.blink_timer -= p_dt;
}

int32_t standing_need_icon(const NeedParty &p_me, const NeedParty &p_mate) {
	const int32_t mine = need_of(p_me, p_mate);
	return mine != kIconNone ? mine : need_of(p_mate, p_me);
}

// SOURCE: notes R2-E2 C (the issuer's order icon while its timer runs, the Highlight variant while it blinks).
int32_t teammate_icon(const OrderIcon &p_order, uint8_t p_mate, int32_t p_standing, double p_clock) {
	if (p_order.icon_timer > 0.0f && p_order.issuer == p_mate) {
		return p_order.blink_timer > 0.0f && blink_high(p_clock) ? highlight_icon(p_order.icon) : p_order.icon;
	}
	return p_standing;
}

// SOURCE: notes R2-E1 D (0 unless Active with a spotter of the viewer's team or 0; 3 flagged, 1 squad leader, else 2).
// INFERRED: kind 1 is a squad-leader spotter, as R2-E1 D infers. Settle: R2-E1 D's squad-leader query.
uint8_t spotted_kind(const SpotKind &p_spot, uint8_t p_viewer_team, bool p_no_3d_spotting) {
	if (p_no_3d_spotting || !p_spot.active || !p_spot.spotter_known ||
			(p_spot.spotter_team != 0 && p_spot.spotter_team != p_viewer_team)) {
		return 0;
	}
	return p_spot.spotter_flagged ? 3 : (p_spot.spotter_leads_squad ? 1 : 2);
}

// SOURCE: notes R2-E1 C (an enemy on the minimap when Active, Passive or Radar and NoMinimapSpotting is off).
bool minimap_shows_enemy(spotting::SpotState p_state, bool p_no_minimap_spotting) {
	return !p_no_minimap_spotting &&
			(p_state == spotting::SpotState::Active || p_state == spotting::SpotState::Passive ||
					p_state == spotting::SpotState::Radar);
}

}
