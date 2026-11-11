#include "commo_frame.h"

#include <algorithm>
#include <array>

namespace bf3::godot_side {

namespace sp = sim::spotting;
namespace cm = sim::commo;

namespace {

const net::PlayerRow *player_row(const host::PresentationFrame &p_frame, uint8_t p_slot) {
	const size_t n = std::min<size_t>(p_frame.player_count, p_frame.players.size());
	for (size_t i = 0; i < n; ++i) {
		if (p_frame.players[i].slot == p_slot) {
			return &p_frame.players[i];
		}
	}
	return nullptr;
}

cm::Supplies kit_supplies(const host::GunplayLevel *p_gunplay, uint8_t p_team, uint8_t p_kit) {
	if (p_gunplay == nullptr || p_team < 1 || p_team > host::kKitTeams || p_kit >= host::kKitCount) {
		return {};
	}
	const host::KitLoadout &k = p_gunplay->kits.loadout(p_team, p_kit);
	return host::supplies_of(k.defs, p_gunplay->gadgets);
}

}

std::optional<sim::commo::OrderType> rose_command(const core::ui::CommoRoseEntry *p_entry, bool p_in_vehicle) {
	if (p_entry == nullptr) {
		return std::nullopt;
	}
	const int32_t c = p_entry->command(p_in_vehicle);
	if (c == core::ui::kCommoRoseNoCommand) {
		return cm::OrderType::None;
	}
	if (c <= 0 || c >= int32_t(cm::OrderType::Count)) {
		return std::nullopt;
	}
	return cm::OrderType(c);
}

sim::commo::Supplies own_supplies(const host::PresentationFrame &p_frame, const host::GunplayLevel *p_gunplay) {
	if (p_gunplay == nullptr || !p_frame.own_weapons_valid) {
		return {};
	}
	std::array<uint16_t, sim::weapons::kMaxWeaponSlots> defs{};
	for (size_t i = 0; i < defs.size(); ++i) {
		defs[i] = p_frame.own_weapons_curr.slots[i].def;
	}
	return host::supplies_of(defs, p_gunplay->gadgets);
}

size_t spot_subjects(const host::PresentationFrame &p_frame, uint64_t p_now_ns, const host::GunplayLevel *p_gunplay,
		std::span<host::SpotSubject> r_out) {
	std::array<host::PresentedSoldier, net::kMaxSlots> others{};
	const size_t n = host::present_others(p_frame, p_now_ns, others);
	const float max_health = p_gunplay != nullptr ? p_gunplay->max_health : 100.0f;
	size_t out = 0;
	for (size_t i = 0; i < n && out < r_out.size(); ++i) {
		const host::PresentedSoldier &o = others[i];
		const net::PlayerRow *row = player_row(p_frame, o.slot);
		if (row == nullptr || !row->alive || row->team == 0) {
			continue;
		}
		const net::SpotRow &spot = p_frame.spots[o.slot];
		host::SpotSubject s;
		s.slot = o.slot;
		s.team = row->team;
		if (p_gunplay == nullptr || !host::place_spot_body(s, p_gunplay->soldier, o.pose, o.position)) {
			continue;
		}
		s.party.position = o.position;
		s.party.max_health = max_health;
		s.party.health = spot.low_health ? 0.0f : max_health;
		s.party.low_ammo = spot.low_ammo;
		s.party.supplies = kit_supplies(p_gunplay, row->team, row->kit);
		r_out[out++] = s;
	}
	return out;
}

CommoFrameOutput CommoFrame::update(const CommoFrameInput &p_in) {
	CommoFrameOutput out;
	const host::PresentationFrame *f = p_in.frame;
	if (f == nullptr || p_in.gunplay == nullptr || !p_in.alive || p_in.own_slot >= net::kMaxSlots) {
		was_held_ = false;
		rose_ = false;
		selected_.reset();
		return out;
	}
	if (!spotter_) {
		core::Result<sp::SpottingDef> def = sp::make_spotting_def(p_in.gunplay->soldier);
		if (!def || !def->spotter_present) {
			return out;
		}
		spotter_.emplace(def->spotter);
	}
	std::array<host::SpotSubject, net::kMaxSlots> subjects{};
	const size_t n = spot_subjects(*f, p_in.now_ns, p_in.gunplay, subjects);
	host::SpotterInput in;
	in.clock = p_in.now_s;
	in.dt = p_in.dt;
	in.own_slot = p_in.own_slot;
	in.own_team = p_in.own_team;
	in.eye = p_in.eye;
	in.view = p_in.view;
	in.me.position = f->own_curr.position;
	in.me.max_health = p_in.gunplay->max_health;
	in.me.health = f->own_curr.health;
	in.me.low_ammo = f->spots[p_in.own_slot].low_ammo;
	in.me.supplies = own_supplies(*f, p_in.gunplay);
	in.subjects = std::span<const host::SpotSubject>(subjects.data(), n);
	in.states = f->spots;
	in.world = p_in.world;

	const bool pressed = p_in.held && !was_held_;
	const bool released = !p_in.held && was_held_;
	if (pressed) {
		pressed_at_ = p_in.now_s;
		++stats_.presses;
	}
	if (p_in.held && !rose_ && p_in.now_s - pressed_at_ >= double(kCommoRoseHoldSeconds)) {
		rose_ = true;
		selected_.reset();
		++stats_.rose_opens;
	}
	bool press = pressed;
	if (released && rose_) {
		rose_ = false;
		++stats_.rose_picks;
		stats_.last_pick = selected_ ? int32_t(*selected_) : -1;
		if (selected_ == cm::OrderType::None) {
			press = true;
		} else if (selected_) {
			const std::optional<uint8_t> mate = host::order_teammate(spotter_->def(), in);
			out.order = net::OrderRequest{ *selected_, mate.value_or(net::kNoOrderTarget) };
		}
		selected_.reset();
	}
	was_held_ = p_in.held;
	in.pressed = press;
	host::SpotterOutput s = spotter_->update(in);
	stats_.refused += s.refused ? 1u : 0u;
	stats_.scans += s.scanned ? 1u : 0u;
	if (s.spot) {
		out.spot = std::move(s.spot);
		++stats_.spots_sent;
	}
	if (!out.order && s.order) {
		out.order = s.order;
	}
	stats_.orders_sent += out.order ? 1u : 0u;
	return out;
}

}
