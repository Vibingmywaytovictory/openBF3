#pragma once

#include "bf3/core/math.h"
#include "bf3/core/result.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/command.h"
#include "bf3/sim/vehicles/vehicle_armament.h"
#include "bf3/sim/weapons/ballistics.h"
#include "bf3/sim/weapons/hit_target.h"
#include "bf3/sim/weapons/surface_materials.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace bf3::host {

inline constexpr uint32_t kVehicleShotIndexBase = 0x80000000u;
inline constexpr uint32_t kVehicleShotIndexStride = 1u << 24;

struct VehicleGunneryStats {
	uint64_t shots = 0;
	uint64_t refused = 0;
	uint64_t damage = 0;
	uint64_t mount_failures = 0;
};

class VehicleGunnery {
public:
	core::Status configure(const LevelVehicles &p_level,
			std::shared_ptr<const sim::weapons::SurfaceMaterialTable> p_materials,
			const sim::weapons::Mechanics &p_mechanics);
	bool on() const { return table_ != nullptr; }
	void reset(size_t p_vehicles);
	void spawned(size_t p_vehicle);

	void fire(sim::Tick p_tick, size_t p_vehicle, const LevelVehicleKind &p_kind,
			const sim::vehicles::ArmamentState &p_armament, const core::Transform &p_hull,
			std::span<const uint8_t> p_occupants, std::span<const sim::Command> p_commands, uint64_t p_seed);

	void step(sim::Tick p_tick, const sim::CollisionWorld &p_world, const sim::weapons::HitTargetProvider &p_targets,
			std::vector<sim::weapons::DamageEvent> &r_damage);

	const sim::weapons::WeaponTable *table() const { return table_.get(); }
	std::span<const sim::weapons::Projectile> live() const;
	const sim::weapons::ProjectileStepOutput &step_output() const { return out_; }
	std::span<const sim::weapons::DamageEvent> vehicle_damage() const { return vehicle_damage_; }
	const VehicleGunneryStats &stats() const { return stats_; }

private:
	struct Mount {
		sim::weapons::SoldierWeaponsState state;
		uint8_t occupant = 0xFF;
	};

	std::shared_ptr<const sim::weapons::WeaponTable> table_;
	std::unique_ptr<sim::weapons::ProjectileWorld> projectiles_;
	std::vector<std::array<Mount, sim::vehicles::kMaxArmamentWeapons>> mounts_;
	sim::weapons::ProjectileStepOutput out_;
	std::vector<sim::weapons::DamageEvent> vehicle_damage_;
	sim::weapons::WeaponTickOutput tick_out_;
	VehicleGunneryStats stats_;
};

}
