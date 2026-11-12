#pragma once

#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/sim/vehicles/vehicle_damage.h"
#include "bf3/sim/vehicles/vehicle_targets.h"
#include "bf3/sim/weapons/ballistics.h"
#include "bf3/sim/weapons/surface_materials.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace bf3::host {

inline constexpr std::string_view kRepairToolClass = "wcPowerTool";

class VehicleDamageSource {
public:
	void configure(const sim::weapons::WeaponTable &p_table, std::span<const core::gameplay::WeaponDef> p_defs);
	bool on() const { return materials_ != nullptr; }
	float amount(const sim::weapons::DamageEvent &p_event, uint8_t p_vehicle_property) const;
	float repair(uint16_t p_def) const { return p_def < repair_.size() ? repair_[p_def] : 0.0f; }

private:
	std::shared_ptr<const sim::weapons::SurfaceMaterialTable> materials_;
	std::vector<uint8_t> bullet_;
	std::vector<std::optional<uint8_t>> blast_;
	std::vector<float> repair_;
};

struct VehicleStrike {
	sim::weapons::DamageEvent event;
	sim::Vec3 position{};
	sim::Vec3 direction{};
	bool located = false;
	bool directional = false;
	float amount = 0;
	bool soldier_weapon = false;
};

uint32_t vehicle_of_target(uint32_t p_target);

void locate_strike(VehicleStrike &r_strike, const sim::weapons::ProjectileStepOutput &p_out, const sim::Vec3 &p_centre);

bool strike_before(const VehicleStrike &p_a, const VehicleStrike &p_b);

}
