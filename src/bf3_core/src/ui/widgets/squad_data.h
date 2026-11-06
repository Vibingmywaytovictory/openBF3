#pragma once

#include "hud_widget_support.h"

#include <optional>

namespace bf3::core::ui {

std::optional<As2Value> hud_squad_source_value(
		const HudDataSource &p_source, const HudModel &p_model, const HudClientState &p_client);

}
