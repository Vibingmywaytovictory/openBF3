#include "hud_feed_tdm_parts.h"

#include "bf3/core/ui/text_db.h"

namespace bf3::host::tdm_feed {

namespace ui = bf3::core::ui;

static_assert(ui::kHudAccessorySlots == kLoadoutAccessories);
static_assert(kHudMenuSids.size() == size_t(ui::HudMenuAction::Quit) + 1);
static_assert(kHudMenuSids.size() <= ui::kHudMenuEntries);

void feed_menu(Frame &r_frame) {
	const HudFeedInput &in = r_frame.input;
	ui::HudModel &model = r_frame.model;
	const bool alive = in.alive && !in.own_unspawned;
	const bool fighting = r_frame.phase == net::MatchPhase::PreRound || r_frame.phase == net::MatchPhase::InRound;
	ui::HudMenuList &list = model.ingame_menu.menu_entries;
	list = ui::HudMenuList();
	for (size_t i = 0; i < kHudMenuSids.size(); ++i) {
		ui::HudMenuEntry &e = list.entries[list.count++];
		e.label_sid_hash = ui::text_hash(kHudMenuSids[i]);
		e.action = ui::HudMenuAction(i);
		switch (e.action) {
			case ui::HudMenuAction::Suicide:
				e.enabled = r_frame.match && alive && fighting;
				break;
			case ui::HudMenuAction::Options:
				e.enabled = false;
				break;
			default:
				e.enabled = true;
				break;
		}
	}

	ui::HudUser &user = model.user;
	user.user_name = ui::HudPlayerName{};
	if (r_frame.own_slot < ui::kHudMaxPlayers && r_frame.present[r_frame.own_slot]) {
		user.user_name = model.players[r_frame.own_slot].name;
	}
	user.is_ingame = r_frame.match;
	user.is_in_menu = in.in_menu;

	ui::HudSettings &settings = model.settings;
	hud_text(settings.current_game_mode, in.game_mode);
	settings.current_game_mode_name = in.tdm ? ui::text_hash(kHudTdmModeSid) : 0u;
	settings.is_pc = true;
	model.level.current_level = in.level_name_sid_hash;
}

}
