#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bf3::core::gameplay {

inline constexpr int32_t kCameraActiveOutput = int32_t(0xfff52c2cu);
inline constexpr int32_t kCameraAlternateViewOutput = int32_t(0xc9e1d573u);
inline constexpr size_t kMaxVehicleHudDepth = 16;

struct VehicleHudTerm {
	uint32_t camera = 0;
	bool alternate_view = false;

	bool operator==(const VehicleHudTerm &) const = default;
};

struct VehicleHudGraphEvent {
	std::string graph;
	std::string priority;
	bool pop_previous = false;
	int32_t event = 0;

	bool operator==(const VehicleHudGraphEvent &) const = default;
};

struct VehicleHudSwitch {
	std::string prefab;
	int32_t prefab_field = 0;
	bool initial = false;
	bool initial_event = false;
	std::vector<VehicleHudTerm> terms;
	std::vector<VehicleHudGraphEvent> on_true;
	std::vector<VehicleHudGraphEvent> on_false;
};

struct VehicleHudCameraFlags {
	bool use_weapon_orientations = false;
	bool use_render_target = false;
	bool use_aim_warning = false;
	bool range_meter = false;
};

struct VehicleHudDef {
	std::vector<VehicleHudSwitch> switches;
};

Result<VehicleHudDef> read_vehicle_hud(const EbxResolver &p_db, const EbxInstance &p_blueprint,
		std::span<const EbxInstance> p_cameras, WorkBudget &r_budget);

VehicleHudCameraFlags read_vehicle_hud_flags(const EbxResolver &p_db, const EbxStruct &p_hud_data);

}
