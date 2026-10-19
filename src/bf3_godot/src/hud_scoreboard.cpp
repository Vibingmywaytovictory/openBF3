// M2b ui: the scoreboard (hud_scoreboard.h).
#include "hud_scoreboard.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;

void Bf3TdmScoreboard::update_screen() {
	const bool v = ctx_->model.team.scoreboard_enabled;
	ctx_->records.scoreboard_visible = v;
	if (!v) {
		ctx_->records.scoreboard_rows = 0;
	}
	show_if(v);
}

void Bf3TdmScoreboard::draw_screen() {
	TdmHudContext &c = *ctx_;
	const ui::HudModel &m = c.model;
	const float s = tdm_scale(*this);
	const Vector2 tl = tdm_at(*this, ui::HudAnchor::Centre, kScoreboardLeft, kScoreboardTop);
	const Rect2 panel(tl, Vector2(kScoreboardWidth * s, kScoreboardHeight * s));
	tdm_draw_panel(*this, panel, kTdmPanelColour, kTdmLineColour, std::max(1.0f, s));
	const int friendly = m.team.friendly_team != 0 ? m.team.friendly_team : 1;
	const int enemy = 3 - friendly;
	const int32_t kills_f = friendly == 1 ? m.kill_counter.team1_kills : m.kill_counter.team2_kills;
	const int32_t kills_e = friendly == 1 ? m.kill_counter.team2_kills : m.kill_counter.team1_kills;
	const float col_w = 0.5f * (kScoreboardWidth - 30.0f);
	uint8_t rows = 0;
	rows = uint8_t(rows + draw_team(m.team.friendly_team_players, friendly, kills_f, kScoreboardLeft + 10.0f, col_w, true));
	rows = uint8_t(rows + draw_team(m.team.enemy_team1_players, enemy, kills_e, kScoreboardLeft + 20.0f + col_w, col_w, false));
	c.records.scoreboard_rows = rows;
	c.records.scoreboard_team_kills = { m.kill_counter.team1_kills, m.kill_counter.team2_kills };
	// The kill limit, centred in the header.
	if (m.kill_counter.team1_max_kills > 0) {
		const Vector2 at = tdm_at(*this, ui::HudAnchor::Centre, 640.0f, kScoreboardTop + 40.0f);
		tdm_draw_text(*this, c.font(TdmFont::Number), at, "/ " + std::to_string(m.kill_counter.team1_max_kills), tdm_px(16.0f, s), kTdmDimColour, 2);
	}
}

