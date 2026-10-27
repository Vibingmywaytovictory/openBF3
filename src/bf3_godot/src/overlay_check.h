#pragma once

#include <godot_cpp/classes/global_constants.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bf3::host {
struct PresentationFrame;
}

namespace bf3::godot_side {

class Bf3UiScreens;

inline constexpr double kOverlayCheckSeconds = 150.0;
inline constexpr double kOverlayCheckSettleSeconds = 1.0;
inline constexpr double kOverlayCheckReactSeconds = 1.0;
inline constexpr double kOverlayCheckRowsSeconds = 0.6;

struct OverlayCheckIo {
	std::function<void(godot::Key, bool)> press;
	std::function<bool(std::string *)> join;
	std::function<bool(const std::string &)> shot;
};

class OverlayCheck {
public:
	explicit OverlayCheck(std::string p_shot_dir) : shot_dir_(std::move(p_shot_dir)) {}

	bool frame(Bf3UiScreens &r_screens, const bf3::host::PresentationFrame &p_f, double p_now_s,
			const OverlayCheckIo &p_io);
	std::string report() const;
	int exit_code() const { return passed_ ? 0 : 1; }

private:
	enum class Stage : uint8_t {
		Waiting,
		Countdown,
		RoundStart,
		TabDown,
		TabUp,
		Done,
	};
	struct Box {
		bool shown = false;
		std::string title;
		std::string description;
		std::string timer;
	};
	static Box box(Bf3UiScreens &r_screens);
	static bool scoreboard(const Bf3UiScreens &p_screens);
	void step(Bf3UiScreens &r_screens, const bf3::host::PresentationFrame &p_f, double p_t, const OverlayCheckIo &p_io);
	void shot(const char *p_name, double p_t, const OverlayCheckIo &p_io);
	void pass(std::string p_line, double p_t);
	void fail(std::string p_why);

	std::string shot_dir_;
	Stage stage_ = Stage::Waiting;
	double start_s_ = -1.0;
	double mark_s_ = -1.0;
	bool shot_taken_ = false;
	bool passed_ = false;
	bool failed_ = false;
	std::string failure_;
	std::vector<std::string> counts_;
	std::vector<std::string> lines_;
	std::vector<std::string> shots_;
};

}
