#include "vehicle_gunnery.h"

#include "bf3/sim/vehicles/vehicle_targets.h"

#include <algorithm>
#include <cmath>

namespace bf3::host {

namespace sw = sim::weapons;
namespace vec = core::vec;

core::Status VehicleGunnery::configure(const LevelVehicles &p_level,
		std::shared_ptr<const sw::SurfaceMaterialTable> p_materials, const sw::Mechanics &p_mechanics) {
	table_.reset();
	projectiles_.reset();
	mounts_.clear();
	if (p_level.weapons.empty() || p_materials == nullptr) {
		return core::Status::success();
	}
	sw::WeaponTableOptions options;
	options.mechanics = p_mechanics;
	core::Result<std::shared_ptr<const sw::WeaponTable>> table =
			sw::make_weapon_table(p_level.weapons, std::move(p_materials), options);
	if (!table) {
		return core::error_at("vehicle weapons", table.error());
	}
	auto armed = std::make_shared<sw::WeaponTable>(**table);
	for (size_t i = 0; i < armed->defs.size() && i < p_level.weapons.size(); ++i) {
		const core::gameplay::FireFunction &fire = p_level.weapons[i].fire;
		if (fire.number_of_magazines < 0 && fire.magazine_capacity > 0) {
			armed->defs[i].unlimited_ammo = false;
			armed->defs[i].infinite_reserve = true;
		}
	}
	armed->hash = sw::weapon_table_hash(*armed);
	table_ = std::move(armed);
	projectiles_ = std::make_unique<sw::ProjectileWorld>(table_);
	return core::Status::success();
}

void VehicleGunnery::reset(size_t p_vehicles) {
	mounts_.assign(p_vehicles, {});
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
		c.buttons &= sim::buttons::kFire | sim::buttons::kReload;
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
		e.def = sw::kNoWeapon;
		r_damage.push_back(e);
		++stats_.damage;
	}
}

std::span<const sw::Projectile> VehicleGunnery::live() const {
	return projectiles_ != nullptr ? projectiles_->live() : std::span<const sw::Projectile>();
}

}
