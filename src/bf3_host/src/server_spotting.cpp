#include "server_spotting.h"

#include "bf3/host/spotter.h"

#include <algorithm>
#include <array>

namespace bf3::host {

namespace sp = sim::spotting;
namespace cm = sim::commo;

namespace {

bool alive_at(std::span<const uint8_t> p_alive, uint8_t p_slot) {
	return p_slot < p_alive.size() && p_alive[p_slot] != 0;
}

}

sim::commo::Supplies soldier_supplies(const Gunplay &p_gunplay, uint8_t p_slot) {
	const sim::weapons::SoldierWeaponsState &w = p_gunplay.weapons(p_slot);
	std::array<uint16_t, sim::weapons::kMaxWeaponSlots> defs{};
	for (size_t i = 0; i < defs.size(); ++i) {
		defs[i] = w.slots[i].def;
	}
	return supplies_of(defs, p_gunplay.level().gadgets);
}

bool soldier_low_ammo(const Gunplay &p_gunplay, uint8_t p_slot, float p_need_percent) {
	const sim::weapons::SoldierWeaponsState &w = p_gunplay.weapons(p_slot);
	const sim::weapons::WeaponTable &table = p_gunplay.table();
	std::vector<cm::WeaponAmmo> ammo;
	for (size_t i = 0; i < 2; ++i) {
		const sim::weapons::WeaponSlotState &s = w.slots[i];
		if (s.def == sim::weapons::kNoWeapon || s.def >= table.defs.size()) {
			continue;
		}
		const sim::weapons::WeaponSimDef &d = table.def(s.def, s.variant);
		if (d.unlimited_ammo) {
			continue;
		}
		ammo.push_back({ float(std::max(s.magazine, 0)), float(std::max(s.reserve, 0)), float(d.magazine_capacity),
				float(d.magazines), true });
	}
	return cm::low_ammo(ammo, p_need_percent);
}

void ServerSpotting::configure(size_t p_slots, const sim::spotting::SpottingDef &p_def) {
	def_ = p_def;
	targets_.assign(p_slots, sp::SpotTarget{});
	low_ammo_.assign(p_slots, 0);
	low_health_.assign(p_slots, 0);
	sent_.assign(p_slots, net::SpotRow{});
	orders_.assign(p_slots, StoredOrder{});
	bots_.assign(p_slots, std::nullopt);
	queued_orders_.clear();
	for (size_t i = 0; i < p_slots; ++i) {
		sent_[i].slot = uint8_t(i);
	}
	low_ammo_timer_ = 0;
}

void ServerSpotting::reset(size_t p_slot) {
	if (p_slot >= targets_.size()) {
		return;
	}
	targets_[p_slot] = sp::SpotTarget{};
	low_ammo_[p_slot] = 0;
	low_health_[p_slot] = 0;
	orders_[p_slot] = StoredOrder{};
	bots_[p_slot].reset();
	for (sp::SpotTarget &t : targets_) {
		if (t.spotter == p_slot) {
			t.spotter = sp::kNoSpotter;
		}
	}
}

void ServerSpotting::spot(uint8_t p_spotter, const net::SpotRequest &p_request, std::span<const uint8_t> p_alive) {
	++stats_.spot_requests;
	if (p_request.type != uint8_t(sp::SpotType::Active) && p_request.type != uint8_t(sp::SpotType::Passive)) {
		++stats_.unhandled_spot_types;
		return;
	}
	if (!def_.target_present || !alive_at(p_alive, p_spotter)) {
		stats_.spot_targets_refused += p_request.targets.size();
		return;
	}
	for (const uint8_t t : p_request.targets) {
		if (t >= targets_.size() || t == p_spotter || !alive_at(p_alive, t)) {
			++stats_.spot_targets_refused;
			continue;
		}
		const bool applied = p_request.type == uint8_t(sp::SpotType::Active)
				? sp::apply_active(targets_[t], def_.target, p_spotter)
				: sp::apply_passive(targets_[t], def_.target, p_spotter, false);
		stats_.spots_applied += applied ? 1u : 0u;
	}
}

std::optional<OrderDelivery> ServerSpotting::order(
		uint8_t p_issuer, const net::OrderRequest &p_request, std::span<const uint8_t> p_alive) {
	++stats_.order_requests;
	if (p_request.target == net::kNoOrderTarget || p_request.order == cm::OrderType::None) {
		return std::nullopt;
	}
	if (p_request.target >= orders_.size() || p_request.target == p_issuer || !alive_at(p_alive, p_issuer) ||
			!alive_at(p_alive, p_request.target)) {
		++stats_.orders_refused;
		return std::nullopt;
	}
	StoredOrder &o = orders_[p_request.target];
	o.serial = ++order_serial_;
	o.type = p_request.order;
	o.issuer = p_issuer;
	++stats_.orders_stored;
	return OrderDelivery{ p_request.target, net::OrderNotice{ p_request.order, p_issuer } };
}

void ServerSpotting::step(float p_dt, std::span<const uint8_t> p_alive) {
	for (size_t i = 0; i < targets_.size(); ++i) {
		if (!alive_at(p_alive, uint8_t(i))) {
			targets_[i] = sp::SpotTarget{};
			continue;
		}
		sp::step_target(targets_[i], p_dt);
	}
}

void ServerSpotting::step_bots(double p_clock, float p_dt, std::span<const BotEye> p_bots,
		std::span<const SpotSubject> p_subjects, const sim::CollisionWorld *p_world, std::span<const uint8_t> p_alive) {
	if (!def_.spotter_present) {
		return;
	}
	rows_.resize(targets_.size());
	for (size_t i = 0; i < targets_.size(); ++i) {
		rows_[i] = row(uint8_t(i));
	}
	for (const BotEye &b : p_bots) {
		if (b.slot >= bots_.size() || !alive_at(p_alive, b.slot)) {
			continue;
		}
		std::optional<Spotter> &s = bots_[b.slot];
		if (!s) {
			s.emplace(def_.spotter);
		}
		SpotterInput in;
		in.clock = p_clock;
		in.dt = p_dt;
		in.own_slot = b.slot;
		in.own_team = b.team;
		in.eye = b.eye;
		in.view = b.view;
		in.subjects = p_subjects;
		in.states = rows_;
		in.world = p_world;
		for (const SpotSubject &e : p_subjects) {
			if (e.team != 0 && e.team != b.team && targets_[e.slot].state != sp::SpotState::Active &&
					sp::in_spot_cone(b.eye, b.view, e.centre, def_.spotter.fov_deg, def_.spotter.distance)) {
				in.pressed = true;
				break;
			}
		}
		const SpotterOutput out = s->update(in);
		if (out.spot) {
			spot(b.slot, *out.spot, p_alive);
		}
	}
}

void ServerSpotting::queue_order(uint8_t p_slot, const net::OrderRequest &p_request) {
	if (queued_orders_.size() < 4u * targets_.size()) {
		queued_orders_.push_back({ p_slot, p_request });
	}
}

std::vector<std::pair<uint8_t, net::OrderRequest>> ServerSpotting::take_queued_orders() {
	std::vector<std::pair<uint8_t, net::OrderRequest>> out;
	out.swap(queued_orders_);
	return out;
}

void ServerSpotting::set_low_ammo(uint8_t p_slot, bool p_low) {
	if (p_slot < low_ammo_.size()) {
		low_ammo_[p_slot] = p_low ? 1 : 0;
	}
}

void ServerSpotting::set_low_health(uint8_t p_slot, bool p_low) {
	if (p_slot < low_health_.size()) {
		low_health_[p_slot] = p_low ? 1 : 0;
	}
}

bool ServerSpotting::low_ammo_due(float p_dt) {
	low_ammo_timer_ -= p_dt;
	if (low_ammo_timer_ > 0.0f) {
		return false;
	}
	low_ammo_timer_ += sp::kLowAmmoCheckSeconds;
	return true;
}

net::SpotRow ServerSpotting::row(uint8_t p_slot) const {
	const sp::SpotTarget &t = targets_[p_slot];
	return net::SpotRow{ p_slot, t.state, t.state == sp::SpotState::None ? sp::kNoSpotter : t.spotter,
		low_ammo_[p_slot] != 0, low_health_[p_slot] != 0 };
}

std::optional<net::SpotStates> ServerSpotting::take_changes(sim::Tick p_tick) {
	net::SpotStates out;
	out.server_tick = p_tick;
	for (size_t i = 0; i < targets_.size(); ++i) {
		const net::SpotRow now = row(uint8_t(i));
		if (now != sent_[i]) {
			out.rows.push_back(now);
			sent_[i] = now;
		}
	}
	if (out.rows.empty()) {
		return std::nullopt;
	}
	++stats_.state_messages;
	return out;
}

net::SpotStates ServerSpotting::full(sim::Tick p_tick) const {
	net::SpotStates out;
	out.server_tick = p_tick;
	out.full = true;
	for (size_t i = 0; i < targets_.size(); ++i) {
		out.rows.push_back(row(uint8_t(i)));
	}
	return out;
}

}
