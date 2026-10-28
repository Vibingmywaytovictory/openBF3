#include "native_eor.h"

#include "native_screens.h"
#include "ui_cursor.h"
#include "ui_post_process.h"

#include "bf3/core/ui/hud_widgets.h"

#include <cstdlib>

namespace bf3::godot_side {

namespace ui = bf3::core::ui;

namespace {

std::string rows_signature(const std::vector<ui::As2Value> &p_rows) {
	std::string out;
	for (const ui::As2Value &r : p_rows) {
		for (const std::string_view f : { "Index", "Label", "Kills", "Deaths", "Score", "Ping", "LocalPlayer" }) {
			out += r.field(f).to_string();
			out += '\t';
		}
		out += '\n';
	}
	return out;
}

}

void NativeEor::build(Bf3UiScreens &r_screens) {
	for (const std::string_view p : kNativeEorScreens) {
		r_screens.set_drawn(p, true);
	}
}

bool NativeEor::take(const ui::UiFlowAction &p_action) {
	if (!p_action.graph.starts_with(kEorGraphPrefix)) {
		return false;
	}
	const std::string_view n = p_action.name;
	if (n != kEorMouseInputAction && n != kEorGameInputAction && n != kEorCameraChangedAction &&
			n != kEorQuitGameAction && n != kEorShutDownAction) {
		return false;
	}
	++stats_.actions;
	if (pending_.size() >= kNativeEorMaxPending) {
		++stats_.dropped;
		return true;
	}
	pending_.push_back(p_action);
	return true;
}

void NativeEor::apply(const ui::UiFlowAction &p_action, const NativeEorIo &p_io) {
	const std::string_view n = p_action.name;
	if (n == kEorMouseInputAction) {
		mouse_input_ = ui::ui_flow_action_truthy(p_action);
	} else if (n == kEorGameInputAction) {
		game_input_ = ui::ui_flow_action_truthy(p_action);
	} else if (n == kEorCameraChangedAction) {
		if (Bf3UiPostProcess *post = Bf3UiPostProcess::ensure(p_io.parent, p_io.level)) {
			post->trigger_glitch(ui::ui_flow_action_float(p_action).value_or(0.0f));
			++stats_.glitches;
		}
	} else if (n == kEorQuitGameAction || n == kEorShutDownAction) {
		++stats_.quits;
		quit_requested_ = true;
	}
}

void NativeEor::feed_rows(Bf3UiScreens &r_screens, const ui::HudModel &p_model) {
	const std::vector<ui::As2Value> friendly = ui::hud_scoreboard_rows(p_model, p_model.team.friendly_team_players);
	const std::vector<ui::As2Value> enemy = ui::hud_scoreboard_rows(p_model, p_model.team.enemy_team1_players);
	const std::string signature = rows_signature(friendly) + "|" + rows_signature(enemy);
	for (size_t i = 0; i < kNativeEorScoreboards.size(); ++i) {
		ui::HudScreen *s = r_screens.hud_screen(kNativeEorScoreboards[i]);
		Fed &fed = fed_[i];
		if (s == nullptr) {
			fed = Fed{};
			continue;
		}
		if (s == fed.screen && signature == fed.signature) {
			continue;
		}
		ui::hud_list_set_direct_rows(*s, "List_01", friendly);
		ui::hud_list_set_direct_rows(*s, "List_02", enemy);
		fed.screen = s;
		fed.signature = signature;
		++stats_.row_feeds;
	}
}

void NativeEor::show_cursor(const NativeEorIo &p_io, bool p_shown) {
	if (p_shown == cursor_shown_) {
		return;
	}
	Bf3UiCursor *cursor = Bf3UiCursor::ensure(p_io.parent);
	if (cursor == nullptr) {
		return;
	}
	cursor_shown_ = p_shown;
	cursor->set_follow_mouse(p_shown);
	if (p_shown) {
		cursor->show_cursor();
		++stats_.cursor_shown;
	} else {
		cursor->hide_cursor();
	}
}

void NativeEor::frame(Bf3UiScreens &r_screens, const ui::HudModel &p_model, const NativeEorIo &p_io) {
	std::vector<ui::UiFlowAction> pending;
	pending.swap(pending_);
	for (const ui::UiFlowAction &a : pending) {
		apply(a, p_io);
	}
	const bool over = p_model.end_of_round.visible;
	if (round_over_ && !over) {
		mouse_input_ = false;
		game_input_ = true;
	}
	round_over_ = over;
	feed_rows(r_screens, p_model);
	show_cursor(p_io, wants_mouse());
}

}
