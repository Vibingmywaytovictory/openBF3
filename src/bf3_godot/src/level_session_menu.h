#pragma once

#include "overview_map.h"

#include "bf3/core/ui/ui_drawn_screens.h"
#include "bf3/core/ui/ui_graph.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace godot {
class Node;
}

namespace bf3::host {
class UiFlowSession;
}

namespace bf3::godot_side {

class Bf3UiScreens;
class InputMapper;
class NativeTags;

using bf3::core::ui::kIngameMenuPartition;
using bf3::core::ui::kMenuDrawnPartitions;
using bf3::core::ui::kMenuScoreboardBackPartition;
using bf3::core::ui::kMenuScoreboardPartition;
using bf3::core::ui::kPopupGenericPartition;
inline constexpr std::string_view kUiGameInputAction = "GameInput";
inline constexpr std::string_view kUiMouseInputAction = "MouseInput";
inline constexpr std::string_view kUiMouseCursorAction = "MouseCursor";
inline constexpr std::string_view kUiCameraChangedAction = "OnFrontendCameraChanged";
inline constexpr std::string_view kUiSuicideAction = "Suicide";
inline constexpr std::string_view kUiQuitGameAction = "QuitGame";
inline constexpr std::string_view kUiShutDownAction = "ShutDown";
inline constexpr std::string_view kUserCompCategory = "usercomp";
inline constexpr int32_t kIsInMenuKey = -1093247625;
inline constexpr size_t kNativeMenuMaxPending = 256;

struct NativeMenuIo {
	godot::Node *parent = nullptr;
	std::string level;
	InputMapper *mapper = nullptr;
	const NativeTags *tags = nullptr;
	std::optional<OverviewMark> local;
	std::vector<OverviewMark> downed;
	std::function<void()> suicide;
};

struct NativeMenuStats {
	uint64_t game_input_off = 0;
	uint64_t game_input_on = 0;
	uint64_t cursor_shown = 0;
	uint64_t cursor_hidden = 0;
	uint64_t glitches = 0;
	uint64_t dims_in = 0;
	uint64_t dims_out = 0;
	uint64_t suicides = 0;
	uint64_t quits = 0;
	uint64_t overview_attached = 0;
	uint64_t dropped = 0;
};

class NativeMenu {
public:
	void build(Bf3UiScreens &r_screens);
	bool take(const bf3::core::ui::UiFlowAction &p_action);
	void frame(Bf3UiScreens &r_screens, const bf3::host::UiFlowSession *p_session, const NativeMenuIo &p_io);

	bool in_menu() const { return in_menu_; }
	bool game_input() const { return game_input_; }
	bool wants_mouse() const { return !game_input_ || mouse_input_; }
	bool quit_requested() const { return quit_requested_; }
	const NativeMenuStats &stats() const { return stats_; }

private:
	void apply(const bf3::core::ui::UiFlowAction &p_action, const NativeMenuIo &p_io);
	void show_cursor(const NativeMenuIo &p_io, bool p_shown);
	void attach_overview(Bf3UiScreens &r_screens, const NativeMenuIo &p_io);

	std::vector<bf3::core::ui::UiFlowAction> pending_;
	NativeMenuStats stats_;
	bool game_input_ = true;
	bool mouse_input_ = false;
	bool cursor_wanted_ = false;
	bool cursor_shown_ = false;
	bool in_menu_ = false;
	bool quit_requested_ = false;
	uint64_t overview_view_ = 0;
};

}
