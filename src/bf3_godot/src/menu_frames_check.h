#pragma once

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::godot_side {

class Bf3UiScreens;
class NativeFlow;
class NativeMenu;

inline constexpr double kMenuFramesCheckSeconds = 270.0;
inline constexpr double kMenuFramesBaselineSeconds = 5.0;
inline constexpr double kMenuFramesHoldSeconds = 1.0;
inline constexpr double kMenuFramesKeySeconds = 0.4;
inline constexpr double kMenuFramesRepeatSeconds = 3.0;
inline constexpr double kMenuFramesRecoverKeySeconds = 1.0;
inline constexpr uint32_t kMenuFramesMenuCycles = 8;
inline constexpr uint32_t kMenuFramesOptionsRounds = 2;
inline constexpr double kMenuFramesHitchFactor = 3.0;
inline constexpr double kMenuFramesHitchFloorMs = 50.0;
inline constexpr double kMenuFramesMedianFactor = 1.25;
inline constexpr double kMenuFramesMedianSlackMs = 2.0;
inline constexpr size_t kMenuFramesMaxFrames = 30000;
inline constexpr size_t kMenuFramesReportFrames = 12;
inline constexpr uint32_t kMenuFramesShots = 18;
inline constexpr uint32_t kMenuFramesShotsPerStep = 2;

struct MenuFramesIo {
	std::function<void(godot::Key, bool)> press;
	std::function<void(godot::Vector2)> move;
	std::function<void(godot::Vector2, bool)> click;
	std::function<bool(const std::string &)> shot;
	bool alive = false;
	bool game_input = true;
	double process_ms = 0.0;
	double ui_ms = 0.0;
	double render_cpu_ms = 0.0;
	double render_gpu_ms = 0.0;
	double video_mb = 0.0;
	uint64_t glitch_triggers = 0;
	bool glitch_drawn = false;
};

struct MenuFrameSample {
	uint16_t step = 0;
	float wall_ms = 0.0f;
	float process_ms = 0.0f;
	float ui_ms = 0.0f;
	float render_cpu_ms = 0.0f;
	float render_gpu_ms = 0.0f;
	uint16_t opened = 0;
	uint16_t glitches = 0;
	bool glitch_drawn = false;
	bool shot = false;
	bool recovering = false;
};

class MenuFramesCheck {
public:
	explicit MenuFramesCheck(std::string p_shot_dir);
	bool frame(Bf3UiScreens &r_screens, NativeFlow &r_flow, const NativeMenu &p_menu, double p_now_s,
			const MenuFramesIo &p_io);
	std::string report() const;
	int exit_code() const { return passed_ ? 0 : 1; }

private:
	enum class Phase : uint8_t { Setup, First, Deploy, Customize, Play, Menu, Options };
	struct Ctx {
		Bf3UiScreens &screens;
		NativeFlow &flow;
		const NativeMenu &menu;
		const MenuFramesIo &io;
	};
	using Act = std::function<void(MenuFramesCheck &, const Ctx &)>;
	struct Step {
		std::string name;
		Phase phase = Phase::Setup;
		double hold_s = kMenuFramesHoldSeconds;
		std::function<bool(const Ctx &)> ready;
		Act act;
		bool repeat_before = false;
		double seen_s = -1.0;
		bool resume = false;
		bool resume_escape = false;
	};
	static std::vector<Step> steps();
	static void options_round(std::vector<Step> &r_steps, Phase p_phase, bool p_leave);
	static const char *phase_name(Phase p_phase);
	static bool gated(Phase p_phase);
	static bool shown(const Ctx &p_c, std::string_view p_partition);
	static void tap(const Ctx &p_c, godot::Key p_key);
	static bool counted(const MenuFrameSample &p_f);
	size_t resume_step() const;
	void recover(const Ctx &p_c, double p_now_s);
	void click_clip(const Ctx &p_c, std::string_view p_partition, const std::string &p_path);
	void record(const Ctx &p_c);
	void judge();
	std::vector<double> walls(bool p_menus) const;

	std::vector<Step> steps_ = steps();
	std::vector<MenuFrameSample> frames_;
	size_t next_ = 0;
	bool acted_ = false;
	double start_s_ = -1.0;
	double last_act_s_ = -1.0;
	std::optional<std::chrono::steady_clock::time_point> last_frame_;
	uint64_t opened_seen_ = 0;
	uint64_t glitches_seen_ = 0;
	uint64_t glitch_frames_ = 0;
	uint32_t repeats_ = 0;
	bool recovering_ = false;
	size_t resume_ = 0;
	uint32_t deaths_ = 0;
	uint64_t recovery_frames_ = 0;
	double video_play_mb_ = 0.0;
	double video_peak_mb_ = 0.0;
	std::string shot_dir_;
	std::vector<std::string> shots_;
	std::vector<uint32_t> step_shots_ = std::vector<uint32_t>(steps_.size(), 0u);
	bool shot_last_ = false;
	bool done_ = false;
	bool passed_ = false;
	std::string failure_;
	std::string verdict_;
};

}
