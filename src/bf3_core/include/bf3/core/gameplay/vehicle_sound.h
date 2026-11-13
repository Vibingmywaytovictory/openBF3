#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/logic_types.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/math.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::gameplay {

namespace vehicle_sound_port {
inline constexpr int32_t kStart = logic_port_id("Start");
inline constexpr int32_t kStop = logic_port_id("Stop");
inline constexpr int32_t kThrottle = logic_port_id("Throttle");
inline constexpr int32_t kRpm = logic_port_id("RPM");
inline constexpr int32_t kGear = logic_port_id("Gear");
inline constexpr int32_t kLoad = logic_port_id("Load");
inline constexpr int32_t kVelocity = logic_port_id("Velocity");
inline constexpr int32_t kAngularVelocity = logic_port_id("AngularVelocity");
inline constexpr int32_t kRelativeVelocity = logic_port_id("RelativeVelocity");
inline constexpr int32_t kSlip = logic_port_id("Slip");
inline constexpr int32_t kTilt = logic_port_id("Tilt");
inline constexpr int32_t kContact = logic_port_id("Contact");
inline constexpr int32_t kBoost = logic_port_id("Boost");
inline constexpr int32_t kSpringCompression = logic_port_id("SpringCompression");
inline constexpr int32_t kInteriorCam = logic_port_id("InteriorCam");
inline constexpr int32_t kLocalPlayerInVehicle = logic_port_id("LocalPlayerInVehicle");
inline constexpr int32_t kHealth = logic_port_id("Health");
inline constexpr int32_t kDisabledDamageThreshold = logic_port_id("DisabledDamageThreshold");
inline constexpr int32_t kInDriverPos = logic_port_id("InDriverPos");
}

struct VehicleEngineSound {
	std::string patch;
	std::string surface_patch;
	Vec3 position{};
	bool first_person_sounds = false;
};

struct VehicleTurretSound {
	std::string patch;
	Vec3 position{};
	int8_t turret = -1;
};

struct VehicleSoundDef {
	std::string blueprint;
	std::vector<VehicleEngineSound> engines;
	std::vector<VehicleTurretSound> turrets;
	std::string bullet_impact;
	std::string switch_to_alternate_view;
	std::string switch_to_normal_view;
	std::string missile_warning;
	std::string locked_warning;
	std::string locking_warning;
};

Result<VehicleSoundDef> read_vehicle_sounds(
		const EbxResolver &p_db, std::string_view p_blueprint, WorkBudget *p_budget = nullptr);

}
