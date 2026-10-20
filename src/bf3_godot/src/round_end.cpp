#include "round_end.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;

void Bf3TdmRoundEnd::update_screen() {
	const bool v = ctx_->model.end_of_round.visible;
	ctx_->records.round_end_visible = v;
	if (v && !was_visible_) {
		++ctx_->records.round_end_shown;
	}
	was_visible_ = v;
	show_if(v);
}

void Bf3TdmRoundEnd::draw_screen() {
	TdmHudContext &c = *ctx_;
	const ui::HudModel &m = c.model;
	const ui::HudEndOfRound &e = m.end_of_round;
	const float s = tdm_scale(*this);
	draw_rect(Rect2(Vector2(), get_size()), kTdmDimBackColour, true);
	const Vector2 tl = tdm_at(*this, ui::HudAnchor::Centre, kRoundEndLeft, kRoundEndTop);
	tdm_draw_panel(*this, Rect2(tl, Vector2(kRoundEndWidth * s, kRoundEndHeight * s)), kTdmPanelColour, kTdmLineColour, std::max(1.0f, s));
	const auto at = [&](float p_x, float p_y) { return tdm_at(*this, ui::HudAnchor::Centre, p_x, p_y); };
	const Ref<Font> title = c.font(TdmFont::Title);
	const Ref<Font> text = c.font(TdmFont::Text);
	const Ref<Font> num = c.font(TdmFont::Number);
	const float cx = kRoundEndLeft + 0.5f * kRoundEndWidth;
	const int own = m.local_team;

	if (own == 1 || own == 2) {
		const bool won = e.local_won;
		tdm_draw_text(*this, title, at(cx, kRoundEndTop + 60.0f), c.text(won ? TdmText::TeamWon : TdmText::TeamLost), tdm_px(36.0f, s),
				won ? kTdmFriendlyColour : kTdmEnemyColour, 2);
	} else if (e.winner == 1 || e.winner == 2) {
		const std::string line = c.text(TdmText::Winner) + "  " + c.text(e.winner == 1 ? TdmText::TeamUs : TdmText::TeamRu);
		tdm_draw_text(*this, title, at(cx, kRoundEndTop + 60.0f), line, tdm_px(36.0f, s), kTdmTextColour, 2);
	}

	const int friendly = own == 2 ? 2 : 1;
	const int enemy = 3 - friendly;
	const auto side = [&](int p_team, float p_x, const Color &p_col) {
		tdm_draw_text(*this, text, at(p_x, kRoundEndTop + 110.0f), c.text(p_team == 1 ? TdmText::TeamUs : TdmText::TeamRu), tdm_px(16.0f, s), p_col, 2);
		tdm_draw_text(*this, num, at(p_x, kRoundEndTop + 168.0f), std::to_string(e.team_kills[size_t(p_team - 1)]), tdm_px(52.0f, s), p_col, 2);
		if (e.winner == p_team) {
			tdm_draw_text(*this, title, at(p_x, kRoundEndTop + 194.0f), c.text(TdmText::Winner), tdm_px(13.0f, s), kTdmLocalColour, 2);
		}
	};
	side(friendly, cx - 150.0f, kTdmFriendlyColour);
	side(enemy, cx + 150.0f, kTdmEnemyColour);
	if (e.kill_limit > 0) {
		tdm_draw_text(*this, num, at(cx, kRoundEndTop + 158.0f), "/ " + std::to_string(e.kill_limit), tdm_px(18.0f, s), kTdmDimColour, 2);
	}

	for (size_t i = 0; i < e.top_players.size(); ++i) {
		const uint8_t slot = e.top_players[i];
		if (slot >= ui::kHudMaxPlayers) {
			continue;
		}
		const float y = kRoundEndTop + 240.0f + float(i) * 30.0f;
		tdm_draw_text(*this, num, at(kRoundEndLeft + 150.0f, y), std::to_string(i + 1), tdm_px(18.0f, s), kTdmDimColour, 1);
		tdm_draw_text(*this, text, at(kRoundEndLeft + 170.0f, y), tdm_player_name(m, slot), tdm_px(18.0f, s), tdm_player_colour(m, slot));
		tdm_draw_text(*this, num, at(kRoundEndLeft + kRoundEndWidth - 150.0f, y), std::to_string(m.players[slot].score), tdm_px(18.0f, s), kTdmTextColour, 1);
	}
	if (e.seconds_to_next_round > 0.0f) {
		const std::string line = c.text(TdmText::NextRoundIn) + " " + std::to_string(int(std::ceil(e.seconds_to_next_round)));
		tdm_draw_text(*this, text, at(cx, kRoundEndTop + kRoundEndHeight - 22.0f), line, tdm_px(16.0f, s), kTdmTextColour, 2);
	}
}

}
