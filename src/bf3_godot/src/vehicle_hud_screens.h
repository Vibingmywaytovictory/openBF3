#pragma once

#include "bf3/host/hud_feed.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/host/vehicle_hud_flow.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace bf3::host {
class UiFlowSession;
}

namespace bf3::godot_side {

class Bf3UiScreens;

inline constexpr std::array<std::string_view, 5> kVehicleHudDrawnPartitions{
	"ui/flow/screen/vehicle/defaulttankscreen",
	"ui/flow/screen/vehicle/defaulttankgunnerscreen",
	"ui/flow/screen/vehicle/tank3pcrosshair",
	"ui/flow/screen/vehicle/vehiclesimplehitindicator",
	"ui/flow/screen/weapon/crosshairdot",
};

struct VehicleHudStepInput {
	std::span<const host::PresentedVehicle> vehicles;
	const host::LevelVehicles *level = nullptr;
	uint8_t own_slot = 0xFF;
	bool alive = false;
	uint32_t camera = 0;
	bool alternate_view = false;
};

host::VehicleHudView vehicle_hud_view(const VehicleHudStepInput &p_in);

class VehicleHudScreens {
public:
	static void draw(Bf3UiScreens &r_screens);
	size_t step(host::UiFlowSession *p_session, Bf3UiScreens *p_screens, const VehicleHudStepInput &p_in);

private:
	host::VehicleHudSwitches switches_;
	std::string vehicle_name_;
	const host::UiFlowSession *session_ = nullptr;
	uint64_t reloads_ = 0;
};

}
