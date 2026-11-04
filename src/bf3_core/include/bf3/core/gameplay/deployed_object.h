#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/logic_types.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::gameplay {

inline constexpr int32_t kOnSpawnedEvent = logic_port_id("OnSpawned");
inline constexpr int32_t kOnDestroyedEvent = logic_port_id("OnDestroyed");
inline constexpr int32_t kEffectStartEvent = logic_port_id("Start");
inline constexpr int32_t kEffectStopEvent = logic_port_id("Stop");
inline constexpr size_t kMaxDeployedComponents = 64;

struct DeployedObject {
	std::string projectile;
	std::string entity_type;
	std::string mesh;
	std::vector<std::string> effects;
};

Result<DeployedObject> read_deployed_object(const EbxResolver &p_db, const EbxInstance &p_projectile);

Result<DeployedObject> read_fired_object(const EbxResolver &p_db, std::string_view p_firing_data);

}
