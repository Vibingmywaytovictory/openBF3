#pragma once

#include "bf3/core/gameplay/vehicle_def.h"
#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/result.h"
#include "bf3/sim/vehicle_model.h"
#include "bf3/sim/vehicle_world.h"
#include "bf3/sim/vehicles/vehicle_armament.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bf3::core::level {
struct LevelTransform;
}

namespace bf3::host {

struct LevelDatabase;

struct LevelVehicleKind {
	std::string blueprint;
	std::string mesh;
	std::shared_ptr<const sim::VehicleModel> model;
	uint8_t seats = 0;
	float entry_radius = 0;
	std::vector<core::gameplay::VehicleSeatDef> seat_defs;
	std::vector<core::Transform> exit_points;
	std::vector<std::pair<std::string, core::Transform>> component_meshes;
	std::shared_ptr<const sim::vehicles::VehicleArmament> armament;
	std::vector<uint16_t> weapon_defs;
};

struct LevelVehicleSpawner {
	uint32_t object = 0;
	uint16_t kind = 0;
	sim::VehiclePlacement placement;
	uint8_t team = 0;
	bool enabled = true;
	float initial_spawn_delay = 0;
	float spawn_delay = 0;
};

struct LevelVehicles {
	std::vector<LevelVehicleKind> kinds;
	std::vector<LevelVehicleSpawner> spawners;
	std::vector<std::string> notes;
	sim::VehicleTuning tuning;
	std::vector<core::gameplay::WeaponDef> weapons;
};

inline constexpr float kVehicleSpawnLift = 0.1f;

sim::VehiclePlacement vehicle_spawn_placement(const core::level::LevelTransform &p_transform, float p_lift);

bool land_jeep(const sim::VehicleModel &p_model);

std::optional<core::Vec3> vehicle_exit_point(const LevelVehicleKind &p_kind, uint8_t p_seat);

bool land_tracked(const sim::VehicleModel &p_model);

inline constexpr uint16_t kNoVehicleWeapon = 0xFFFF;

core::Result<std::shared_ptr<const LevelVehicles>> load_level_vehicles(
		const LevelDatabase &p_db, const sim::VehicleTuning &p_tuning);

}
