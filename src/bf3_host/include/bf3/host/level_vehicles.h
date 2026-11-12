#pragma once

#include "bf3/core/gameplay/vehicle_def.h"
#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/result.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/vehicle_model.h"
#include "bf3/sim/vehicle_world.h"
#include "bf3/sim/vehicles/vehicle_armament.h"
#include "bf3/sim/vehicles/vehicle_damage.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bf3::core::level {
struct LevelTransform;
}
namespace bf3::core::gameplay {
struct VehicleDef;
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
	sim::vehicles::VehicleDamageRule damage;
	sim::vehicles::HullBox box;
	core::gameplay::MaterialPairRef material;
	std::array<bool, net::kMaxVehicleSeats> exposed{};
	float min_speed_for_mine = 0;
	core::gameplay::Explosion explosion;
	std::string explosion_effect;
	std::vector<core::gameplay::VehicleInputModifierDef> disabled_input;
};

struct LevelVehicleSpawner {
	uint32_t object = 0;
	uint16_t kind = 0;
	sim::VehiclePlacement placement;
	uint8_t team = 0;
	bool enabled = true;
	float initial_spawn_delay = 0;
	float spawn_delay = 0;
	bool apply_damage_to_abandoned = false;
	float time_until_abandoned = 0;
	float time_until_abandoned_is_destroyed = 0;
	float keep_alive_radius = 0;
	float respawn_range = 0;
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

sim::Vec3 vehicle_seat_point(const LevelVehicleKind &p_kind, uint8_t p_seat, const std::array<float, 4> &p_rotation,
		const sim::Vec3 &p_origin);

sim::Vec3 vehicle_body_origin(const sim::VehicleModel &p_model, const sim::VehicleBodyState &p_body);

bool land_tracked(const sim::VehicleModel &p_model);

sim::vehicles::VehicleDamageRule vehicle_damage_rule(const core::gameplay::VehicleDef &p_def);

inline constexpr uint16_t kNoVehicleWeapon = 0xFFFF;

core::Result<std::shared_ptr<const LevelVehicles>> load_level_vehicles(
		const LevelDatabase &p_db, const sim::VehicleTuning &p_tuning);

}
