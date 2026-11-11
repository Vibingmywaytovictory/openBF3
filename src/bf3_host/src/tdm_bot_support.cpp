#include "tdm_bot_support.h"

#include "bf3/host/gunplay.h"
#include "bf3/host/kits.h"
#include "bf3/sim/commo.h"
#include "bf3/sim/spotting.h"
#include "bf3/sim/weapons/gadgets.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace bf3::host::tdm_bot {

namespace {

namespace sw = sim::weapons;

constexpr sim::Tick kCallLiveTicks = 30;

bool healing(SupplyNeed p_need) {
	return p_need == SupplyNeed::Health;
}

const sw::GadgetDef *gadget_of(const GunplayLevel &p_level, uint16_t p_def) {
	if (p_def == sw::kNoWeapon) {
		return nullptr;
	}
	for (const sw::GadgetDef &g : p_level.gadgets) {
		if (g.def == p_def) {
			return &g;
		}
	}
	return nullptr;
}

bool calls(const TdmShared &p_shared, size_t p_slot, sim::Tick p_tick) {
	return p_slot < p_shared.medic_call.size() && p_shared.medic_call[p_slot] != 0 &&
			p_tick - p_shared.medic_call[p_slot] <= kCallLiveTicks;
}

size_t soldiers(const TdmShared &p_shared, std::span<const sim::SoldierState> p_states) {
	return std::min({ p_states.size(), p_shared.teams.size(), p_shared.alive.size() });
}

}

bool may_help(const ai::BehaviourTuning &p_tuning, const SupportWay &p_way, const sim::Vec3 &p_self,
		const sim::Vec3 &p_mate, size_t p_mate_slot) {
	if (!p_way.point.has_value()) {
		return !p_way.following || (p_mate_slot < 64 && ((p_way.squad_mates >> p_mate_slot) & 1u) != 0);
	}
	const sim::Vec3 &goal = *p_way.point;
	const float straight = horizontal(p_self, goal);
	return straight <= p_way.free_within_m ||
			horizontal(p_self, p_mate) + horizontal(p_mate, goal) - straight <= p_tuning.order_detour_m;
}

uint8_t supply_slot(const Gunplay &p_gunplay, const BotArms &p_arms, SupplyNeed p_need) {
	const sw::SoldierWeaponsState *ws = p_arms.weapons();
	if (ws == nullptr) {
		return kNoSupplySlot;
	}
	for (const uint8_t s : kGadgetSlots) {
		const sw::GadgetDef *g = gadget_of(p_gunplay.level(), ws->slots[s].def);
		if (g != nullptr && g->kind == sw::GadgetKind::Supply && g->supply_soldiers &&
				(healing(p_need) ? g->healing : g->ammo).active()) {
			return s;
		}
	}
	return kNoSupplySlot;
}

bool needs_supply(const Gunplay &p_gunplay, const ai::BehaviourTuning &p_tuning, uint8_t p_slot,
		const sim::SoldierState &p_state, SupplyNeed p_need) {
	if (healing(p_need)) {
		return p_state.health > 0.0f && p_state.health < p_tuning.assist_below_hp;
	}
	const BotArms arms(&p_gunplay, p_slot);
	const sw::WeaponSimDef *d = arms.def(kSlotPrimary);
	if (d == nullptr || d->unlimited_ammo || d->magazine_capacity <= 0) {
		return false;
	}
	const int32_t used = int32_t(p_tuning.resupply_used_mags) * d->magazine_capacity;
	return arms.weapons()->slots[kSlotPrimary].reserve <= d->magazines * d->magazine_capacity - used;
}

bool supplied(const Gunplay &p_gunplay, uint8_t p_slot, uint8_t p_team, const sim::Vec3 &p_at, SupplyNeed p_need,
		int p_owner) {
	const std::span<const sw::GadgetDef> defs = p_gunplay.level().gadgets;
	const sw::SupplyReceiver receiver{ p_slot, p_team, true, p_at };
	for (const sw::Deployable &d : p_gunplay.deployables()) {
		if (d.gadget >= defs.size() || (p_owner >= 0 && (d.owner_gone || int(d.owner) != p_owner))) {
			continue;
		}
		if (sw::supplies(defs[d.gadget], d, receiver, healing(p_need))) {
			return true;
		}
	}
	return false;
}

