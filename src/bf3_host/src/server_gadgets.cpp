#include "server_gadgets.h"

#include "bf3/core/math.h"
#include "bf3/core/util/fnv.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/squads.h"
#include "bf3/sim/weapons/spread_recoil.h"
#include "server_ghosts.h"

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
		core::fnv1a_value(h, g.pack.remote_detonator);
		core::fnv1a_value(h, g.pack.vehicle_detonation);
		core::fnv1a_value(h, g.pack.vehicle_radius);
		core::fnv1a_value(h, g.pack.vehicle_delay_s);
		core::fnv1a_value(h, g.pack.soldier_detonation);
		core::fnv1a_value(h, g.pack.soldier_radius);
		core::fnv1a_value(h, g.pack.soldier_angle_deg);
		core::fnv1a_value(h, g.pack.soldier_min_speed);
		core::fnv1a_value(h, g.pack.soldier_delay_s);
		core::fnv1a_value(h, g.pack.use_angle);
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
	tick_ = p_tick;
	if (p_world != nullptr) {
		field_.step(*p_world);
	}
	field_.expire(p_tick);
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

void ServerGadgets::blast(std::span<const sw::DetonationEvent> p_detonations, const sim::CollisionWorld *p_world,
		std::span<const uint8_t> p_teams, bool p_friendly_fire) {
	if (table_ == nullptr) {
		return;
	}
	const sw::Mechanics &mech = table_->mechanics;
	for (const sw::DetonationEvent &d : p_detonations) {
		if (field_.live().empty()) {
			return;
		}
		if (d.def >= table_->defs.size()) {
			continue;
		}
		const sw::ProjectileSimDef &p = table_->def(d.def, d.variant).projectile;
		const uint8_t team = !p_friendly_fire && d.player_slot < p_teams.size() ? p_teams[d.player_slot] : 0;
		field_.blast(d.dud ? p.dud : p.explosion, mech.blast_falloff, d.position, d.normal,
				mech.blast_occlusion ? p_world : nullptr, team);
	}
}

void ServerGadgets::detonator(uint8_t p_slot, uint16_t p_def, bool p_pressed) {
	if (p_slot >= detonator_held_.size()) {
		return;
	}
	const bool edge = p_pressed && !detonator_held_[p_slot];
	detonator_held_[p_slot] = p_pressed;
	const sw::GadgetDef *g = field_.gadget_of(p_def);
	if (edge && g != nullptr && g->kind == sw::GadgetKind::ExplosionPack && g->pack.remote_detonator) {
		stats_.remote_detonations += field_.remote_detonate(p_slot, p_def);
	}
}

void ServerGadgets::detonate_packs(sim::Tick p_tick, std::span<const sim::SoldierState> p_states,
		std::span<const uint8_t> p_teams, std::span<const sw::PackVehicle> p_vehicles,
		const sim::CollisionWorld &p_world, const sw::HitTargetProvider &p_targets, sw::ProjectileStepOutput &r_out) {
	ownerless_.clear();
	if (table_ == nullptr || (field_.live().empty() && pending_.empty())) {
		return;
	}
	pack_soldiers_.clear();
	for (size_t i = 0; i < p_states.size() && i < p_teams.size(); ++i) {
		const sim::SoldierState &s = p_states[i];
		if ((s.flags & sim::soldier_flags::kAlive) == 0 || s.mode == sim::MoveMode::Dead) {
			continue;
		}
		pack_soldiers_.push_back(sw::PackSoldier{ uint8_t(i), p_teams[i], s.position, core::vec::length(s.velocity) });
	}
	if (sw::starts_sway_tick(p_tick)) {
		pack_vehicles_.assign(p_vehicles.begin(), p_vehicles.end());
		for (sw::PackVehicle &v : pack_vehicles_) {
			v.driver_team = v.driver < p_teams.size() ? p_teams[v.driver] : uint8_t(0);
		}
		field_.arm_packs(pack_soldiers_, pack_vehicles_, sw::kSwaySeconds);
	}
	const sw::SimTime now = sw::tick_end(p_tick);
	for (const sw::Deployable &d : field_.take_detonating()) {
		PendingBlast b;
		b.event.time = now + (d.def < table_->defs.size() ? table_->defs[d.def].projectile.explosion.spawn_delay : 0);
		b.event.position = core::vec::add(d.position, core::vec::scale(d.normal, sw::kBlastEyeLift));
		b.event.normal = d.normal;
		b.event.player_slot = d.owner;
		b.event.shot_index = kPackShotIndex | d.id;
		b.event.def = d.def;
		b.owner_left = d.owner_left;
		pending_.push_back(b);
	}
	core::gameplay::WorkBudget budget(kPackBlastWork);
	for (size_t i = 0; i < pending_.size();) {
		const PendingBlast b = pending_[i];
		if (b.event.time > now) {
			++i;
			continue;
		}
		pending_.erase(pending_.begin() + std::ptrdiff_t(i));
		sw::explode(*table_, p_world, p_targets, b.event, budget, r_out);
		++stats_.pack_detonations;
		if (b.owner_left) {
			ownerless_.push_back(b.event.shot_index);
		}
	}
}

