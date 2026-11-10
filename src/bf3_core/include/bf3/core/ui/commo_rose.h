#pragma once

#include "bf3/core/result.h"
#include "bf3/core/ui/ui_graph.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ui {

inline constexpr std::string_view kCommoRoseScreenPartition = "ui/flow/screen/commrosescreen";
inline constexpr std::string_view kCommoRoseWidget = "ComRose_01";
inline constexpr int32_t kCommoRoseCommandKey = -505845692;
inline constexpr int32_t kCommoRoseButtonKey = -627045675;
inline constexpr int32_t kCommoRoseNoCommand = -1;

struct CommoRoseEntry {
	std::string instance;
	int32_t button = -1;
	int32_t command_on_foot = kCommoRoseNoCommand;
	int32_t command_in_vehicle = kCommoRoseNoCommand;
	bool spot_and_order = false;

	int32_t command(bool p_in_vehicle) const { return p_in_vehicle ? command_in_vehicle : command_on_foot; }
};

struct CommoRoseLayout {
	std::vector<CommoRoseEntry> entries;

	const CommoRoseEntry *find(std::string_view p_instance) const;
};

Result<CommoRoseLayout> read_commo_rose(const UiGraphAsset &p_screen);

}