namespace {

std::optional<uint8_t> nearest_medic(const TdmShared &p_shared, uint8_t p_slot, float p_within) {
	const TdmShared &sh = p_shared;
	const std::span<const sim::SoldierState> states = sh.host.view->states();
	if (sh.host.gunplay == nullptr || p_slot >= states.size() || p_slot >= sh.teams.size()) {
		return std::nullopt;
	}
	const Gunplay &g = *sh.host.gunplay;
	std::optional<uint8_t> best;
	float best_d = p_within * p_within;
	const size_t n = std::min(states.size(), sh.teams.size());
	for (size_t s = 0; s < n; ++s) {
		if (s == p_slot || sh.teams[s] == 0 || sh.teams[s] != sh.teams[p_slot] || sh.alive[s] == 0 ||
				supply_slot(g, BotArms(&g, uint8_t(s)), SupplyNeed::Health) == kNoSupplySlot) {
			continue;
		}
		const sim::Vec3 d = core::vec::sub(states[s].position, states[p_slot].position);
		const float dd = core::vec::dot(d, d);
		if (dd < best_d) {
			best_d = dd;
			best = uint8_t(s);
		}
	}
	return best;
}

void ask_medic(TdmShared &r_shared, uint8_t p_slot) {
	const TdmShared &p_shared = r_shared;
	if (p_shared.host.gunplay == nullptr) {
		return;
	}
	const core::Result<sim::spotting::SpottingDef> def =
			sim::spotting::make_spotting_def(p_shared.host.gunplay->level().soldier);
	if (!def || !def->spotter_present) {
		return;
	}
	const std::optional<uint8_t> medic = nearest_medic(p_shared, p_slot, def->spotter.pickup_order_distance);
	p_shared.host.view->post_order(
			p_slot, net::OrderRequest{ sim::commo::OrderType::RequestHeal, medic.value_or(net::kNoOrderTarget) });
	r_shared.count(&TdmBotStats::medic_orders, uint64_t(1));
}

}

void BotSupport::new_life() {
	targets_ = {};
	since_ = {};
	pursued_ = {};
	calling_ = false;
	posted_ = false;
	medkit_.reset();
}

const sim::Vec3 *BotSupport::target(SupplyNeed p_need) const {
	const Target &t = targets_[size_t(p_need)];
	return t.slot >= 0 ? &t.at : nullptr;
}

BotSupport::Target BotSupport::choose(
		const TdmShared &p_shared, SupplyNeed p_need, const SupportCheck &p_check, int p_keep) const {
	const TdmShared &sh = p_shared;
	const ai::BehaviourTuning &b = sh.config->behaviours;
	const Gunplay &g = *sh.host.gunplay;
	const SupportCheck &q = p_check;
	const uint8_t team = sh.teams[q.slot];
	Target best;
	float best_d = std::numeric_limits<float>::max();
	const size_t n = soldiers(sh, q.states);
	for (size_t s = 0; s < n; ++s) {
		if (s == q.slot || team == 0 || sh.teams[s] != team || sh.alive[s] == 0 || !finite(q.states[s].position)) {
			continue;
		}
		const bool called = healing(p_need) && calls(sh, s, q.tick);
		const float d = horizontal(q.self->position, q.states[s].position);
		if (d > (called ? b.call_radius_m : b.assist_radius_m) ||
				!may_help(b, q.way, q.self->position, q.states[s].position, s)) {
			continue;
		}
		if (!needs_supply(g, b, uint8_t(s), q.states[s], p_need) ||
				supplied(g, uint8_t(s), team, q.states[s].position, p_need)) {
			continue;
		}
		const float rank = int(s) == p_keep ? 0.5f * d : d;
		if (rank < best_d) {
			best_d = rank;
			best = Target{ int(s), q.states[s].position, called };
		}
	}
	return best;
}

bool BotSupport::own_bag_busy(const TdmShared &p_shared, SupplyNeed p_need, const SupportCheck &p_check) const {
	const TdmShared &sh = p_shared;
	const Gunplay &g = *sh.host.gunplay;
	const SupportCheck &q = p_check;
	const uint8_t team = sh.teams[q.slot];
	bool own = false;
	for (const sw::Deployable &d : g.deployables()) {
		own = own || (d.owner == q.slot && d.resting && !d.owner_gone);
	}
	if (!own) {
		return false;
	}
	const size_t n = soldiers(sh, q.states);
	for (size_t s = 0; s < n; ++s) {
		if (team != 0 && sh.teams[s] == team && sh.alive[s] != 0 &&
				needs_supply(g, sh.config->behaviours, uint8_t(s), q.states[s], p_need) &&
				supplied(g, uint8_t(s), team, q.states[s].position, p_need, q.slot)) {
			return true;
		}
	}
	return false;
}