bool ServerGadgets::ownerless(uint32_t p_shot_index) const {
	return std::find(ownerless_.begin(), ownerless_.end(), p_shot_index) != ownerless_.end();
}

void ServerGadgets::forget(uint8_t p_slot) {
	field_.orphan_packs(p_slot, tick_);
	stats_.forgotten += field_.remove_owner(p_slot);
	if (p_slot < refill_.size()) {
		refill_[p_slot] = sw::AmmoRefillState();
	}
}

void ServerGadgets::owner_left(uint8_t p_slot) {
	stats_.forgotten += field_.owner_gone(p_slot, tick_);
	if (p_slot < refill_.size()) {
		refill_[p_slot] = sw::AmmoRefillState();
	}
}

void ServerGadgets::owner_died(uint8_t p_slot) {
	field_.orphan_packs(p_slot, tick_);
}

void ServerGadgets::clear() {
	field_.clear();
	pending_.clear();
	ownerless_.clear();
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

void DeployableSync::configure(size_t p_slots, bool p_scoped) {
	viewers_.assign(p_slots, Viewer());
	scoped_ = p_scoped;
}

void DeployableSync::reset(size_t p_slot) {
	if (p_slot < viewers_.size()) {
		viewers_[p_slot] = Viewer();
	}
}

std::optional<std::vector<uint8_t>> DeployableSync::update(size_t p_slot, uint8_t p_team, const sim::Vec3 &p_position,
		std::span<const sw::Deployable> p_live, uint64_t p_revision, sim::Tick p_tick) {
	if (p_slot >= viewers_.size()) {
		return std::nullopt;
	}
	Viewer &v = viewers_[p_slot];
	if (v.revision == p_revision && p_tick - v.checked < kDeployableScopeTicks) {
		return std::nullopt;
	}
	v.revision = p_revision;
	v.checked = p_tick;
	GhostViewer viewer;
	viewer.slot = uint8_t(p_slot);
	viewer.team = p_team;
	viewer.spawned = true;
	viewer.position = p_position;
	viewer.eye = p_position;
	std::vector<uint16_t> scope;
	net::Deployables m;
	m.server_tick = p_tick;
	for (const sw::Deployable &d : p_live) {
		if (!d.resting) {
			continue;
		}
		GhostCandidate c;
		c.slot = d.owner;
		c.team = d.team;
		c.position = d.position;
		if (scoped_ && !ghost_tier(viewer, c, false)) {
			++stats_.out_of_scope;
			continue;
		}
		scope.push_back(d.id);
		if (!std::binary_search(v.known.begin(), v.known.end(), d.id) &&
				m.added.size() < net::kMaxDeployablesPerMessage) {
			m.added.push_back(net::DeployableWire{ d.id, d.def, d.owner, d.team, d.position });
		}
	}
	std::sort(scope.begin(), scope.end());
	for (const uint16_t id : v.known) {
		if (!std::binary_search(scope.begin(), scope.end(), id) && m.removed.size() < net::kMaxDeployablesPerMessage) {
			m.removed.push_back(id);
		}
	}
	if (m.added.empty() && m.removed.empty()) {
		return std::nullopt;
	}
	std::vector<uint16_t> known = v.known;
	for (const uint16_t id : m.removed) {
		known.erase(std::lower_bound(known.begin(), known.end(), id));
	}
	for (const net::DeployableWire &a : m.added) {
		known.insert(std::lower_bound(known.begin(), known.end(), a.id), a.id);
	}
	v.known = std::move(known);
	if (m.added.size() + m.removed.size() >= net::kMaxDeployablesPerMessage) {
		v.revision = UINT64_MAX;
	}
	++stats_.messages;
	stats_.added += m.added.size();
	stats_.removed += m.removed.size();
	return net::encode_deployables(m);
}

}
