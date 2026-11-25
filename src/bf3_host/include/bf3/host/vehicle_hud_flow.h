#pragma once

#include "bf3/core/gameplay/vehicle_def.h"
#include "bf3/core/gameplay/vehicle_hud.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace bf3::host {

class UiFlowSession;

std::vector<const core::gameplay::VehicleCameraDef *> vehicle_seat_cameras(
		const core::gameplay::VehicleSeatDef &p_seat);

const core::gameplay::VehicleCameraDef *vehicle_seat_camera(
		const core::gameplay::VehicleSeatDef &p_seat, uint32_t p_counter);

struct VehicleHudView {
	const core::gameplay::VehicleHudDef *hud = nullptr;
	uint64_t vehicle = 0;
	int camera = -1;
	bool alternate_view = false;
};

class VehicleHudSwitches {
public:
	std::vector<core::gameplay::VehicleHudGraphEvent> step(const VehicleHudView &p_view);
	const std::vector<bool> &values() const { return values_; }

private:
	void settle(const VehicleHudView &p_view, std::vector<core::gameplay::VehicleHudGraphEvent> &r_out);

	const core::gameplay::VehicleHudDef *hud_ = nullptr;
	std::optional<uint64_t> vehicle_;
	std::vector<bool> values_;
};

size_t apply_vehicle_hud_events(
		UiFlowSession &r_session, const std::vector<core::gameplay::VehicleHudGraphEvent> &p_events);

}
