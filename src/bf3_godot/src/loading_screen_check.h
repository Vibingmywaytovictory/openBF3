#pragma once

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
class NativeLoading;

inline constexpr double kLoadingCheckSeconds = 120.0;
inline constexpr double kLoadingCheckHintSeconds = 13.0;
inline constexpr double kLoadingCheckHintSlackSeconds = 0.25;
inline constexpr double kLoadingCheckReopenSeconds = 14.5;
inline constexpr double kLoadingCheckHoldSeconds = 1.0;
inline constexpr double kLoadingCheckCaptureSeconds = 3.0;
inline constexpr size_t kLoadingCheckMaxLines = 8192;
inline constexpr std::string_view kLoadingCheckHeader = "TEAM DEATHMATCH";
inline constexpr std::string_view kLoadingCheckLevelName = "OPERATION MÉTRO";
inline constexpr std::string_view kLoadingCheckWaiting = "LOADING";
inline constexpr double kLoadingCheckLevelNameSeconds = (68.0 - 1.0) / 30.0;

struct LoadingCheckMessage {
	std::string text;
	double at_s = 0.0;
};

struct LoadingCheckShot {
	double at_s = 0.0;
	const char *name = "";
	bool taken = false;
};

struct LoadingScreenCheckIo {
	std::function<void(godot::Key, bool)> press;
	std::function<void()> reopen;
	std::function<void()> close;
	std::function<bool(const std::string &)> shot;
	std::function<bool()> captured;
	double ready_s = 0.0;
	double done_s = 0.0;
};

class LoadingScreenCheck {
public:
	explicit LoadingScreenCheck(std::string p_shot_dir) : shot_dir_(std::move(p_shot_dir)) {}

	bool frame(const NativeLoading &p_loading, Bf3UiScreens &r_screens, NativeFlow &r_flow, double p_now_s,
			const LoadingScreenCheckIo &p_io);
	std::string report() const;
	int exit_code() const { return passed_ ? 0 : 1; }

private:
	enum class Phase : uint8_t {
		Load,
		Reopen,
		Hints,
		Deploy,
		Spawned,
		Done,
	};
	void collect(Bf3UiScreens &r_screens, NativeFlow &r_flow);
	bool seen(std::string_view p_line) const;
	void check_load(const NativeLoading &p_loading, const LoadingScreenCheckIo &p_io);
	void hints(const NativeLoading &p_loading, double p_now_s, const LoadingScreenCheckIo &p_io);
	void deploy(double p_now_s, const LoadingScreenCheckIo &p_io);
	void fail(std::string p_why);

	std::string shot_dir_;
	Phase phase_ = Phase::Load;
	double start_s_ = -1.0;
	double phase_s_ = -1.0;
	double open_s_ = -1.0;
	bool passed_ = false;
	std::string failure_;
	std::vector<std::string> lines_;
	std::string load_detail_;
	std::string hint_first_;
	std::string hint_next_;
	uint64_t served_before_ = 0;
	double hint_changed_s_ = -1.0;
	std::vector<LoadingCheckMessage> messages_;
	std::array<LoadingCheckShot, 4> shots_{ { { 1.2, "loading" }, { 3.4, "level_name" }, { 7.5, "level_name_late" },
			{ 13.6, "next_hint" } } };
	std::vector<std::string> shots_taken_;
	bool enter_sent_ = false;
	bool released_on_spawn_screen_ = false;
	double spawned_s_ = -1.0;
};

}
