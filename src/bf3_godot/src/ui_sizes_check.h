#pragma once

#include "bf3/core/ui/ui_stage.h"

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::godot_side {

class Bf3UiScreens;
class NativeFlow;
class NativeMenu;
class NativeOptions;

inline constexpr double kUiSizesCheckSeconds = 100.0;
inline constexpr double kUiSizesHoldSeconds = 0.75;
inline constexpr double kUiSizesRetapSeconds = 1.5;
inline constexpr double kUiSizesFitTolerance = 1e-3;
inline constexpr float kUiSizesCursorTolerance = 1e-3f;
inline constexpr size_t kUiSizesScoreboardRow = 1;
inline constexpr size_t kUiSizesResumeRow = 0;
inline constexpr size_t kUiSizesOptionsRow = 4;
inline constexpr size_t kUiSizesVideoTab = 3;

struct UiSizesWindow {
	const char *tag;
	int32_t width;
	int32_t height;
	bool fullscreen;
};

inline constexpr std::array<UiSizesWindow, 6> kUiSizesWindows{ {
		{ "1280x720", 1280, 720, false },
		{ "1920x1080", 1920, 1080, false },
		{ "fullscreen", 0, 0, true },
		{ "1024x768", 1024, 768, false },
		{ "2560x1080", 2560, 1080, false },
		{ "restored_1280x720", 1280, 720, false },
} };

struct UiSizesCheckIo {
	std::function<void(godot::Key, bool)> press;
	std::function<void(godot::Vector2, int32_t, bool)> mouse;
	std::function<void(godot::Vector2i, bool)> set_window;
	std::function<bool(const std::string &)> shot;
	godot::Vector2i window_size;
	godot::Vector2i screen_size;
	bool fullscreen = false;
	godot::Vector2 canvas_size;
	float cursor_scale = 0.0f;
	bool cursor_shown = false;
	bool alive = false;
	bool game_input = true;
	bool hud_visible = false;
};

class UiSizesCheck {
public:
	explicit UiSizesCheck(std::string p_shot_dir) : shot_dir_(std::move(p_shot_dir)) {}
	bool frame(Bf3UiScreens &r_screens, NativeFlow &r_flow, const NativeMenu &p_menu, const NativeOptions *p_options,
			double p_now_s, const UiSizesCheckIo &p_io);
	std::string report() const;
	int exit_code() const { return passed_ ? 0 : 1; }

private:
	struct Ctx {
		Bf3UiScreens &screens;
		NativeFlow &flow;
		const NativeMenu &menu;
		const NativeOptions *options;
		const UiSizesCheckIo &io;
	};
	struct Step {
		std::string name;
		std::function<bool(UiSizesCheck &, const Ctx &)> ready;
		std::function<void(UiSizesCheck &, const Ctx &)> act;
		std::string shot;
		std::function<void(UiSizesCheck &, const Ctx &)> waiting;
		double seen_s = -1.0;
	};
	std::vector<Step> steps() const;
	static bool shown(const Ctx &p_c, std::string_view p_partition);
	static bool options_tab_shown(const Ctx &p_c, size_t p_tab);
	static godot::Vector2i wanted_size(const Ctx &p_c, const UiSizesWindow &p_window);
	static bf3::core::ui::UiStage rule(const Ctx &p_c);
	bool window_settled(const Ctx &p_c, const UiSizesWindow &p_window);
	void check_fit(const Ctx &p_c, std::string_view p_partition, const UiSizesWindow &p_window);
	void mouse_at(
			const Ctx &p_c, std::string_view p_what, std::optional<bf3::core::ui::SwfPoint> p_stage, bool p_click);

	std::string shot_dir_;
	std::vector<Step> steps_ = steps();
	std::vector<std::string> facts_;
	std::vector<std::string> shots_;
	std::string waiting_;
	size_t next_ = 0;
	bool acted_ = false;
	double start_s_ = -1.0;
	double last_tap_s_ = -1.0;
	double now_s_ = 0.0;
	godot::Vector2i settling_;
	int32_t settled_frames_ = 0;
	bool done_ = false;
	bool passed_ = false;
	std::string failure_;
};

}
