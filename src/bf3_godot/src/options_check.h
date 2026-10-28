#pragma once

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::core::ui {
enum class OptionsTab : uint8_t;
}

namespace bf3::godot_side {

class Bf3UiScreens;
class GameSettings;
class NativeFlow;
class NativeMenu;
class NativeOptions;

inline constexpr double kOptionsCheckSeconds = 150.0;
inline constexpr double kOptionsCheckHoldSeconds = 1.0;
inline constexpr int32_t kOptionsCheckSteps = 10;
inline constexpr size_t kOptionsCheckFlowLog = 80;

struct OptionsCheckIo {
	std::function<void(godot::Key, bool)> press;
	std::function<void(godot::Vector2)> move;
	std::function<void(godot::Vector2, bool)> click;
	std::function<bool(const std::string &)> shot;
	std::function<void(godot::Key, bool)> game_key;
	std::function<void(bool)> hold_zoom;
	GameSettings *settings = nullptr;
	std::string settings_path;
	bool alive = false;
	bool game_input = true;
	float look_sensitivity = 0.0f;
	uint32_t jump_presses = 0;
	uint16_t air_ticks = 0;
	float camera_fov = 0.0f;
	float zoom_fov = 0.0f;
	float fov_progress = 0.0f;
	bool can_zoom = false;
	std::optional<float> master_gain;
};

class OptionsCheck {
public:
	explicit OptionsCheck(std::string p_shot_dir) : shot_dir_(std::move(p_shot_dir)) {}
	bool frame(Bf3UiScreens &r_screens, NativeFlow &r_flow, const NativeMenu &p_menu, const NativeOptions &p_options,
			double p_now_s, const OptionsCheckIo &p_io);
	std::string report() const;
	bool passed() const { return passed_; }

private:
	struct Ctx {
		Bf3UiScreens &screens;
		NativeFlow &flow;
		const NativeMenu &menu;
		const NativeOptions &options;
		const OptionsCheckIo &io;
	};
	struct Step {
		const char *name;
		std::function<bool(OptionsCheck &, const Ctx &)> ready;
		std::function<void(OptionsCheck &, const Ctx &)> act;
		const char *shot = nullptr;
		bool repeat_before = false;
		double seen_s = -1.0;
		uint32_t repeats = 0;
	};
	static std::vector<Step> steps();
	static bool shown(const Ctx &p_c, std::string_view p_partition);
	static std::string shown_text(const Ctx &p_c);
	static std::string menu_row_state(const Ctx &p_c, size_t p_row);
	static void open_options(std::vector<Step> &r_steps, const char *p_name);
	static void tap(const Ctx &p_c, godot::Key p_key, int32_t p_times = 1);
	void click_clip(const Ctx &p_c, std::string_view p_partition, const std::string &p_path);
	void click_tab(const Ctx &p_c, std::string_view p_partition, size_t p_tab);
	std::optional<std::string> list_row(const Ctx &p_c, std::string_view p_partition, size_t p_row);
	static double value(const Ctx &p_c, std::string_view p_option);
	static double applied(const Ctx &p_c, std::string_view p_option);
	void click_row(const Ctx &p_c, bf3::core::ui::OptionsTab p_tab, std::string_view p_source);
	void check_file_value(const Ctx &p_c, std::string_view p_option, double p_want);
	void pick_key(const Ctx &p_c);
	bool file_is_saved(const Ctx &p_c);
	void check_file(const Ctx &p_c);
	void check_play(const Ctx &p_c);
	void game_tap(const Ctx &p_c, godot::Key p_key);
	static bool hip_ready(const Ctx &p_c, double p_fov);
	static bool ads_ready(const Ctx &p_c);
	void note_fov(const Ctx &p_c, const char *p_what);

	std::string shot_dir_;
	std::string waiting_;
	std::vector<std::string> shots_;
	std::vector<std::string> notes_;
	std::vector<std::string> flow_log_;
	size_t flow_seen_ = 0;
	std::vector<Step> steps_ = steps();
	size_t next_ = 0;
	bool acted_ = false;
	double start_s_ = -1.0;
	double last_s_ = -1.0;
	int32_t jump_row_ = -1;
	godot::Key new_key_ = godot::KEY_NONE;
	std::string new_idk_;
	std::string old_idk_;
	double volume_default_ = 0.0;
	double sensitivity_default_ = 0.0;
	double saved_volume_ = 0.0;
	double saved_sensitivity_ = 0.0;
	std::string saved_text_;
	uint32_t jumps_before_ = 0;
	uint16_t max_air_ticks_ = 0;
	double fov_default_ = 0.0;
	float ads_fov_ = 0.0f;
	bool done_ = false;
	bool passed_ = false;
	std::string failure_;
};

}
