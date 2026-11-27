#include "vehicle_gunnery.h"

#include "bf3/sim/vehicles/vehicle_targets.h"

#include <algorithm>
#include <cmath>

namespace bf3::host {

namespace sw = sim::weapons;
namespace vec = core::vec;

core::Result<std::shared_ptr<const sw::WeaponTable>> make_vehicle_weapon_table(const LevelVehicles &p_level,
		std::shared_ptr<const sw::SurfaceMaterialTable> p_materials, const sw::Mechanics &p_mechanics) {
	sw::WeaponTableOptions options;
	options.mechanics = p_mechanics;
	core::Result<std::shared_ptr<const sw::WeaponTable>> table =
			sw::make_weapon_table(p_level.weapons, std::move(p_materials), options);
	if (!table) {
		return table.error();
	}
	auto armed = std::make_shared<sw::WeaponTable>(**table);
	for (size_t i = 0; i < armed->defs.size() && i < p_level.weapons.size(); ++i) {
		const core::gameplay::FireFunction &fire = p_level.weapons[i].fire;
		// INFERRED: a negative NumberOfMagazines with a magazine is an endless reserve that still reloads. Settle: a note
		// on FireFunction's magazine count.
		if (fire.number_of_magazines < 0 && fire.magazine_capacity > 0) {
			armed->defs[i].unlimited_ammo = false;
			armed->defs[i].infinite_reserve = true;
		}
	}
	armed->hash = sw::weapon_table_hash(*armed);
	return std::shared_ptr<const sw::WeaponTable>(std::move(armed));
}

std::shared_ptr<const sw::WeaponTable> with_vehicle_weapons(
		const sw::WeaponTable &p_soldier, const sw::WeaponTable &p_vehicle) {
	auto out = std::make_shared<sw::WeaponTable>(p_soldier);
	out->variants.resize(out->defs.size());
	out->defs.insert(out->defs.end(), p_vehicle.defs.begin(), p_vehicle.defs.end());
	out->variants.resize(out->defs.size());
	return out;
}

net::DetonationWire vehicle_detonation_wire(const sw::DetonationEvent &p_event) {
	net::DetonationWire w;
	w.shooter = uint8_t(std::min<uint32_t>(p_event.player_slot, 255u));
	w.shot_index = p_event.shot_index;
	w.def = p_event.def;
	w.time = p_event.time;
	w.position = p_event.position;
	w.normal = p_event.normal;
	w.dud = p_event.dud;
	w.vehicle = true;
	return w;
}

core::Status VehicleGunnery::configure(const LevelVehicles &p_level,
		std::shared_ptr<const sw::SurfaceMaterialTable> p_materials, const sw::Mechanics &p_mechanics) {
	table_.reset();
	projectiles_.reset();
	mounts_.clear();
	heat_rules_.clear();
	interval_shots_.clear();
	if (p_level.weapons.empty() || p_materials == nullptr) {
		return core::Status::success();
	}
	core::Result<std::shared_ptr<const sw::WeaponTable>> table =
			make_vehicle_weapon_table(p_level, std::move(p_materials), p_mechanics);
	if (!table) {
		return core::error_at("vehicle weapons", table.error());
	}
	table_ = std::move(*table);
	projectiles_ = std::make_unique<sw::ProjectileWorld>(table_);
	for (const core::gameplay::WeaponDef &d : p_level.weapons) {
		heat_rules_.push_back({ d.fire.heat_per_bullet, d.fire.heat_drop_per_second, d.fire.overheat_penalty_time,
				d.fire.overheat_threshold });
	}
	return core::Status::success();
}

void VehicleGunnery::reset(size_t p_vehicles) {
	mounts_.assign(p_vehicles, {});
	interval_shots_.clear();
	if (table_ != nullptr) {
		projectiles_ = std::make_unique<sw::ProjectileWorld>(table_);
	}
}

void VehicleGunnery::spawned(size_t p_vehicle) {
	if (p_vehicle < mounts_.size()) {
		mounts_[p_vehicle] = {};
	}
}

void VehicleGunnery::fire(sim::Tick p_tick, size_t p_vehicle, const LevelVehicleKind &p_kind,
		const sim::vehicles::ArmamentState &p_armament, const core::Transform &p_hull,
		std::span<const uint8_t> p_occupants, std::span<const sim::Command> p_commands, uint64_t p_seed) {
	if (!on() || p_vehicle >= mounts_.size() || p_kind.armament == nullptr) {
		return;
	}
	const sim::vehicles::VehicleArmament &a = *p_kind.armament;
	for (size_t w = 0; w < a.weapons.size() && w < sim::vehicles::kMaxArmamentWeapons && w < p_kind.weapon_defs.size();
			++w) {
		const sim::vehicles::ArmamentWeapon &weapon = a.weapons[w];
		Mount &mount = mounts_[p_vehicle][w];
		const uint16_t def = p_kind.weapon_defs[w];
		const size_t seat = size_t(weapon.seat);
		const uint8_t occupant = seat < p_occupants.size() ? p_occupants[seat] : uint8_t(0xFF);
		// INFERRED: the mount cools before its fire logic runs, and an overheated mount gets no fire. Settle: a note on
		// the vehicle weapon's overheat order.
		if (def < heat_rules_.size()) {
			sim::vehicles::overheat_tick(mount.heat, heat_rules_[def], sim::kTickSeconds);
		}
		// STAND-IN: only a seat's stance-0 weapons fire, all on its fire button. Settle: the seat's weapon switching data.
		if (def == kNoVehicleWeapon || occupant == 0xFF || seat >= p_commands.size() ||
				!sim::vehicles::weapon_in_stance(weapon, 0)) {
			mount.occupant = 0xFF;
			continue;
		}
		if (mount.occupant != occupant) {
			mount.state.player_slot = occupant;
			mount.state.next_shot_index = kVehicleShotIndexBase + uint32_t(w) * kVehicleShotIndexStride;
			const uint16_t loadout[] = { def };
			if (!sw::spawn_soldier_weapons(mount.state, *table_, loadout, 0, p_tick - 1u).ok()) {
				++stats_.mount_failures;
				continue;
			}
			mount.occupant = occupant;
		}
		const core::Transform frame = sim::vehicles::weapon_world(a, p_armament, p_hull, w);
		const core::Vec3 direction = vec::normalize(frame.forward);
		const core::Vec3 muzzle = frame.transform_point(weapon.spawn_offset);
		sim::Command c = p_commands[seat];
		c.tick = p_tick;
		c.buttons &= mount.heat.overheated ? sim::buttons::kReload : sim::buttons::kFire | sim::buttons::kReload;
		c.yaw = sim::quantize_yaw(std::atan2(direction[0], direction[2]));
		c.pitch = sim::quantize_pitch(std::asin(std::clamp(direction[1], -1.0f, 1.0f)));
		c.pose = sim::Pose::Stand;
		sw::WeaponContext context;
		context.eye_start = muzzle;
		context.eye_end = muzzle;
		context.yaw = c.yaw;
		context.pitch = c.pitch;
		sw::step_soldier_weapons(*table_, mount.state, c, context, p_seed, tick_out_);
		for (uint8_t i = 0; i < tick_out_.shot_count; ++i) {
			++stats_.shots;
			if (def < heat_rules_.size() && sim::vehicles::overheat_shot(mount.heat, heat_rules_[def])) {
				++stats_.overheats;
			}
			interval_shots_.push_back(tick_out_.shots[i]);
			if (!projectiles_->spawn(tick_out_.shots[i]).ok()) {
				++stats_.refused;
			}
		}
	}
}

void VehicleGunnery::step(sim::Tick p_tick, const sim::CollisionWorld &p_world, const sw::HitTargetProvider &p_targets,
		std::vector<sw::DamageEvent> &r_damage) {
	if (!on()) {
		return;
	}
	out_.clear();
	vehicle_damage_.clear();
	projectiles_->step(p_tick, p_world, p_targets, out_);
	for (sw::DamageEvent e : out_.damage) {
		if (e.target >= sim::vehicles::kVehicleTargetIdBase) {
			vehicle_damage_.push_back(e);
			continue;
		}
		// STAND-IN: a soldier hit by a vehicle weapon carries no weapon def, so its kill names none. Settle: vehicle
		// weapon defs in the soldiers' damage and kill events.
		e.def = sw::kNoWeapon;
		r_damage.push_back(e);
		++stats_.damage;
	}
}

VehicleBlastHits VehicleGunnery::explode(const sw::ExplosionSimDef &p_explosion, const sw::DetonationEvent &p_source,
		uint8_t p_credit, const sim::CollisionWorld &p_world, const sw::HitTargetProvider &p_targets,
		std::vector<sw::DamageEvent> &r_damage) {
	VehicleBlastHits hits;
	if (!on()) {
		return hits;
	}
	hits.ran = true;
	const size_t first = out_.damage.size();
	const size_t detonation = out_.detonations.size();
	core::gameplay::WorkBudget budget(sw::BallisticsLimits{}.budget_per_step);
	sw::explode(*table_, p_explosion, p_world, p_targets, p_source, budget, out_);
	if (out_.detonations.size() > detonation) {
		out_.detonations[detonation].player_slot = p_credit;
	}
	++stats_.blasts;
	for (size_t i = first; i < out_.damage.size(); ++i) {
		sw::DamageEvent e = out_.damage[i];
		if (e.target >= sim::vehicles::kVehicleTargetIdBase) {
			e.attacker_slot = p_credit;
			vehicle_damage_.push_back(e);
			++hits.vehicles;
		} else {
			e.attacker_slot = p_credit < net::kMaxSlots ? p_credit : e.target;
			r_damage.push_back(e);
			++stats_.damage;
			++hits.soldiers;
		}
	}
	return hits;
}

std::optional<net::OwnSeatWeapon> VehicleGunnery::seat_weapon(
		size_t p_vehicle, const LevelVehicleKind &p_kind, uint8_t p_seat, uint8_t p_occupant) const {
	if (!on() || p_vehicle >= mounts_.size() || p_kind.armament == nullptr) {
		return std::nullopt;
	}
	const sim::vehicles::VehicleArmament &a = *p_kind.armament;
	const size_t weapons = std::min({ a.weapons.size(), p_kind.weapon_defs.size(), sim::vehicles::kMaxArmamentWeapons,
			size_t(net::kMaxSeatWeaponWire) + 1u });
	for (size_t w = 0; w < weapons; ++w) {
		const sim::vehicles::ArmamentWeapon &weapon = a.weapons[w];
		if (weapon.seat != int8_t(p_seat) || p_kind.weapon_defs[w] == kNoVehicleWeapon ||
				!sim::vehicles::weapon_in_stance(weapon, 0)) {
			continue;
		}
		const Mount &mount = mounts_[p_vehicle][w];
		if (mount.occupant != p_occupant) {
			return std::nullopt;
		}
		const sw::WeaponSlotState &slot = mount.state.current();
		net::OwnSeatWeapon out;
		out.weapon = uint8_t(w);
		out.magazine = uint16_t(std::clamp<int32_t>(slot.magazine, 0, UINT16_MAX));
		out.heat = net::seat_weapon_heat_wire(mount.heat.heat);
		out.reloading = slot.phase == sw::WeaponPhase::Reloading || slot.phase == sw::WeaponPhase::ReloadShell;
		return out;
	}
	return std::nullopt;
}

std::span<const sw::Projectile> VehicleGunnery::live() const {
	return projectiles_ != nullptr ? projectiles_->live() : std::span<const sw::Projectile>();
}

}
