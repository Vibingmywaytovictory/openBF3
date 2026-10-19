// M2b ui: the pre-round (pre_round.h).
#include "pre_round.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;

void Bf3TdmPreRound::update_screen() {
	const ui::HudPreRound &p = ctx_->model.pre_round;
	// The round end and the scoreboard cover the top centre; the pre-round never shows under them.
	const bool v = p.visible && !ctx_->model.end_of_round.visible;
	ctx_->records.pre_round_visible = v;
	show_if(v);
}

void Bf3TdmPreRound::draw_screen() {
	TdmHudContext &c = *ctx_;
	const ui::HudPreRound &p = c.model.pre_round;
	const float s = tdm_scale(*this);
	const Vector2 tl = tdm_at(*this, ui::HudAnchor::Top, kPreRoundCentreX - 0.5f * kPreRoundWidth, kPreRoundTop);
	const Color edge = p.input_restricted ? kTdmEnemyColour : kTdmLineColour;
	tdm_draw_panel(*this, Rect2(tl, Vector2(kPreRoundWidth * s, kPreRoundHeight * s)), kTdmPanelColour, edge, std::max(1.0f, 2.0f * s));
	const auto at = [&](float p_y) { return tdm_at(*this, ui::HudAnchor::Top, kPreRoundCentreX, kPreRoundTop + p_y); };
	if (p.counting_down) {
		tdm_draw_text(*this, c.font(TdmFont::Text), at(26.0f), c.text(TdmText::RoundWillStartIn), tdm_px(15.0f, s), kTdmTextColour, 2);
		const int secs = int(std::ceil(std::max(p.seconds_left, 0.0f)));
		tdm_draw_text(*this, c.font(TdmFont::Number), at(78.0f), std::to_string(secs), tdm_px(44.0f, s), kTdmLocalColour, 2);
	} else {
		tdm_draw_text(*this, c.font(TdmFont::Title), at(56.0f), c.text(TdmText::WaitingForPlayers), tdm_px(20.0f, s), kTdmTextColour, 2);
	}
}

} // namespace bf3::godot_side