uint8_t Bf3TdmScoreboard::draw_team(const ui::HudPlayerList &p_list, int p_team, int32_t p_kills, float p_left, float p_width, bool p_friendly) {
	TdmHudContext &c = *ctx_;
	const ui::HudModel &m = c.model;
	const float s = tdm_scale(*this);
	const Color team_col = p_friendly ? kTdmFriendlyColour : kTdmEnemyColour;
	const Ref<Font> title = c.font(TdmFont::Title);
	const Ref<Font> text = c.font(TdmFont::Text);
	const Ref<Font> num = c.font(TdmFont::Number);
	// Header: the team's name at the outer edge, its kills towards the centre (the limit sits between the two counts).
	const bool right_side = !p_friendly; // friendly on the left
	const Vector2 head = tdm_at(*this, ui::HudAnchor::Centre, right_side ? p_left + p_width - 8.0f : p_left + 8.0f, kScoreboardTop + 34.0f);
	tdm_draw_text(*this, title, head, c.text(p_team == 1 ? TdmText::TeamUs : TdmText::TeamRu), tdm_px(24.0f, s), team_col, right_side ? 1 : 0);
	const Vector2 kills_at = tdm_at(*this, ui::HudAnchor::Centre, right_side ? p_left + 36.0f : p_left + p_width - 36.0f, kScoreboardTop + 40.0f);
	tdm_draw_text(*this, num, kills_at, std::to_string(p_kills), tdm_px(32.0f, s), team_col, right_side ? 0 : 1);
	// Columns: name | K | D | SCORE | PING (right edges, stage pixels from the column's left).
	const float x_k = p_width * 0.62f;
	const float x_d = p_width * 0.72f;
	const float x_score = p_width * 0.87f;
	const float x_ping = p_width - 8.0f;
	const float head_y = kScoreboardTop + kScoreboardHeaderHeight;
	const int32_t hs = tdm_px(12.0f, s);
	const auto col_head = [&](float p_x, TdmText p_key, int p_align) {
		tdm_draw_text(*this, text, tdm_at(*this, ui::HudAnchor::Centre, p_left + p_x, head_y), c.text(p_key), hs, kTdmDimColour, p_align);
	};
	col_head(40.0f, TdmText::Name, 0);
	col_head(x_k, TdmText::Kills, 1);
	col_head(x_d, TdmText::Deaths, 1);
	col_head(x_score, TdmText::Score, 1);
	col_head(x_ping, TdmText::Ping, 1);
	const Vector2 l0 = tdm_at(*this, ui::HudAnchor::Centre, p_left, head_y + 4.0f);
	draw_line(l0, l0 + Vector2(p_width * s, 0.0f), kTdmLineColour, std::max(1.0f, s));
	// Rows.
	const size_t n = std::min<size_t>(p_list.count, p_list.index.size());
	const float avail = kScoreboardHeight - kScoreboardHeaderHeight - 16.0f;
	const float row_h = std::clamp(avail / float(std::max<size_t>(n, 16)), kScoreboardRowMin, kScoreboardRowMax);
	const int32_t rs = tdm_px(std::min(14.0f, row_h * 0.62f), s);
	const Ref<Texture2D> dead = c.dead_icon();
	uint8_t drawn = 0;
	for (size_t i = 0; i < n; ++i) {
		const uint8_t slot = p_list.index[i];
		if (slot >= ui::kHudMaxPlayers) {
			continue;
		}
		const ui::HudPlayer &p = m.players[slot];
		const float top = head_y + 8.0f + float(i) * row_h;
		if (top + row_h > kScoreboardTop + kScoreboardHeight) {
			break;
		}
		const Vector2 row_tl = tdm_at(*this, ui::HudAnchor::Centre, p_left, top);
		const Vector2 row_size(p_width * s, row_h * s);
		if (slot == m.local_player) {
			Color hl = kTdmLocalColour;
			hl.a = 0.18f;
			draw_rect(Rect2(row_tl, row_size), hl, true);
		} else if ((i & 1) != 0) {
			draw_rect(Rect2(row_tl, row_size), Color(1.0f, 1.0f, 1.0f, 0.04f), true);
		}
		const float base_stage = top + 0.5f * row_h + 0.35f * float(rs) / std::max(s, 1e-3f);
		const auto at = [&](float p_x) { return tdm_at(*this, ui::HudAnchor::Centre, p_left + p_x, base_stage); };
		const Color name_col = slot == m.local_player ? kTdmLocalColour : (p.alive ? kTdmTextColour : kTdmDimColour);
		tdm_draw_text(*this, num, at(24.0f), std::to_string(i + 1), rs, kTdmDimColour, 1);
		if (!p.alive && dead.is_valid()) {
			const float isz = std::min(row_h - 2.0f, 16.0f) * s;
			draw_texture_rect(dead, Rect2(tdm_at(*this, ui::HudAnchor::Centre, p_left + 28.0f, top + 0.5f * row_h) - Vector2(0.0f, 0.5f * isz),
											  Vector2(isz, isz)),
					false, Color(1.0f, 1.0f, 1.0f, 0.8f));
		}
		const float nw = tdm_draw_text(*this, text, at(46.0f), tdm_player_name(m, slot), rs, name_col);
		if (p.bot) {
			const Vector2 b = at(46.0f) + Vector2(nw + 6.0f * s, 0.0f);
			tdm_draw_text(*this, text, b, std::string(kTdmOwnTexts.bot), tdm_px(9.0f, s), kTdmDimColour);
		}
		tdm_draw_text(*this, num, at(x_k), std::to_string(p.kills), rs, kTdmTextColour, 1);
		tdm_draw_text(*this, num, at(x_d), std::to_string(p.deaths), rs, kTdmTextColour, 1);
		tdm_draw_text(*this, num, at(x_score), std::to_string(p.score), rs, kTdmTextColour, 1);
		tdm_draw_text(*this, num, at(x_ping), p.bot ? std::string("-") : std::to_string(p.ping_ms), rs, kTdmDimColour, 1);
		++drawn;
	}
	return drawn;
}

} // namespace bf3::godot_side
