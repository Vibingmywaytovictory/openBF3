#pragma once

#include "bf3/host/ai_world.h"
#include "bf3/host/bot_fill.h"

namespace bf3::host {

struct TdmShared;

float tdm_area_temperature(TdmShared &r_shared, const BotStrategicArea &p_area, sim::Tick p_tick);

}
