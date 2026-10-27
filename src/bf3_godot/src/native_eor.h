#pragma once

#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/ui_graph.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace godot {
class Node;
}

namespace bf3::godot_side {

class Bf3UiScreens;

inline constexpr std::string_view kEorGraphPrefix = "ui/flow/graph/endofround/";
inline constexpr std::string_view kEorHud64Partition = "ui/flow/screen/scoreboards/scoreboardtwoteamshud64screen";
inline constexpr std::string_view kEorTabsPartition = "ui/flow/screen/eordetailstabsscreen";
inline constexpr std::string_view kEorScoreboardPartition = "ui/flow/screen/eordetailsscoreboardscreen";
inline constexpr std::string_view kEorTwoTeamsPartition = "ui/flow/screen/scoreboards/scoreboardtwoteamsscreen";
inline constexpr std::string_view kEorStatsPartition = "ui/flow/screen/eordetailsstatsscreen";
inline constexpr std::string_view kEorPopupPartition = "ui/flow/screen/popups/popupgeneric";
inline constexpr std::array<std::string_view, 7> kNativeEorScreens{ bf3::core::ui::kEorWinningTeamScreenPartition,
	kEorHud64Partition, kEorTabsPartition, kEorScoreboardPartition, kEorTwoTeamsPartition, kEorStatsPartition,
	kEorPopupPartition };
inline constexpr std::array<std::string_view, 2> kNativeEorScoreboards{ kEorHud64Partition, kEorTwoTeamsPartition };
inline constexpr std::string_view kEorMouseInputAction = "MouseInput";
inline constexpr std::string_view kEorGameInputAction = "GameInput";
inline constexpr std::string_view kEorCameraChangedAction = "OnFrontendCameraChanged";
inline constexpr std::string_view kEorQuitGameAction = "QuitGame";
inline constexpr std::string_view kEorShutDownAction = "ShutDown";
inline constexpr size_t kNativeEorMaxPending = 64;

struct NativeEorStats {
	uint64_t actions = 0;
	uint64_t dropped = 0;
	uint64_t row_feeds = 0;
	uint64_t glitches = 0;
	uint64_t cursor_shown = 0;
	uint64_t quits = 0;
};

struct NativeEorIo {
	godot::Node *parent = nullptr;
	std::string level;
};

class NativeEor {
public:
	void build(Bf3UiScreens &r_screens);
	bool take(const bf3::core::ui::UiFlowAction &p_action);
	void frame(Bf3UiScreens &r_screens, const bf3::core::ui::HudModel &p_model, const NativeEorIo &p_io);

	bool wants_mouse() const { return mouse_input_ && !game_input_; }
	bool quit_requested() const { return quit_requested_; }
	const NativeEorStats &stats() const { return stats_; }

private:
	struct Fed {
		const bf3::core::ui::HudScreen *screen = nullptr;
		std::string signature;
	};
	void apply(const bf3::core::ui::UiFlowAction &p_action, const NativeEorIo &p_io);
	void feed_rows(Bf3UiScreens &r_screens, const bf3::core::ui::HudModel &p_model);
	void show_cursor(const NativeEorIo &p_io, bool p_shown);

	std::vector<bf3::core::ui::UiFlowAction> pending_;
	std::array<Fed, kNativeEorScoreboards.size()> fed_{};
	bool mouse_input_ = false;
	bool game_input_ = true;
	bool cursor_shown_ = false;
	bool quit_requested_ = false;
	bool round_over_ = false;
	NativeEorStats stats_;
};

}
