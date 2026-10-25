#pragma once

#include "bf3/core/gameplay/death_timeline.h"

#include <godot_cpp/classes/global_constants.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::godot_side {

class Bf3UiScreens;
class NativeFlow;

inline constexpr double kNativeScreensCheckSeconds = 90.0;
inline constexpr double kNativeScreensCheckHoldSeconds = 1.0;
inline constexpr double kNativeScreensCheckDelayToleranceSeconds = 0.1;
inline constexpr size_t kNativeScreensCheckMaxLines = 8192;
inline constexpr std::array<std::string_view, 3> kNativeScreensCheckSoldierWidgets{ "Ammo_01.Visible",
	"Health_01.Visible", "Minimap_01.Visible" };

struct NativeScreensCheckIo {
	std::function<void(godot::Key, bool)> press;
	std::function<void()> suicide;
	std::function<void()> revive;
	bf3::core::gameplay::DeathTimeline death;
};

class NativeScreensCheck {
public:
	bool frame(Bf3UiScreens &r_screens, NativeFlow &r_flow, double p_now_s, const NativeScreensCheckIo &p_io);
	std::string report() const;
	int exit_code() const { return passed_ ? 0 : 1; }

private:
	enum class Act : uint8_t {
		None,
		Press,
		Suicide,
		Revive,
	};
	enum class Delay : uint8_t {
		None,
		KillCard,
		SpawnScreens,
	};
	struct Step {
		const char *line;
		Act act = Act::None;
		godot::Key press = godot::KEY_NONE;
		const char *act_name = nullptr;
		bool absent = false;
		bool widgets_back = false;
		size_t timed_from = 0;
		Delay delay = Delay::None;
		bool sent = false;
		double seen_s = -1.0;
	};
	struct Line {
		double t = 0.0;
		std::string text;
	};
	void take(std::string p_line, double p_now_s);
	void collect(Bf3UiScreens &r_screens, NativeFlow &r_flow, double p_now_s);
	void advance(const Bf3UiScreens &p_screens, double p_now_s, const NativeScreensCheckIo &p_io);
	void act(Step &r_step, const NativeScreensCheckIo &p_io, double p_now_s);
	struct SoldierWidgets {
		size_t sent = 0;
		size_t shown = 0;
		std::string detail;
	};
	static SoldierWidgets soldier_widgets(const Bf3UiScreens &p_screens);
	void watch_card(const Bf3UiScreens &p_screens);
	static double delay_seconds(Delay p_delay, const NativeScreensCheckIo &p_io);

	std::vector<Step> steps_{ { "screens entered spawnscreenpc" },
		{ .line = "flow action Spawn(", .act = Act::Press, .press = godot::KEY_ENTER, .act_name = "Enter" },
		{ "screens exited spawnscreenpc" }, { "screens entered hudmpscreen" },
		{ .line = "screens entered ingamemenump", .act = Act::Press, .press = godot::KEY_ESCAPE, .act_name = "Escape" },
		{ .line = "screens exited ingamemenump", .act = Act::Press, .press = godot::KEY_ESCAPE, .act_name = "Escape" },
		{ "screens entered hudmpscreen" }, { .line = "screens open ingamemenump", .absent = true },
		{ .line = "flow native Died", .act = Act::Suicide, .act_name = "suicide" },
		{ .line = "screens open killscreen", .timed_from = 1, .delay = Delay::KillCard },
		{ "screens entered killscreen" }, { .line = "flow native Playing", .act = Act::Revive, .act_name = "revive" },
		{ "screens exited killscreen" },
		{ .line = "screens entered spawnscreenpc", .absent = true, .widgets_back = true, .delay = Delay::SpawnScreens },
		{ .line = "flow native Died", .act = Act::Suicide, .act_name = "suicide" },
		{ .line = "screens open killscreen", .timed_from = 1, .delay = Delay::KillCard },
		{ "screens entered killscreen" },
		{ .line = "screens close killscreen", .timed_from = 3, .delay = Delay::SpawnScreens },
		{ "screens exited killscreen" }, { "screens entered spawnscreenpc" } };
	size_t next_ = 0;
	size_t cursor_ = 0;
	double start_s_ = -1.0;
	bool done_ = false;
	bool passed_ = false;
	std::vector<Line> lines_;
	std::string failure_;
	uint64_t actions_ = 0;
	double load_seconds_ = 0.0;
	bf3::core::gameplay::DeathTimeline death_;
	bool card_shown_ = false;
	uint64_t card_frames_ = 0;
	uint64_t card_frames_widgets_shown_ = 0;
	std::string card_widgets_detail_;
	std::string back_widgets_detail_;
	bool back_widgets_shown_ = false;
};

}
