#pragma once

#include "bf3/core/ui/hud_model.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>

namespace godot {
class Node;
}

namespace bf3::godot_side {

class Bf3Localisation;
class Bf3UiAssets;

struct TdmHudRecords {
	uint64_t model_serial = 0;
	uint64_t draws = 0;

	uint8_t kill_feed_lines = 0;
	uint32_t kill_feed_newest_serial = 0;
	uint64_t kill_feed_lines_started = 0;

	bool scoreboard_visible = false;
	uint8_t scoreboard_rows = 0;
	std::array<int32_t, 2> scoreboard_team_kills{};

	bool spawn_screen_visible = false;
	bool deploy_enabled = false;
	uint8_t selected_kit = 0;
	uint64_t spawn_screen_shown = 0;

	bool kill_card_visible = false;
	uint32_t kill_card_serial = 0;
	core::ui::HudPlayerName kill_card_killer{};
	uint32_t kill_card_item = 0;
	bool kill_card_weapon_named = false;

	bool pre_round_visible = false;
	bool round_end_visible = false;
	uint64_t round_end_shown = 0;

	uint64_t missing_fonts = 0;
	uint64_t missing_icons = 0;
	uint64_t missing_strings = 0;
};
static_assert(std::is_trivially_copyable_v<TdmHudRecords>);

class Bf3TdmHud {
public:
	Bf3TdmHud();
	~Bf3TdmHud();
	Bf3TdmHud(const Bf3TdmHud &) = delete;
	Bf3TdmHud &operator=(const Bf3TdmHud &) = delete;

	bool build(godot::Node *p_parent, Bf3Localisation *p_localisation, Bf3UiAssets *p_ui_assets, std::string *r_error);
	bool built() const;

	void set_model(const core::ui::HudModel &p_model);

	int take_kit_click();
	bool take_deploy_click();

	bool wants_mouse() const;
	TdmHudRecords records() const;
	void shutdown();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}
