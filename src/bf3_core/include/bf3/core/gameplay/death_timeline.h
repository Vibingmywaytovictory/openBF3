#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/result.h"

#include <string_view>

namespace bf3::core::gameplay {

inline constexpr std::string_view kDeathLogicScope = "ui/flow/logic/uideathlogic";
inline constexpr std::string_view kKilledGraphAsset = "ui/flow/graph/ingame/killedgraph";
inline constexpr std::string_view kKilledGraphDeathInput = "Death";
inline constexpr std::string_view kKilledGraphDefragInput = "DefragMemory";

struct DeathTimeline {
	float kill_card_s = 0.0f;
	float spawn_screens_s = 0.0f;

	float kill_card_lifetime_s() const { return spawn_screens_s - kill_card_s; }
};

Result<DeathTimeline> read_death_timeline(const EbxResolver &p_db, const LogicGraph &p_client_graph);

}
