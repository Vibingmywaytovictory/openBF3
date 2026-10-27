#pragma once

#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/ui_graph.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace godot {
class Node;
}

namespace bf3::host {
class UiFlowSession;
}

namespace bf3::godot_side {

class Bf3NativeHud;
class Bf3UiScreens;

inline constexpr std::string_view kSpawnScreenPcPartition = "ui/flow/screen/spawnscreenpc";
inline constexpr std::string_view kSpawnButtonScreenPartition = "ui/flow/screen/spawnbuttonscreen";
inline constexpr std::string_view kSpawnTicketCounterPartition = "ui/flow/screen/spawnscreenticketcountertdmscreen";
inline constexpr std::string_view kSpawnScreenOverviewPath = "instance1.OverviewMap_01";
inline constexpr std::string_view kUiKitCompPartition = "ui/uicomponents/uikitcomp";
inline constexpr std::string_view kUiSpawnPointCompPartition = "ui/uicomponents/uispawnpointcomp";
inline constexpr std::string_view kUiSpawnLogicCompPartition = "ui/uicomponents/uispawnlogiccomp";
inline constexpr std::string_view kUiTeamCompPartition = "ui/uicomponents/uiteamcomp";
inline constexpr int32_t kUiKitSelectedKitKey = 1985842909;
inline constexpr int32_t kUiSpawnPointCanSpawnKey = -990983020;
inline constexpr int32_t kUiSpawnPointHasValidSpawnKey = -225037516;
inline constexpr int32_t kUiSpawnLogicTimeKey = -1005581981;
inline constexpr int32_t kUiTeamScoreboardEnabledKey = 1855620510;

struct DeployScreensStats {
	uint64_t spawn_actions = 0;
	uint64_t glitches = 0;
	uint64_t kits_from_flow = 0;
	uint64_t dim_ins = 0;
	uint64_t dim_outs = 0;
	uint64_t overview_attaches = 0;
	uint64_t spawn_screens_shown = 0;
	std::string last_error;
};

class DeployScreens {
public:
	static void draw(Bf3UiScreens &r_screens);

	std::optional<uint8_t> feed(bf3::host::UiFlowSession &r_session, const bf3::core::ui::HudModel &p_model,
			bool p_scoreboard_held, uint8_t p_spawn_kit);
	bool take_action(const bf3::core::ui::UiFlowAction &p_action);
	void frame(Bf3UiScreens &r_screens, Bf3NativeHud *p_hud, godot::Node *p_parent, const std::string &p_level,
			bool p_cursor);

	bool spawn_screen_shown() const { return shown_; }
	const DeployScreensStats &stats() const { return stats_; }

private:
	std::optional<std::string> kit_written_;
	std::optional<float> glitch_;
	bool shown_ = false;
	bool cursor_ = false;
	uint64_t overview_view_ = 0;
	DeployScreensStats stats_;
};

}
