#include "bf3/host/level_vehicles.h"

#include "bf3/core/gameplay/vehicle_def.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/level_transform.h"
#include "bf3/core/level/vehicle_spawns.h"
#include "bf3/host/level_load.h"
#include "bf3/net/protocol.h"

#include <algorithm>
#include <cmath>

namespace bf3::host {

sim::VehiclePlacement vehicle_spawn_placement(const core::level::LevelTransform &p_transform, float p_lift) {
	sim::VehiclePlacement p;
	p.position = { p_transform.trans[0], p_transform.trans[1] + p_lift, p_transform.trans[2] };
	const double heading = std::atan2(double(p_transform.forward[0]), double(p_transform.forward[2]));
	p.rotation = { 0.0f, float(std::sin(heading / 2.0)), 0.0f, float(std::cos(heading / 2.0)) };
	return p;
}

bool land_jeep(const sim::VehicleModel &p_model) {
	return p_model.wheel_count == 4;
}

core::Result<std::shared_ptr<const LevelVehicles>> load_level_vehicles(
		const LevelDatabase &p_db, const sim::VehicleTuning &p_tuning) {
	if (p_db.level == nullptr || p_db.world == nullptr) {
		return core::make_error("level vehicles: no level database");
	}
	core::Result<std::vector<core::level::VehicleSpawnerDef>> spawners =
			core::level::read_vehicle_spawners(*p_db.world);
	if (!spawners) {
		return core::error_at("level vehicles", spawners.error());
	}
	auto out = std::make_shared<LevelVehicles>();
	out->tuning = p_tuning;
	std::vector<std::string> refused;
	for (const core::level::VehicleSpawnerDef &s : *spawners) {
		if (s.blueprint.empty() || std::find(refused.begin(), refused.end(), s.blueprint) != refused.end()) {
			continue;
		}
		auto kind = std::find_if(out->kinds.begin(), out->kinds.end(),
				[&](const LevelVehicleKind &k) { return k.blueprint == s.blueprint; });
		if (kind == out->kinds.end()) {
			core::Result<std::shared_ptr<const sim::VehicleModel>> model =
					sim::load_vehicle_model(p_db.level->db(), p_db.level->mount(), s.blueprint, p_tuning);
			if (!model) {
				out->notes.push_back("vehicle " + s.blueprint + " not drivable: " + model.error().message);
				refused.push_back(s.blueprint);
				continue;
			}
			if (!land_jeep(**model)) {
				out->notes.push_back("vehicle " + s.blueprint + " has " + std::to_string((*model)->wheel_count) +
						" wheels, not a jeep");
				refused.push_back(s.blueprint);
				continue;
			}
			core::Result<core::gameplay::VehicleDef> def =
					core::gameplay::read_vehicle_def(p_db.level->db(), s.blueprint);
			if (!def) {
				out->notes.push_back("vehicle " + s.blueprint + " has no seats: " + def.error().message);
				refused.push_back(s.blueprint);
				continue;
			}
			LevelVehicleKind k;
			k.blueprint = s.blueprint;
			k.model = *model;
			k.seats = uint8_t(std::min(def->seats.size(), net::kMaxVehicleSeats));
			for (const core::gameplay::VehicleSeatDef &seat : def->seats) {
				k.entry_radius = std::max(k.entry_radius, std::isfinite(seat.entry_radius) ? seat.entry_radius : 0.0f);
			}
			if (k.seats == 0) {
				out->notes.push_back("vehicle " + s.blueprint + " has no seats");
				refused.push_back(s.blueprint);
				continue;
			}
			out->kinds.push_back(std::move(k));
			kind = out->kinds.end() - 1;
		}
		LevelVehicleSpawner sp;
		sp.object = s.object;
		sp.kind = uint16_t(kind - out->kinds.begin());
		sp.placement = vehicle_spawn_placement(s.transform, kVehicleSpawnLift);
		sp.team = uint8_t(std::clamp(s.team, 0, int(net::kMaxTeam)));
		sp.enabled = s.enabled;
		sp.initial_spawn_delay = std::max(s.initial_spawn_delay, 0.0f);
		sp.spawn_delay = std::max(s.spawn_delay, 0.0f);
		out->spawners.push_back(sp);
	}
	return std::shared_ptr<const LevelVehicles>(std::move(out));
}

}
