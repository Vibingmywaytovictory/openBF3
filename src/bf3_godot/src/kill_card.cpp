#include "kill_card.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;

void Bf3TdmKillCard::update_screen() {
	const ui::HudKillCard &k = ctx_->model.kill_card;
	ctx_->records.kill_card_visible = k.visible;
	show_if(k.visible);
}

void Bf3TdmKillCard::draw_screen() {
	TdmHudContext &c = *ctx_;
	const ui::HudModel &m = c.model;
	const ui::HudKillCard &k = m.kill_card;
	TdmHudRecords &rec = c.records;
	rec.kill_card_serial = k.serial;
	rec.kill_card_killer = {};
	rec.kill_card_item = 0;
	rec.kill_card_weapon_named = false;
	const float s = tdm_scale(*this);
	const auto at = [&](float p_x, float p_y) { return tdm_at(*this, ui::HudAnchor::Bottom, p_x, p_y); };
	const Vector2 tl = at(kKillCardLeft, kKillCardTop);
	tdm_draw_panel(*this, Rect2(tl, Vector2(kKillCardWidth * s, kKillCardHeight * s)), kTdmPanelColour, kTdmLineColour, std::max(1.0f, s));
	draw_rect(Rect2(tl, Vector2(4.0f * s, kKillCardHeight * s)), kTdmEnemyColour, true);
	const Ref<Font> title = c.font(TdmFont::Title);
	const Ref<Font> text = c.font(TdmFont::Text);
	const Ref<Font> num = c.font(TdmFont::Number);
	const float x0 = kKillCardLeft + 18.0f;
	const float x1 = kKillCardLeft + kKillCardWidth - 16.0f;
	if (k.suicide || k.killer == ui::kHudNoPlayer) {
		tdm_draw_text(*this, title, at(x0, kKillCardTop + 46.0f), c.text(TdmText::Suicide), tdm_px(22.0f, s), kTdmTextColour);
		return;
	}
	tdm_draw_text(*this, text, at(x0, kKillCardTop + 22.0f), c.text(TdmText::KilledBy), tdm_px(12.0f, s), kTdmDimColour);
	const std::string killer = tdm_player_name(m, k.killer);
	tdm_draw_text(*this, title, at(x0, kKillCardTop + 50.0f), killer, tdm_px(26.0f, s), tdm_player_colour(m, k.killer));
	std::memcpy(rec.kill_card_killer.data(), killer.data(), std::min(killer.size(), rec.kill_card_killer.size() - 1));

	if (k.killer_kit >= 0 && size_t(k.killer_kit) < kTdmKitNames.size()) {
		const Ref<Texture2D> kit = c.kit_icon(k.killer_kit);
		float kx = x1;
		const std::string &kit_name = c.text(kTdmKitNames[size_t(k.killer_kit)]);
		const float w = tdm_draw_text(*this, text, at(kx, kKillCardTop + 24.0f), kit_name, tdm_px(13.0f, s), kTdmTextColour, 1);
		if (kit.is_valid()) {
			const float isz = 22.0f * s;
			const Vector2 p = at(kx, kKillCardTop + 9.0f) - Vector2(w + 6.0f * s + isz, 0.0f);
			draw_texture_rect(kit, Rect2(p, Vector2(isz, isz)), false);
		}
	}

	const Ref<Texture2D> icon = c.weapon_icon(k.item_id);
	if (icon.is_valid()) {
		draw_texture_rect(icon, Rect2(at(x0, kKillCardTop + 64.0f), Vector2(128.0f * s, 32.0f * s)), false);
		rec.kill_card_item = k.item_id;
	}
	const std::string &weapon = c.localised(k.weapon_name_sid_hash);
	if (!weapon.empty()) {
		tdm_draw_text(*this, text, at(x0, kKillCardTop + 116.0f), weapon, tdm_px(15.0f, s), kTdmTextColour);
		rec.kill_card_weapon_named = true;
	}
	if (k.headshot) {
		tdm_draw_text(*this, title, at(x0, kKillCardTop + 138.0f), c.text(TdmText::Headshot), tdm_px(12.0f, s), kTdmLocalColour);
	}

	tdm_draw_text(*this, text, at(x1, kKillCardTop + 74.0f), c.text(TdmText::Health), tdm_px(11.0f, s), kTdmDimColour, 1);
	tdm_draw_text(*this, num, at(x1, kKillCardTop + 104.0f), std::to_string(int(std::lround(k.killer_health))), tdm_px(28.0f, s), kTdmTextColour, 1);
	if (k.distance_m >= 0.0f) {
		const std::string d = std::to_string(int(std::lround(k.distance_m))) + " " + std::string(kTdmOwnTexts.metres);
		tdm_draw_text(*this, num, at(x1, kKillCardTop + 136.0f), d, tdm_px(16.0f, s), kTdmDimColour, 1);
	}
}

}
