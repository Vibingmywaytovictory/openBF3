#pragma once

#include "bf3/core/ui/swf_movie.h"

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/vector2.hpp>

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
struct UiScreenSlot;

inline constexpr double kMenuCheckSeconds = 150.0;
inline constexpr double kMenuCheckHoldSeconds = 1.0;
inline constexpr std::string_view kMenuCheckList = "instance1.List_01";
inline constexpr float kMenuCheckRowHitX = 120.0f;
inline constexpr float kMenuCheckRowHitY = 17.0f;
inline constexpr float kMenuCheckBrightLevel = 110.0f / 255.0f;
inline constexpr float kMenuCheckOverviewBright = 0.2f;
inline constexpr int32_t kMenuCheckSampleStep = 2;

std::string menu_row_label(const UiScreenSlot &p_slot, size_t p_row);
std::optional<bf3::core::ui::SwfPoint> menu_row_stage_point(
		const UiScreenSlot &p_slot, size_t p_row, bool p_hit = true);

struct MenuCheckIo {
	std::function<void(godot::Key, bool)> press;
	std::function<void(godot::Vector2)> move;
	std::function<void(godot::Vector2, bool)> click;
	std::function<bool(const std::string &)> shot;
	std::function<godot::Ref<godot::Image>()> capture;
	bool alive = false;
	bool game_input = true;
	bool cursor_shown = false;
	bool hud_visible = true;
	std::string overview;
	uint32_t overview_icons = 0;
};

class MenuCheck {
public:
	explicit MenuCheck(std::string p_shot_dir) : shot_dir_(std::move(p_shot_dir)) {}
	bool frame(Bf3UiScreens &r_screens, NativeFlow &r_flow, const NativeMenu &p_menu, double p_now_s,
			const MenuCheckIo &p_io);
	std::string report() const;
	int exit_code() const { return passed_ ? 0 : 1; }

private:
	struct Ctx {
		Bf3UiScreens &screens;
		NativeFlow &flow;
		const NativeMenu &menu;
		const MenuCheckIo &io;
	};
	struct Step {
		const char *name;
		std::function<bool(MenuCheck &, const Ctx &)> ready;
		std::function<void(MenuCheck &, const Ctx &)> act;
		const char *shot = nullptr;
		bool repeat_before = false;
		double seen_s = -1.0;
		uint32_t repeats = 0;
	};
	static std::vector<Step> steps();
	static std::string state_text(const Ctx &p_c, bool p_menu);
	static std::string hud_screens_text(const Ctx &p_c);
	static bool shown(const Ctx &p_c, std::string_view p_partition);
	static std::string row_state(const Ctx &p_c, size_t p_row);
	static std::optional<godot::Vector2> row_point(const Ctx &p_c, size_t p_row, bool p_hit = true);
	static std::optional<godot::Rect2> overview_rect(const Ctx &p_c);
	void check_overview_drawn(const Ctx &p_c);
	static void tap(const Ctx &p_c, godot::Key p_key);
	void click_row(const Ctx &p_c, size_t p_row, bool p_hit = true);

	std::string shot_dir_;
	std::string overview_;
	std::string overview_drawn_;
	std::string waiting_;
	std::vector<std::string> shots_;
	std::vector<Step> steps_ = steps();
	size_t next_ = 0;
	bool acted_ = false;
	double start_s_ = -1.0;
	double last_s_ = -1.0;
	uint64_t glitches_at_open_ = 0;
	bool done_ = false;
	bool passed_ = false;
	std::string failure_;
};

}
