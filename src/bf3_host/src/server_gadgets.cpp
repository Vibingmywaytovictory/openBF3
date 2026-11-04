#include "server_gadgets.h"

#include "bf3/core/util/fnv.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/squads.h"

#include <algorithm>
#include <cmath>

namespace bf3::host {

namespace {

namespace sw = sim::weapons;

void hash_unit(uint64_t &r_h, const sw::SupplyUnitDef &p_u) {
	core::fnv1a_value(r_h, p_u.radius);
	core::fnv1a_value(r_h, p_u.inc_speed);
	core::fnv1a_value(r_h, p_u.capacity);
	core::fnv1a_value(r_h, p_u.refill_speed);
	core::fnv1a_value(r_h, p_u.infinite);
}

}

uint64_t gadget_defs_hash(std::span<const sw::GadgetDef> p_gadgets, std::span<const sw::AmmoPickupDef> p_pickups) {
	uint64_t h = core::kFnv1aOffset;
	core::fnv1a_value(h, uint64_t(p_gadgets.size()));
	for (const sw::GadgetDef &g : p_gadgets) {
		core::fnv1a_value(h, uint8_t(g.kind));
		core::fnv1a_value(h, g.def);
		hash_unit(h, g.healing);
		hash_unit(h, g.ammo);
		core::fnv1a_value(h, g.team_specific);
		core::fnv1a_value(h, g.exclude_self);
		core::fnv1a_value(h, g.supply_soldiers);
		core::fnv1a_value(h, g.max_count);
		core::fnv1a_value(h, g.max_attach_inclination_deg);
		core::fnv1a_value(h, g.ground_ray_m);
		core::fnv1a_value(h, g.ground_flatness);
	}
	core::fnv1a_value(h, uint64_t(p_pickups.size()));
	for (const sw::AmmoPickupDef &p : p_pickups) {
		core::fnv1a_value(h, p.amount);
		core::fnv1a_value(h, p.delay_multiplier);
		core::fnv1a_value(h, p.auto_replenish);
		core::fnv1a_value(h, p.auto_replenish_delay);
	}
	return h;
}

ServerGadgets::ServerGadgets(const GunplayLevel &p_level, std::shared_ptr<const sw::WeaponTable> p_table) :
		level_(&p_level), table_(std::move(p_table)), field_(p_level.gadgets) {}

sw::DeployOutcome ServerGadgets::deploy(
		const sw::ShotEvent &p_shot, uint8_t p_team, sim::Tick p_tick, const sim::CollisionWorld *p_world) {
	sw::GadgetThrow t;
	t.def = p_shot.def;
	t.owner = uint8_t(std::min<uint32_t>(p_shot.player_slot, 255u));
	t.team = p_team;
	t.tick = p_tick;
	t.origin = p_shot.origin;
	t.direction = p_shot.direction;
	if (table_ != nullptr && p_shot.def < table_->defs.size()) {
		const sw::ProjectileSimDef &d = table_->def(p_shot.def, p_shot.variant).projectile;
		t.launch_velocity = d.launch_velocity;
	}
	const sw::DeployOutcome o = field_.deploy(t, p_world);
	if (o == sw::DeployOutcome::Thrown || o == sw::DeployOutcome::Placed) {
		++stats_.deploys;
	} else {
		++stats_.refused;
	}
	return o;
}

void ServerGadgets::armed(uint8_t p_slot, const sw::SoldierWeaponsState &p_weapons) {
	if (p_slot < refill_.size()) {
		sw::arm_ammo_refill(refill_[p_slot], p_weapons);
	}
}

void ServerGadgets::step(sim::Tick p_tick, const sim::CollisionWorld *p_world,
		std::span<const GadgetSoldier> p_soldiers, float p_max_health) {
	if (p_world != nullptr) {
		field_.step(*p_world);
	}
	receivers_.clear();
	receiver_slots_.clear();
	for (size_t i = 0; i < p_soldiers.size() && i < refill_.size(); ++i) {
		const GadgetSoldier &s = p_soldiers[i];
		if (s.state == nullptr || s.weapons == nullptr) {
			continue;
		}
		if (table_ != nullptr) {
			stats_.rounds_replenished +=
					uint64_t(sw::replenish_ammo(*s.weapons, *table_, level_->ammo_pickups, p_tick, refill_[i]));
		}
		receivers_.push_back(sw::SupplyReceiver{ uint8_t(i), s.team, true, s.state->position });
		receiver_slots_.push_back(uint8_t(i));
	}
	grants_.assign(receivers_.size(), sw::SupplyGrant());
	if (field_.live().empty()) {
		return;
	}
	field_.supply(receivers_, sim::kTickSeconds, grants_);
	for (size_t k = 0; k < receivers_.size(); ++k) {
		const GadgetSoldier &s = p_soldiers[receiver_slots_[k]];
		const sw::SupplyGrant &g = grants_[k];
		if (g.health > 0.0f && s.state->health < p_max_health) {
			const float before = s.state->health;
			s.state->health = std::min(p_max_health, s.state->health + g.health);
			stats_.healed += double(s.state->health - before);
			++stats_.heal_ticks;
		}
		if (g.ammo > 0.0f && table_ != nullptr) {
			++stats_.ammo_ticks;
			stats_.rounds_refilled += uint64_t(
					sw::refill_ammo(*s.weapons, *table_, level_->ammo_pickups, g.ammo, refill_[receiver_slots_[k]]));
		}
	}
}

void ServerGadgets::forget(uint8_t p_slot) {
	stats_.forgotten += field_.remove_owner(p_slot);
	if (p_slot < refill_.size()) {
		refill_[p_slot] = sw::AmmoRefillState();
	}
}

void ServerGadgets::clear() {
	field_.clear();
}

std::optional<sim::Vec3> ServerGadgets::squad_beacon(uint8_t p_slot, uint8_t p_team, const SquadTable *p_squads) const {
	for (const sw::Deployable &d : field_.live()) {
		const sw::GadgetDef &g = field_.defs()[d.gadget];
		if (g.kind != sw::GadgetKind::SpawnBeacon || !d.resting || d.team != p_team) {
			continue;
		}
		if (d.owner == p_slot || (p_squads != nullptr && p_squads->same_squad(p_slot, d.owner))) {
			return d.position;
		}
	}
	return std::nullopt;
}

const GadgetStats &ServerGadgets::stats() const {
	stats_.field = field_.stats();
	return stats_;
}

}