SupportSense BotSupport::update(TdmShared &r_shared, const BotArms &p_arms, const SupportCheck &p_check) {
	TdmShared &sh = r_shared;
	const SupportCheck &q = p_check;
	SupportSense out;
	if (sh.host.gunplay == nullptr || q.self == nullptr || q.slot >= sh.teams.size() || q.slot >= sh.alive.size()) {
		targets_ = {};
		slots_ = { kNoSupplySlot, kNoSupplySlot };
		calling_ = false;
		posted_ = false;
		medkit_.reset();
		return out;
	}
	const Gunplay &g = *sh.host.gunplay;
	const ai::BehaviourTuning &b = sh.config->behaviours;
	const uint32_t assist_ticks = sim::seconds_to_ticks(b.assist_s);
	for (const SupplyNeed need : { SupplyNeed::Health, SupplyNeed::Ammo }) {
		const size_t k = size_t(need);
		slots_[k] = supply_slot(g, p_arms, need);
		Target &t = targets_[k];
		const int keep = t.slot;
		if (q.engaged || q.tick < rethrow_until_ || slots_[k] == kNoSupplySlot || p_arms.rounds(slots_[k]) <= 0 ||
				own_bag_busy(sh, need, q)) {
			t = Target{};
			continue;
		}
		t = choose(sh, need, q, keep);
		if (t.slot >= 0 && (t.slot != keep || pursued_[k] < updated_)) {
			since_[k] = q.tick;
		}
		if (t.slot >= 0 && q.tick - since_[k] > assist_ticks) {
			rethrow_until_ = q.tick + sim::seconds_to_ticks(b.rethrow_s);
			sh.count(&TdmBotStats::assists_given_up, uint64_t(1));
			t = Target{};
		}
	}
	out.heal = targets_[size_t(SupplyNeed::Health)].slot >= 0;
	out.supply = targets_[size_t(SupplyNeed::Ammo)].slot >= 0;
	out.call = update_call(sh, q);
	updated_ = q.tick;
	return out;
}

bool BotSupport::update_call(TdmShared &r_shared, const SupportCheck &p_check) {
	TdmShared &sh = r_shared;
	const SupportCheck &q = p_check;
	const ai::BehaviourTuning &b = sh.config->behaviours;
	const Gunplay &g = *sh.host.gunplay;
	const float hurt_below = calling_ ? b.assist_below_hp : b.call_below_hp;
	medkit_.reset();
	if (q.engaged || slots_[size_t(SupplyNeed::Health)] != kNoSupplySlot || !(q.self->health > 0.0f) ||
			q.self->health >= hurt_below) {
		calling_ = false;
		posted_ = false;
		return false;
	}
	if (q.tick < call_blocked_until_) {
		return false;
	}
	const uint8_t team = sh.teams[q.slot];
	const std::span<const sw::GadgetDef> defs = g.level().gadgets;
	const bool covered = supplied(g, q.slot, team, q.self->position, SupplyNeed::Health);
	bool medic = covered;
	if (!covered) {
		float best = b.call_radius_m;
		for (const sw::Deployable &d : g.deployables()) {
			if (d.gadget >= defs.size() || !d.resting || !defs[d.gadget].healing.active() ||
					(defs[d.gadget].team_specific && d.team != team)) {
				continue;
			}
			const float m = horizontal(q.self->position, d.position);
			if (m <= best) {
				best = m;
				medkit_ = d.position;
			}
		}
		medic = medkit_.has_value();
	}
	const size_t n = soldiers(sh, q.states);
	for (size_t s = 0; s < n && !medic; ++s) {
		if (s == q.slot || team == 0 || sh.teams[s] != team || sh.alive[s] == 0 ||
				horizontal(q.self->position, q.states[s].position) > b.call_radius_m) {
			continue;
		}
		medic = supply_slot(g, BotArms(&g, uint8_t(s)), SupplyNeed::Health) != kNoSupplySlot;
	}
	if (!medic) {
		calling_ = false;
		posted_ = false;
		return false;
	}
	calling_ = true;
	const uint32_t call_ticks = sim::seconds_to_ticks(b.call_s);
	if (!covered && posted_ && q.tick - call_since_ >= call_ticks) {
		calling_ = false;
		posted_ = false;
		medkit_.reset();
		call_blocked_until_ = q.tick + 2u * call_ticks;
		return false;
	}
	return true;
}

void BotSupport::thrown(TdmShared &r_shared, sim::Tick p_tick, uint8_t p_weapon_slot) {
	TdmShared &sh = r_shared;
	rethrow_until_ = p_tick + sim::seconds_to_ticks(sh.config->behaviours.rethrow_s);
	for (const SupplyNeed need : { SupplyNeed::Health, SupplyNeed::Ammo }) {
		const Target &t = targets_[size_t(need)];
		if (slots_[size_t(need)] != p_weapon_slot || t.slot < 0) {
			continue;
		}
		sh.count(healing(need) ? &TdmBotStats::medkit_throws : &TdmBotStats::ammo_throws, uint64_t(1));
		if (t.called) {
			sh.count(&TdmBotStats::calls_answered, uint64_t(1));
		}
	}
}

void BotSupport::post_call(TdmShared &r_shared, sim::Tick p_tick, uint8_t p_slot) {
	if (!posted_) {
		posted_ = true;
		call_since_ = p_tick;
		r_shared.count(&TdmBotStats::medic_calls, uint64_t(1));
		ask_medic(r_shared, p_slot);
	}
	if (p_slot < r_shared.medic_call.size()) {
		r_shared.medic_call[p_slot] = p_tick;
	}
}

}
