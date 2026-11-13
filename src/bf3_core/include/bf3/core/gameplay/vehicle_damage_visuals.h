#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/math.h"
#include "bf3/core/result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::gameplay {

enum class HealthCompare : uint8_t { Equal, NotEqual, Less, LessOrEqual, Greater, GreaterOrEqual };

bool health_compare(HealthCompare p_op, float p_health, float p_threshold);

struct VehicleDamageEffectDef {
	std::string effect;
	core::Transform transform;
	HealthCompare compare = HealthCompare::LessOrEqual;
	float threshold = 0;
};

struct VehicleWreckGateDef {
	float probability = 0;
	int parent = -1;
	bool on_success = true;
};

struct VehicleWreckEffectDef {
	std::string effect;
	core::Transform transform;
	int gate = -1;
	bool on_success = true;
};

struct VehicleWreckDef {
	std::string blueprint;
	std::string mesh;
	core::Transform transform;
	float time_to_live = 0;
	std::vector<VehicleWreckGateDef> gates;
	std::vector<VehicleWreckEffectDef> effects;
};

struct VehicleDamageVisualsDef {
	std::vector<VehicleDamageEffectDef> damage;
	std::optional<VehicleWreckDef> wreck;
	size_t unresolved = 0;
};

Result<VehicleDamageVisualsDef> read_vehicle_damage_visuals(
		const EbxResolver &p_db, const EbxInstance &p_blueprint, const EbxInstance &p_entity, WorkBudget &r_budget);

std::vector<bool> roll_wreck_effects(const VehicleWreckDef &p_wreck, uint64_t p_seed);

}
