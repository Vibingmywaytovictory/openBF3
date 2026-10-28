#pragma once

#include "bf3/core/gameplay/eor_timeline.h"
#include "bf3/core/level/eor_camera.h"

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace bf3::core::ui {
struct HudModel;
}

namespace bf3::host {
class UiFlowSession;
}

namespace godot {
class Camera3D;
}

namespace bf3::godot_side {

class Bf3UiScreens;
class EorCameraView;
class EorFlash;
class NativeEor;

inline constexpr double kEorCheckSeconds = 420.0;
inline constexpr double kEorCheckToleranceS = 0.25;
inline constexpr double kEorCheckSettleS = 1.0;
inline constexpr double kEorCheckStepWaitS = 2.0;
inline constexpr double kEorCheckDeployEveryS = 1.0;
inline constexpr double kEorCheckBannerShotS = 1.5;
inline constexpr double kEorCheckHud64ShotS = 2.75;
inline constexpr double kEorCheckStatsShotS = 1.0;
inline constexpr double kEorCheckPopupShotS = 1.0;
inline constexpr size_t kEorCheckMaxLines = 4096;
inline constexpr float kEorCheckStatsTabX = 346.0f;
inline constexpr float kEorCheckStatsTabY = 112.0f;
inline constexpr float kEorCheckQuitX = 145.0f;
inline constexpr float kEorCheckQuitY = 647.0f;
inline constexpr double kEorCheckCameraM = 0.01;
inline constexpr double kEorCheckFovDeg = 0.01;
inline constexpr float kEorCheckFlashPeak = 0.9f;
inline constexpr const char *kEorQuitTitle = "QUIT TO WINDOWS";
inline constexpr const char *kEorQuitText = "Are you sure you want to quit? Any unsaved progress will be lost.";
inline constexpr const char *kEorQuitOk = "OK";
inline constexpr const char *kEorQuitCancel = "CANCEL";

struct EorCheckIo {
	std::function<bool(const std::string &)> shot;
	std::function<void(godot::Key, bool)> press;
	bf3::core::gameplay::EorTimeline eor;
	std::optional<bf3::core::level::EorCamera> camera_data;
	const EorCameraView *eor_camera = nullptr;
	const EorFlash *flash = nullptr;
	godot::Camera3D *camera = nullptr;
};

class EorCheck {
public:
	explicit EorCheck(std::string p_shot_dir) : shot_dir_(std::move(p_shot_dir)) {}

	bool frame(Bf3UiScreens &r_screens, bf3::host::UiFlowSession *p_session, const NativeEor &p_eor,
			const bf3::core::ui::HudModel &p_model, double p_now_s, const EorCheckIo &p_io);
	std::string report() const;
	int exit_code() const { return passed_ ? 0 : 1; }

private:
	enum class Step : uint8_t {
		Banner,
		Scoreboard,
		Page,
		PageSettled,
		StatsClicked,
		StatsShown,
		TabLeft,
		TabRight,
		Restart,
		Banner2,
		Page2,
		QuitClicked,
		PopupShown,
		PopupActivated,
		Done,
	};
	void note(std::string p_line);
	void deploy(const Bf3UiScreens &p_screens, double p_now_s, const EorCheckIo &p_io);
	void collect(Bf3UiScreens &r_screens, bf3::host::UiFlowSession *p_session);
	void fail(std::string p_why);
	void go(Step p_step, double p_now_s);
	bool timed(double p_took, double p_want, double p_tolerance, const char *p_what);
	void shot(const EorCheckIo &p_io, const char *p_name);
	void shot_later(const char *p_name, double p_at_s);
	bool check_page(Bf3UiScreens &r_screens, const bf3::core::ui::HudModel &p_model, double p_left_s);
	bool check_eor_view(Bf3UiScreens &r_screens, const EorCheckIo &p_io);
	bool check_player_view(const EorCheckIo &p_io);
	static double page_after_banner(const EorCheckIo &p_io);
	bool click_popup_ok(Bf3UiScreens &r_screens);
	static void click(Bf3UiScreens &r_screens, godot::Vector2 p_at);
	static void press_concept(Bf3UiScreens &r_screens, int32_t p_concept);

	std::string shot_dir_;
	Step step_ = Step::Banner;
	double start_s_ = -1.0;
	double step_s_ = 0.0;
	double banner_s_ = -1.0;
	double page_s_ = -1.0;
	double over_s_ = -1.0;
	double deploy_s_ = -1.0;
	double shot_at_s_ = -1.0;
	const char *shot_name_ = nullptr;
	bool flash_shot_ = false;
	bool banner_under_hud64_checked_ = false;
	bool player_view_checked_ = false;
	bool done_ = false;
	bool passed_ = false;
	std::string failure_;
	std::vector<std::string> lines_;
};

}
